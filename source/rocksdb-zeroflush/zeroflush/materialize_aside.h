//  Copyright (c) 2026, ZeroFlush-RocksDB.
//  ZeroFlush M4.2：物化"协助排序模块"（拆分独立验证版，不接入系统路径）。
//
//  动机：ZfMaterializeJob::MaterializePartition/MaterializeMergePartition 顶部
//  目前对某分区的封存代 WAL「顺序整读后自行排序」（WalScanner 逐条读 → 收集
//  keys/values → std::sort），实测排序是物化耗时大头。但同一批条目的"每分区
//  slim 跳表"（PartitionIndex，M4.3+ 写路径与分区 WAL 同步写入）天然按
//  internal comparator 有序（user key 升、seq 降 == BuildTable/CompactionIterator
//  需要的序）。若用冻结 slim 索引的序直接产出 A 侧，排序即可免去。
//
//  本模块把这条"协助排序"逻辑**独立拆分**出来做等价性验证（见 tools/zf_test.cc
//  TestSlimIndexSortAssist），刻意**不改动任何系统读写/物化路径**（materialize_job、
//  flush_job、zeroflush_db 均不引用本头文件）：
//   - DrainPartitionAside()：免排序主路径（按冻结 slim 索引序 + D1 整段读缓冲
//     取 value，不调用任何 key 比较器）；
//   - BuildWalSortedAside()：参考路径（WalScanner 整读 + InternalKeyComparator
//     排序），复刻现物化逻辑语义，作为逐字节对拍参照物。
//  模块级断言证明"冻结索引序 == 同记录集按 comparator 排序的序（含非 bytewise
//  user comparator）"后，将来接入物化即可安全替换掉排序段（完整性校验失败时
//  回落 BuildWalSortedAside 对应原路径）。
//
//  header-only：不新增 .cc，不改动任何构建清单；仅在 include 的 TU 中实例化。
//  类型引用统一 ROCKSDB_NAMESPACE 前缀（= rocksdb）。

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/env.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "db/dbformat.h"  // InternalKeyComparator / PackSequenceAndType / GetInternalKeySeqno
#include "util/coding.h"  // PutFixed64 / DecodeFixed32 / DecodeFixed64
#include "zeroflush/partition_index.h"
#include "zeroflush/wal_format.h"   // ZF01 帧 / SlimLocator / DecodeZfRecord
#include "zeroflush/wal_manager.h"  // WalScanner

namespace zeroflush {

// D1 整段读缓冲：一次整段读 zf-wal-<part>-<gen>.log 进内存，逐帧顺序解码建立
// (offset→value) 索引。value 按记录起始字节偏移内存二分取回——物化免排序的
// value 源，绝不做按跳表序的逐条定点随机 IO。
// 与 WalScanner（块读/顺序扫）互为独立解析实现，供交叉验证。
class SealedGenBuffer {
 public:
  // 清空并整段读入 <dir>/zf-wal-<part>-<gen>.log（文件须为已封存/只读状态）。
  ROCKSDB_NAMESPACE::Status Load(ROCKSDB_NAMESPACE::Env* env,
                                 const std::string& dir, uint32_t part,
                                 uint32_t gen) {
    offsets_.clear();
    values_.clear();
    buf_.clear();
    status_ = ROCKSDB_NAMESPACE::Status::OK();
    const std::string fname = dir + "/zf-wal-" + std::to_string(part) + "-" +
                              std::to_string(gen) + ".log";
    status_ = ROCKSDB_NAMESPACE::ReadFileToString(env, fname, &buf_);
    if (!status_.ok()) {
      return status_;
    }
    size_t pos = 0;
    while (pos < buf_.size()) {
      // 头部解码出 key_len/val_len（与 WalScanner 的 DecodeKeyValLen 同布局）。
      if (buf_.size() - pos < kZfHeaderSize + kZfCrcSize) {
        status_ = ROCKSDB_NAMESPACE::Status::Corruption(
            "ZF D1 buffer: truncated record header at offset " +
            std::to_string(pos));
        break;
      }
      const char* d = buf_.data() + pos;
      if (ROCKSDB_NAMESPACE::DecodeFixed32(d) != kZfMagic) {
        status_ = ROCKSDB_NAMESPACE::Status::Corruption(
            "ZF D1 buffer: bad magic at offset " + std::to_string(pos));
        break;
      }
      const uint32_t key_len = ROCKSDB_NAMESPACE::DecodeFixed32(d + 8);
      const uint32_t val_len = ROCKSDB_NAMESPACE::DecodeFixed32(d + 12);
      const size_t total = ZfRecordLength(key_len, val_len);
      if (pos + total > buf_.size()) {
        status_ = ROCKSDB_NAMESPACE::Status::Corruption(
            "ZF D1 buffer: truncated record at offset " + std::to_string(pos));
        break;
      }
      ZfRecordHeader h;
      ROCKSDB_NAMESPACE::Slice key, value;
      // DecodeZfRecord 要求 len == 单条记录精确长度，且校验 magic + crc。
      status_ = DecodeZfRecord(d, total, &h, &key, &value);
      if (!status_.ok()) {
        break;
      }
      offsets_.push_back(pos);
      values_.emplace_back(value.data(), value.size());
      pos += total;
    }
    return status_;
  }

