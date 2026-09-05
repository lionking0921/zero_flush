// ZeroFlush FPGA 加速器 —— host 端字节格式库（纯 C++，无 XRT / 内核依赖）
//
// 本文件是"字节格式的事实标准"，只服务于 host 端（数据生成 + 预言机），
// 与 device 端 kernel/krnl_vadd.cpp 里的 decoder 各自独立实现、互为校验。
//
// 涵盖的字节格式：
//   1. Slim 条目（RocksDB block_based 格式，L0->L1 辅助排序的 memtable 索引项）
//        [varint32 ik_len][internal_key = user(24) || LE8((seq<<8)|type)]
//        [varint32 16][locator 16B]    locator = part u32 | gen u32 | wal_offset u64（全 LE）
//   2. WAL ZfRecord 帧（zero_flush P-partition WAL）
//        [24B header][user key][value][crc32c 4B]
//        header: magic u32=0x31304655 @0, cf_id u16 @4, type u8 @6, flags u8 @7,
//                key_len u32 @8, val_len u32 @12, seq u64 @16
//        value 起始 = 24 + key_len
//   3. 每输入口 staged 缓冲 = [WAL 区字节][slim 区字节]（连续一个 BO，供 kernel
//      decoder(buf, kv, file_size=WAL区字节数) 消费）
//
// 参考：source/rocksdb-zeroflush/zeroflush/FPGA_SLIM_MEMTABLE_BYTEFORMAT.md
//       （§9 WAL 记录 / §10 slim memtable / §11 internal key）。

