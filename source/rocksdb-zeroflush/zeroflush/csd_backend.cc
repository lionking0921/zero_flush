//  Copyright (c) 2026, ZeroFlush-RocksDB.
//  ZeroFlush F-2：CSD 后端协议/资格/会话 —— csd_backend.h 的实现。
//
//  字节协议逐字同源 CPU-sim/真机 host（AcceleratorKernelSstV2/host/zf_format.h：
//  BuildZfRecord / Port::Staged / decoder_sst 链布局）与 kernel 解码器
//  （krnl_vadd.cpp decoder/decoder_sst）。本 TU 只含纯打包/装配 + 进程级会话
//  工厂注册；XRT/OpenCL 设备实现（csd_session_opencl.cc）单独翻译单元，仅链接
//  进带设备的测试/驱动目标，引擎 lib 恒不依赖 XRT。

#include "zeroflush/csd_backend.h"

#include <cstring>
#include <mutex>
#include <utility>

#include "rocksdb/env.h"
#include "db/dbformat.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "zeroflush/partition_index.h"
#include "zeroflush/wal_format.h"
#include "zeroflush/zf_seal.h"

namespace zeroflush {

ROCKSDB_NAMESPACE::Status ReadSealedWal(ROCKSDB_NAMESPACE::Env* env,
                                        const std::string& wal_dir,
                                        uint32_t part, uint32_t gen,
                                        std::string* bytes) {
  // 与 wal_manager.cc / SealedGenBuffer::Load 同文件名约定。
  const std::string fname = wal_dir + "/zf-wal-" + std::to_string(part) + "-" +
                            std::to_string(gen) + ".log";
  return ROCKSDB_NAMESPACE::ReadFileToString(env, fname, bytes);
}

bool BuildCsdSlotA(ROCKSDB_NAMESPACE::Env* env, const std::string& wal_dir,
                   const std::shared_ptr<PartitionIndex>& idx, uint32_t part,
                   uint32_t gen, ZfCsdSlot* out) {
  if (!idx) {
    return false;
  }
  std::string wal;
  ROCKSDB_NAMESPACE::Status s = ReadSealedWal(env, wal_dir, part, gen, &wal);
  if (!s.ok()) {
    return false;
  }
  if (wal.size() > kCsdMaxStagedBytes) {
    return false;  // WAL 段超出经验上限 → 回落 host
  }
  out->file_size = wal.size();
  out->kv = 0;
  out->kind = 0;

  // ---- 遍历冻结 slim 索引：先做逐条完整性/界校验，全部通过才落 slim 区 ----
  // 与 DrainPartitionAside 同构但用整段 WAL 字节直读（本模块自己就是字节源，
  // 无需再经 SealedGenBuffer 建 value 索引）。任一条不符 = 打包失败（不产生
  // 半成品槽）。slim 区先独立累积到 std::string，再整段拼接 WAL 区。
  std::string slim;
  slim.reserve(64 * 1024);
  bool ok = true;
  uint64_t kv = 0;
  const char* wbase = wal.data();
  const uint64_t wlen = wal.size();
  idx->ForEachEntry([&](const ROCKSDB_NAMESPACE::Slice& ik,
                        const ROCKSDB_NAMESPACE::Slice& locator) {
    if (!ok) {
      return;  // 已失败，仅继续走完枚举（避免 ForEachEntry 中途退出）
    }
    if (ik.size() < 8) {
      ok = false;
      return;
    }
    const uint64_t user_len = ik.size() - 8;
    if (user_len == 0 || user_len > kCsdMaxUserKeyBytes) {
      ok = false;  // 用户键超 24B（ik>32B），或空键
      return;
    }
    if (locator.size() != sizeof(SlimLocator)) {
      ok = false;
      return;
    }
    SlimLocator loc;
    std::memcpy(&loc, locator.data(), sizeof(loc));
    if (loc.part_id != part || loc.gen != gen) {
      ok = false;  // locator 代际不匹配（与 aside 完整性断言一致）
      return;
    }
    const uint64_t off = loc.wal_offset;
    if (off + kZfHeaderSize > wlen) {
      ok = false;  // 帧头越界
      return;
    }
    const char* d = wbase + off;
    if (ROCKSDB_NAMESPACE::DecodeFixed32(d) != kZfMagic) {
      ok = false;  // 非 ZF01 帧
      return;
    }
    const uint32_t klen = ROCKSDB_NAMESPACE::DecodeFixed32(d + 8);
    const uint32_t vlen = ROCKSDB_NAMESPACE::DecodeFixed32(d + 12);
    if (vlen > kCsdMaxValueBytes) {
      ok = false;  // 值 > 1024B（kernel 固定 2×512B 切片）
      return;
    }
    if (static_cast<uint64_t>(klen) != user_len) {
      ok = false;  // 帧 key_len != ik 用户键长（值定位 vbase 依赖 wkey_len）
      return;
    }
    if (off + kZfHeaderSize + klen + vlen > wlen) {
      ok = false;  // 帧体越界
      return;
    }
    // 该条合格：写 slim 条目 = varint ik_len | ik | varint loc_len | locator16
    ROCKSDB_NAMESPACE::PutVarint32(
        &slim, static_cast<uint32_t>(ik.size()));
    slim.append(ik.data(), ik.size());
    ROCKSDB_NAMESPACE::PutVarint32(
        &slim, static_cast<uint32_t>(sizeof(SlimLocator)));
    // locator 显式 LE 编码（part u32 | gen u32 | wal_offset u64），
    // 与 host zf_format.h 的 PutFixed32LE/64LE 同布局。
    ROCKSDB_NAMESPACE::PutFixed32(&slim, loc.part_id);
    ROCKSDB_NAMESPACE::PutFixed32(&slim, loc.gen);
    ROCKSDB_NAMESPACE::PutFixed64(&slim, loc.wal_offset);
    ++kv;
  });
  if (!ok) {
    return false;
  }
  if (kv == 0) {
    return false;  // 空 run 不卸载（host 单文件契约）
  }
  if (kv > kCsdMaxRecords || slim.size() > kCsdMaxStagedBytes) {
    return false;
  }
  out->bytes.assign(wal.begin(), wal.end());
  out->bytes.insert(out->bytes.end(), slim.begin(), slim.end());
  out->kv = kv;
  return true;
}

bool BuildCsdSlotAFromSorted(uint32_t part, uint32_t gen,
                             const std::vector<std::string>& keys,
                             const std::vector<std::string>& values,
                             uint64_t* deletions, ZfCsdSlot* out) {
  if (deletions != nullptr) {
    *deletions = 0;
  }
  if (out == nullptr || keys.size() != values.size() || keys.empty()) {
    return false;
  }
  const size_t n = keys.size();
  if (n > kCsdMaxRecords) {
    return false;
  }
  // ---- 帧区（WAL 区）：按 keys 顺序重建 ZF01 帧；offset 顺序累加 ----
  // 布局与 zf_format.h BuildZfRecord 一致：24B 头(magic|cf_id2|type|flags|
  // key_len4|val_len4|seq8) + user + value + crc32c(header+body)。
  // kernel A decoder 只经 locator.wal_offset 读帧头 key_len/val_len 取 value，
  // 不校验 crc —— 但仍写引擎一致的 crc32c，保持字节协议逐字同源。
  std::string wal;
  wal.reserve(4096);
  for (size_t i = 0; i < n; ++i) {
    const std::string& ik = keys[i];
    if (ik.size() != 32) {
      return false;  // 只接受 user 恰 24B（ik 恰 32B，kernel 验证语料档位）
    }
    const std::string& value = values[i];
    if (value.size() > kCsdMaxValueBytes) {
      return false;  // 值 > 1024B（kernel 固定 2×512B 切片）
    }
    const uint64_t packed = ROCKSDB_NAMESPACE::DecodeFixed64(ik.data() + 24);
    const uint64_t seq = packed >> 8;
    const uint8_t type = static_cast<uint8_t>(packed & 0xFF);
    // 删除计数与引擎 builder.cc Add() 同口径（type==deletion 系）。
    if (deletions != nullptr &&
        (type == ROCKSDB_NAMESPACE::kTypeDeletion ||
         type == ROCKSDB_NAMESPACE::kTypeSingleDeletion ||
         type == ROCKSDB_NAMESPACE::kTypeDeletionWithTimestamp)) {
      ++(*deletions);
    }
    const size_t fr_off = wal.size();  // 本条帧首（= 帧区内 wal_offset）
    // 帧头 24B
    ROCKSDB_NAMESPACE::PutFixed32(&wal, kZfMagic);
    wal.push_back(static_cast<char>(part & 0xFF));        // cf_id 低
    wal.push_back(static_cast<char>((part >> 8) & 0xFF)); // cf_id 高
    wal.push_back(static_cast<char>(type));
    wal.push_back(static_cast<char>(0));                  // flags
    ROCKSDB_NAMESPACE::PutFixed32(&wal, 24);              // key_len == user 长
    ROCKSDB_NAMESPACE::PutFixed32(&wal, static_cast<uint32_t>(value.size()));
    ROCKSDB_NAMESPACE::PutFixed64(&wal, seq);
    wal.append(ik.data(), 24);                            // user key
    wal.append(value.data(), value.size());
    // crc32c 覆盖 header+body（不含 crc 自身）
    const uint32_t crc = ROCKSDB_NAMESPACE::crc32c::Value(
        wal.data() + fr_off, static_cast<size_t>(wal.size() - fr_off));
    ROCKSDB_NAMESPACE::PutFixed32(&wal, crc);
  }
  if (wal.size() > kCsdMaxStagedBytes) {
    return false;
  }

  // ---- slim 区：每条 (ik, locator16)；locator 的 wal_offset = 帧区顺序偏移 ----
  std::string slim;
  slim.reserve(n * 56);
  uint64_t off = 0;
  for (size_t i = 0; i < n; ++i) {
    const std::string& ik = keys[i];
    // ik_len varint + ik
    ROCKSDB_NAMESPACE::PutVarint32(&slim, static_cast<uint32_t>(ik.size()));
    slim.append(ik.data(), ik.size());
    // loc_len varint + locator 16B（part u32 | gen u32 | wal_offset u64，LE）
    ROCKSDB_NAMESPACE::PutVarint32(&slim, sizeof(SlimLocator));
    ROCKSDB_NAMESPACE::PutFixed32(&slim, part);
    ROCKSDB_NAMESPACE::PutFixed32(&slim, gen);
    ROCKSDB_NAMESPACE::PutFixed64(&slim, off);
    off += ZfRecordLength(24, static_cast<uint32_t>(values[i].size()));
  }

  out->kind = 0;
  out->kv = static_cast<uint64_t>(n);
  out->file_size = wal.size();
  out->bytes.clear();
  out->bytes.reserve(wal.size() + slim.size());
  out->bytes.insert(out->bytes.end(), wal.begin(), wal.end());
  out->bytes.insert(out->bytes.end(), slim.begin(), slim.end());
  return true;
}


ZfCsdSlot BuildCsdSlotB(
    const std::vector<std::pair<uint64_t, std::string>>& files) {
  ZfCsdSlot s;
  s.kind = 1;
  s.file_size = 0;
  s.kv = 0;
  if (files.empty() || files.size() > kCsdMaxChainFiles) {
    return s;  // 空/超限 → 空槽；由卸载编排点在调用前 gate（此处不发信号）
  }
  // 布局：[u64 K][K × {u64 file_off,u64 file_sz}][文件1 字节..文件K 字节]
  // file_off = 链内绝对偏移（kernel decoder_sst 按绝对偏移读文件区）。
  std::vector<uint8_t> buf;
  const uint64_t K = files.size();
  const uint64_t hdr = 8 + 16 * K;
  uint64_t total = hdr;
  std::vector<uint64_t> offs;
  offs.reserve(files.size());
  for (const auto& f : files) {
    offs.push_back(total);
    total += f.second.size();
  }
  buf.reserve(static_cast<size_t>(total));

  auto put_u64 = [&buf](uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
  };
  put_u64(K);
  for (uint64_t i = 0; i < K; ++i) {
    put_u64(offs[static_cast<size_t>(i)]);
    put_u64(files[static_cast<size_t>(i)].second.size());
  }
  for (const auto& f : files) {
    buf.insert(buf.end(), f.second.begin(), f.second.end());
  }
  s.bytes = std::move(buf);
  s.file_size = total;
  return s;
}

bool ZfCsdManifestFromPps(const uint64_t pps[512], uint64_t data_size,
                          uint64_t index_size,
                          ROCKSDB_NAMESPACE::ZfSealManifest* m) {
  if (m == nullptr || pps == nullptr) {
    return false;
  }
  // 单文件契约：首文件槽（pps[0..128)）必须有块与条目；无则不符。
  const uint64_t num_blocks = pps[0];
  const uint64_t num_entries = pps[1];
  if (num_blocks == 0 || num_entries == 0) {
    return false;
  }
  // pps[2] == data_size（磁盘 data 区字节）；若非 0 且与回读不一致 → 不符。
  if (pps[2] != 0 && pps[2] != data_size) {
    return false;
  }
  m->data_size = data_size;
  m->index_size = index_size;
  m->num_entries = num_entries;
  m->num_data_blocks = num_blocks;
  m->raw_key_size = pps[3];
  m->raw_value_size = pps[4];
  // pps[6]/[7] = 全体记录 footer 极小/极大值（seq<<8|type）；seq = 高 56 位。
  // kernel 无删除计数 → num_deletions 保持调用方传入（通常引擎 props 补位）。
  m->key_smallest_seqno = pps[6] >> 8;
  m->key_largest_seqno = pps[7] >> 8;
  return true;
}

// ---- 进程级会话工厂（默认无设备：未注册 → Create 返回 nullptr）----
namespace {
std::mutex g_csd_factory_mu;
ZfCsdSessionFactory* g_csd_factory = nullptr;  // 注册后不再释放（进程级生命周期）
}  // namespace

void RegisterZfCsdSessionFactory(ZfCsdSessionFactory f) {
  std::lock_guard<std::mutex> lock(g_csd_factory_mu);
  delete g_csd_factory;
  g_csd_factory = new ZfCsdSessionFactory(std::move(f));
}

std::shared_ptr<ZfCsdSession> CreateZfCsdSession(const ZeroFlushOptions& zfo) {
  ZfCsdSessionFactory* f = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_csd_factory_mu);
    f = g_csd_factory;
  }
  if (f == nullptr) {
    return nullptr;  // 无设备实现注册 → 设备不可用，回落 host
  }
  return (*f)(zfo);
}

}  // namespace zeroflush