  uint64_t record_count() const { return offsets_.size(); }
  ROCKSDB_NAMESPACE::Status status() const { return status_; }
  const std::vector<uint64_t>& offsets() const { return offsets_; }

  // 按记录起始 offset 取 value（内存二分，无 IO）。未命中返回 false。
  bool GetValue(uint64_t offset, std::string* out) const {
    auto it = std::lower_bound(offsets_.begin(), offsets_.end(), offset);
    if (it == offsets_.end() || *it != offset) {
      return false;
    }
    *out = values_[static_cast<size_t>(it - offsets_.begin())];
    return true;
  }

 private:
  std::string buf_;
  std::vector<uint64_t> offsets_;  // 每帧起始偏移（单调升序）
  std::vector<std::string> values_;
  ROCKSDB_NAMESPACE::Status status_;
};

// 免排序 A 侧产出（核心"协助排序"逻辑）：
//   输入 = 某代封存 gen 的冻结 slim 索引（天然按 internal comparator 有序）
//          + 该 gen 封存 WAL 的 D1 整段缓冲；
//   输出 = 已排序 (internal key, value) 流（internal key 含 8B seq/type 尾），
//         以及该代最小 seq（min_seq，融合归并断言用，由 ik 尾解码）。
// 全程不调用任何 key 比较器 / 不做排序；value 经 locator.offset 从缓冲内存二分取回。
// 完整性校验：条目数 == buf.record_count()、locator (part,gen) 与期望一致、
// offset 全部命中——任一不符返回 Corruption（将来接入物化时 = 回落
// WalScanner+std::sort 原路径的信号；本模块保持原样不改动调用方）。
// M4.2 接入生产物化后由多个 TU（zf_test / materialize_job）include，须 inline
// 避免 ODR 重复定义。
inline ROCKSDB_NAMESPACE::Status DrainPartitionAside(
    const PartitionIndex& idx, const SealedGenBuffer& buf,
    uint32_t expect_part, uint32_t expect_gen,
    std::vector<std::string>* keys /*out: internal key*/,
    std::vector<std::string>* values /*out: 与 keys 对齐*/,
    uint64_t* min_seq /*out, 可为 nullptr*/) {
  keys->clear();
  values->clear();
  keys->reserve(static_cast<size_t>(buf.record_count()));
  values->reserve(static_cast<size_t>(buf.record_count()));
  uint64_t mn = std::numeric_limits<uint64_t>::max();
  bool integrity_ok = true;
  const std::string expect_msg = "part=" + std::to_string(expect_part) +
                                 " gen=" + std::to_string(expect_gen);
  idx.ForEachEntry([&](const ROCKSDB_NAMESPACE::Slice& ik,
                       const ROCKSDB_NAMESPACE::Slice& locator) {
    if (!integrity_ok) {
      return;  // 已失败，仅继续走完枚举（避免额外分支）
    }
    if (locator.size() != sizeof(SlimLocator)) {
      integrity_ok = false;
      return;
    }
    SlimLocator loc;
    std::memcpy(&loc, locator.data(), sizeof(loc));
    if (loc.part_id != expect_part || loc.gen != expect_gen) {
      integrity_ok = false;
      return;
    }
    std::string v;
    if (!buf.GetValue(loc.wal_offset, &v)) {
      integrity_ok = false;
      return;
    }
    keys->emplace_back(ik.data(), ik.size());
    values->push_back(std::move(v));
    const uint64_t seq = ROCKSDB_NAMESPACE::GetInternalKeySeqno(ik);
    if (seq < mn) {
      mn = seq;
    }
  });
  if (!integrity_ok) {
    return ROCKSDB_NAMESPACE::Status::Corruption(
        "ZF aside drain integrity mismatch: " + expect_msg);
  }
  if (keys->size() != static_cast<size_t>(buf.record_count())) {
    return ROCKSDB_NAMESPACE::Status::Corruption(
        "ZF aside drain count mismatch: idx=" + std::to_string(keys->size()) +
        " wal=" + std::to_string(buf.record_count()) + " " + expect_msg);
  }
  if (min_seq != nullptr) {
    *min_seq = mn;
  }
  return ROCKSDB_NAMESPACE::Status::OK();
}

// 参考路径（等价性对拍参照物 = 现 materialize_job 顶部"WAL 全局扫描排序"的
// 独立复刻）：
//   WalScanner 整读 <dir>/zf-wal-<part>-<gen>.log → internal key =
//   user key + PackSequenceAndType(seq,type) → 按同一 InternalKeyComparator
//   （CompareKeySeq = user key 升 / seq 降）排序 → keys/values/min_seq。
//   最终序 == BuildSortKeys memcmp 快排的最终结果 == memtable/slim 索引序。
inline ROCKSDB_NAMESPACE::Status BuildWalSortedAside(
    ROCKSDB_NAMESPACE::Env* env, const std::string& dir, uint32_t part,
    uint32_t gen, const ROCKSDB_NAMESPACE::InternalKeyComparator& icmp,
    std::vector<std::string>* keys /*out: internal key，已排序*/,
    std::vector<std::string>* values /*out: 与 keys 对齐*/,
    uint64_t* min_seq /*out, 可为 nullptr*/) {
  keys->clear();
  values->clear();
  uint64_t mn = std::numeric_limits<uint64_t>::max();
  WalScanner scanner(env, dir, part, gen);
  ZfRecordHeader h;
  ROCKSDB_NAMESPACE::Slice key, value;
  while (scanner.Next(&h, &key, &value)) {
    std::string ik;
    ik.reserve(key.size() + 8);
    ik.append(key.data(), key.size());
    ROCKSDB_NAMESPACE::PutFixed64(
        &ik, ROCKSDB_NAMESPACE::PackSequenceAndType(
                 h.seq, static_cast<ROCKSDB_NAMESPACE::ValueType>(h.type)));
    keys->push_back(std::move(ik));
    values->emplace_back(value.data(), value.size());
    if (h.seq < mn) {
      mn = h.seq;
    }
  }
  if (!scanner.status().ok()) {
    keys->clear();
    values->clear();
    return ROCKSDB_NAMESPACE::Status::Corruption(
        "ZF WAL scan failed: " + scanner.status().ToString());
  }
  if (!keys->empty()) {
    // 索引排序（每 ik 唯一——seq 唯一，比较器不会返回 0）。
    std::vector<size_t> order(keys->size());
    for (size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) {
                return icmp.CompareKeySeq(ROCKSDB_NAMESPACE::Slice((*keys)[a]),
                                          ROCKSDB_NAMESPACE::Slice((*keys)[b])) < 0;
              });
    std::vector<std::string> nk, nv;
    nk.reserve(order.size());
    nv.reserve(order.size());
    for (const size_t i : order) {
      nk.push_back(std::move((*keys)[i]));
      nv.push_back(std::move((*values)[i]));
    }
    *keys = std::move(nk);
    *values = std::move(nv);
  }
  if (min_seq != nullptr) {
    *min_seq = mn;
  }
  return ROCKSDB_NAMESPACE::Status::OK();
}

}  // namespace zeroflush