#ifndef ZF_FORMAT_H
#define ZF_FORMAT_H

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace zf {

// ---- 固定尺寸常量（与 kernel/krnl_host.h 对齐）----------------------------
static constexpr uint32_t kUserKeyLength = 24;    // 用户键长（固定 24）
static constexpr uint32_t kInternalKeyLength = 32; // user + 8B footer
static constexpr uint32_t kValueLength = 1024;    // 值上限（2×512B slice）
static constexpr uint32_t kMaxInputNum = 4;       // MAX_INPUT_FILE_NUM
static constexpr uint32_t kZfMagic = 0x31304655;  // 'FU01'
static constexpr size_t kZfHeaderLen = 24;        // ZfRecord 帧头
static constexpr size_t kZfCrcLen = 4;            // ZfRecord 帧尾 crc32c

enum ValueType : uint8_t {
  kTypeDeletion = 0,
  kTypeValue = 1,
  kTypeMerge = 2,
};

// ============================================================================
// 小工具：小端 / varint / crc32c
// ============================================================================
inline void PutFixed32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 24));
}
inline void PutFixed64LE(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
// 从字节偏移读小端（未对齐安全；b 必须足够长）
inline uint32_t ReadFixed32LE(const std::vector<uint8_t>& b, size_t off) {
  assert(off + 4 <= b.size());
  return uint32_t(b[off]) | (uint32_t(b[off + 1]) << 8) |
         (uint32_t(b[off + 2]) << 16) | (uint32_t(b[off + 3]) << 24);
}
inline uint64_t ReadFixed64LE(const std::vector<uint8_t>& b, size_t off) {
  assert(off + 8 <= b.size());
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | b[off + size_t(i)];
  return v;
}
inline void PutVarint32(std::vector<uint8_t>& out, uint32_t v) {
  while (v >= 0x80) { out.push_back(uint8_t(v) | 0x80); v >>= 7; }
  out.push_back(uint8_t(v));
}

// CRC32C (Castagnoli)。仅用于 ZfRecord 帧尾字节保真；kernel 不校验。
inline uint32_t Crc32c(const uint8_t* data, size_t n) {
  static std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0x82F63B78U ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFU;
}

// ============================================================================
// 一条待排序/去重/物化的 K/V 记录（预言机内存态）
// ============================================================================
struct KRec {
  std::string user;    // 用户键（kUserKeyLength 字节）
  uint64_t seq = 0;
  uint8_t type = kTypeValue;
  std::string value;   // 0..kValueLength 字节；deletion 为空
  bool IsDeletion() const { return type == kTypeDeletion; }
};

// user + seq + type → 32B internal key：user || LE64((seq<<8)|type)
inline std::string InternalKey(const std::string& user, uint64_t seq, uint8_t type) {
  assert(user.size() == kUserKeyLength);
  std::vector<uint8_t> v(user.begin(), user.end());
  PutFixed64LE(v, (seq << 8) | uint64_t(type));
  return std::string(v.begin(), v.end());
}

// 内部键排序/等价比较：user 升序；user 相同按 seq 降序（新版本在前）。
// 语义 == kernel keystring 比较器（c[1] 低位块取反使 seq 大者在前）。
struct InternalKeyCmp {
  static int CmpUser(const std::string& a, const std::string& b) { return a.compare(b); }
  bool operator()(const KRec& a, const KRec& b) const {
    int c = CmpUser(a.user, b.user);
    if (c != 0) return c < 0;
    if (a.seq != b.seq) return a.seq > b.seq;      // seq 降序
    if (a.type != b.type) return a.type > b.type; // tie-break，稳定
    return false;                                  // 等键（同 user+seq+type）
  }
  // 仅 user 是否相同（encoder 去重判据：seq/type 不参与）
  static bool SameUser(const KRec& a, const KRec& b) { return a.user == b.user; }
  // 是否 a 排序在 b 前 或 二者相等（用于 unique）
  static bool EqualKey(const KRec& a, const KRec& b) {
    return a.user == b.user && a.seq == b.seq && a.type == b.type;
  }
};

// ============================================================================
// ZfRecord WAL 帧编码（字节保真；供 host 生成每口 WAL 段）
// ============================================================================
inline std::vector<uint8_t> BuildZfRecord(uint16_t cf_id, uint8_t type, uint8_t flags,
                                          const std::string& user, const std::string& value,
                                          uint64_t seq) {
  assert(user.size() == kUserKeyLength && value.size() <= kValueLength);
  std::vector<uint8_t> f;
  f.reserve(kZfHeaderLen + user.size() + value.size() + kZfCrcLen);
  PutFixed32LE(f, kZfMagic);            //  0..3  magic
  f.push_back(uint8_t(cf_id));          //  4     cf_id 低字节
  f.push_back(uint8_t(cf_id >> 8));     //  5     cf_id 高字节
  f.push_back(type);                    //  6     type
  f.push_back(flags);                   //  7     flags
  PutFixed32LE(f, uint32_t(user.size()));     //  8..11 key_len
  PutFixed32LE(f, uint32_t(value.size()));    // 12..15 val_len
  PutFixed64LE(f, seq);                       // 16..23 seq
  assert(f.size() == kZfHeaderLen);
  f.insert(f.end(), user.begin(), user.end());          // 24 .. 24+key_len
  f.insert(f.end(), value.begin(), value.end());        //     .. +val_len
  PutFixed32LE(f, Crc32c(f.data(), f.size()));          // + crc32c
  return f;
}

// ============================================================================
// 单个输入口 = 一条 WAL segment（一个 (part,gen)）的完整设备端内容
// ============================================================================
struct Port {
  uint32_t part = 0;
  uint32_t gen = 0;
  struct Entry {
    KRec rec;
    uint64_t wal_offset = 0;  // 该记录帧在本口 WAL 段内的字节偏移
  };
  std::vector<Entry> entries;   // 已按内部键排序（= slim 条目顺序）
  std::vector<uint8_t> wal;     // WAL 区
  std::vector<uint8_t> slim;    // slim 区（紧随 wal）

  uint32_t kv() const { return uint32_t(entries.size()); }
  uint64_t wal_bytes() const { return wal.size(); }
  // host 拼成一个 XRT BO：WAL 区 ++ slim 区
  std::vector<uint8_t> Staged() const {
    std::vector<uint8_t> s;
    s.reserve(wal.size() + slim.size());
    s.insert(s.end(), wal.begin(), wal.end());
    s.insert(s.end(), slim.begin(), slim.end());
    return s;
  }
};

// ============================================================================
// 工作负载：N(≤4) 口有序 run + 预言机期望
// ============================================================================
struct Workload {
  std::vector<Port> port;         // 参与输入的口（index 即 kernel 输入口编号）
  std::vector<KRec> expected;     // encoder 去重后应落盘的有序记录

  uint32_t total_kv() const {
    uint32_t n = 0;
    for (const auto& p : port) n += p.kv();
    return n;
  }
};

// 生成随机 24B 键（字节域伪随机；做查重防碰撞）。
inline std::vector<std::string> GenDistinctUsers(std::mt19937_64& rng, size_t n) {
  std::vector<std::string> u;
  u.reserve(n);
  while (u.size() < n) {
    std::string k(kUserKeyLength, '\0');
    for (size_t i = 0; i < kUserKeyLength; ++i) k[i] = char(uint8_t(rng() >> (8 * (i % 6))));
    bool dup = false;
    for (const auto& e : u) if (e == k) { dup = true; break; }
    if (!dup) u.push_back(k);
  }
  std::sort(u.begin(), u.end());
  return u;
}

// 随机值（长短混合 + 少量 deletion）；len ≤ kValueLength。
inline KRec GenRec(std::mt19937_64& rng, const std::string& user, uint64_t seq) {
  KRec r;
  r.user = user;
  r.seq = seq;
  r.type = ((rng() & 0xFF) < 24) ? kTypeDeletion : kTypeValue;
  if (r.IsDeletion()) { r.value.clear(); return r; }
  size_t len = ((rng() & 3) == 0) ? 1 + size_t(rng() % 600) : kValueLength;
  r.value.resize(len);
  for (size_t i = 0; i < len; ++i) {
    r.value[i] = char(uint8_t((rng() >> (8 * (i % 5))) ^ 0x5A) & 0xFF);
  }
  return r;
}

// 主入口：由随机种子生成 1..kMaxInputNum 个口的 run + 预言机。
//  port_count    使用的输入口数（1..4，余下口为空 run）
//  base_keys     互异 user 键数
//  dup_keys      复制到其它口、以产生同 user 不同 seq 遮蔽对的 user 键数
//                约束：dup_keys < base_keys，且不得是全局最大 user 键
inline Workload GenWorkload(uint64_t seed, uint32_t port_count, size_t base_keys,
                            size_t dup_keys) {
  assert(port_count >= 1 && port_count <= kMaxInputNum);
  assert(base_keys >= 2 && dup_keys < base_keys);

  std::mt19937_64 rng(seed);
  Workload w;
  std::vector<Port> ports(port_count);
  for (uint32_t p = 0; p < port_count; ++p) {
    ports[p].part = 1000 + p;
    ports[p].gen = 300 + p;
  }

  auto users = GenDistinctUsers(rng, base_keys);
  const std::string& max_user = users.back();  // 全局最大 user，禁止复制

  // owner 摊派：把基础键轮转分给各口
  std::vector<uint32_t> owner(base_keys);
  for (size_t i = 0; i < base_keys; ++i) owner[i] = uint32_t(i % port_count);

  // (1) 每基础键在 owner 口放基础版本
  std::vector<KRec> base(base_keys);
  for (size_t i = 0; i < base_keys; ++i) {
    // seq 限制在 < 2^48：encoder 的 PPS minSeqno 初值 = 2^56-1 哨兵，
    // footer=(seq<<8|type) < 2^56-1 才能保证 min 被真实 seq 更新。
    uint64_t seq = 1 + (rng() % 0x0000FFFFFFFFFFFFULL);
    base[i] = GenRec(rng, users[i], seq);
  }
  for (size_t i = 0; i < base_keys; ++i)
    ports[owner[i]].entries.push_back({base[i], 0});

  // (2) dup：从非最大键里挑 dup_keys 个，放另一口、给一个更大 seq
  std::vector<size_t> dup_src;
  {
    std::vector<size_t> cand;
    for (size_t i = 0; i + 1 < base_keys; ++i) cand.push_back(i);
    std::shuffle(cand.begin(), cand.end(), rng);
    for (size_t k = 0; k < dup_keys && k < cand.size(); ++k) dup_src.push_back(cand[k]);
  }
  for (size_t k = 0; k < dup_src.size(); ++k) {
    size_t i = dup_src[k];
    uint32_t dst = (owner[i] + 1 + uint32_t(k)) % port_count;
    if (dst == owner[i]) dst = (dst + 1) % port_count;
    uint64_t seq2 = base[i].seq + 100 + (rng() % 3);  // 保证 > owner 版本 seq
    ports[dst].entries.push_back({GenRec(rng, users[i], seq2), 0});
  }

  // (3) 每口内部排序（user 升；同口同 user 应唯一），再建 WAL + slim
  for (uint32_t p = 0; p < port_count; ++p) {
    auto& P = ports[p];
    std::stable_sort(P.entries.begin(), P.entries.end(),
                     [](const Port::Entry& a, const Port::Entry& b) {
                       return InternalKeyCmp()(a.rec, b.rec);
                     });
    // 兜底去同口同 user（不应发生，防生成器缺陷）
    P.entries.erase(
        std::unique(P.entries.begin(), P.entries.end(),
                    [](const Port::Entry& a, const Port::Entry& b) {
                      return InternalKeyCmp::SameUser(a.rec, b.rec);
                    }),
        P.entries.end());
    // WAL：按 entries 序（或任意序）追加帧
    uint64_t off = 0;
    for (auto& e : P.entries) {
      e.wal_offset = off;
      auto fr = BuildZfRecord(uint16_t(P.part), e.rec.type, 0, e.rec.user, e.rec.value,
                              e.rec.seq);
      P.wal.insert(P.wal.end(), fr.begin(), fr.end());
      off += fr.size();
    }
    // slim：按排好序的 entries 顺序写
    for (const auto& e : P.entries) {
      const std::string ik = InternalKey(e.rec.user, e.rec.seq, e.rec.type);
      PutVarint32(P.slim, kInternalKeyLength);      // ik_len=32
      P.slim.insert(P.slim.end(), ik.begin(), ik.end());
      PutVarint32(P.slim, 16);                       // locator 长=16
      PutFixed32LE(P.slim, P.part);
      PutFixed32LE(P.slim, P.gen);
      PutFixed64LE(P.slim, e.wal_offset);
    }
  }

  // (4) 预言机：全键全局排序（user 升、seq 降）→ encoder 去重（保留首见=最大 seq）
  std::vector<KRec> all;
  for (const auto& P : ports)
    for (const auto& e : P.entries) all.push_back(e.rec);
  std::stable_sort(all.begin(), all.end(), InternalKeyCmp());
  std::vector<KRec> expect;
  expect.reserve(all.size());
  for (const auto& r : all) {
    if (expect.empty() || !InternalKeyCmp::SameUser(expect.back(), r)) expect.push_back(r);
  }
  // 校验：全局最大 user 必须唯一出现（即 expect 末条 == all 末条，避免 encoder
  // 在最后一条恰好是重复而丢弃、造成"末块收尾"缺陷被触发）。
  assert(!all.empty());
  assert(InternalKeyCmp::EqualKey(all.back(), expect.back()) ||
         all.back().user != max_user);  // all.back() 就是 max_user，重复保护见下
  // all 末条 == max_user 的唯一条；expect 末条亦应为它
  if (!InternalKeyCmp::EqualKey(all.back(), expect.back())) {
    // 若 max_user 意外被 dup，报告（生成器不应触发）
    assert(false && "global-max user must be unique in workload");
  }

  w.port = std::move(ports);
  w.expected = std::move(expect);
  return w;
}

}  // namespace zf
#endif  // ZF_FORMAT_H
