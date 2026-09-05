// zf_cpu_sim_ab.cpp —— A+B 全版本保留 · CPU 端整链数据流仿真（AB-3）
//
// 目的：证明当输入 = [1..3 个 B 侧 §14.6 raw-SST 文件字节链] + [≤4 个 A 侧 staged
//（封存 WAL 段 + slim 条目区）]，kernel 的
//     decode_port(decoder / decoder_sst) → merge → encoder(keep_all_versions=1)
// 产出的 v2/crc32c [data+index] 前缀（engine TableReader 全键直读对拍在 AB-4，
// 用 build/zf_seal_check），其记录多重集 == 预言机（A∪B 全版本、user 升 seq 降）。
//
// B 侧文件怎么来：用「本 kernel 自己的 full-version encoder（mode=1）」把一条
// 有序记录 run 预产成 [data+index] 前缀，再附一个与引擎 footer 同几何的 53B
// 合成 footer —— 即一个 §14.6 raw SST 字节。zf_seal_check（AB-4）已证同款前缀
// + 引擎封口 = 引擎可读，故这里是 round-trip 喂回 decoder_sst。
//
// 每个 case 额外 dump [PPS+data+index] 前缀（布局同 zf_cpu_sim / zf_seal_check）
// 与 .expect（预言机逐行 hex(32B ik)\thex(value)），供 AB-4 引擎直读对拍。
//
// 编译：
//   g++ -std=c++17 -O1 -pthread -DHLS_STREAM_THREAD_SAFE \
//       -I ../kernel -I ../host -I /tools/Xilinx/Vitis_HLS/2022.2/include \
//       zf_cpu_sim_ab.cpp -o /tmp/zf_cpu_sim_ab
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define HLS_STREAM_THREAD_SAFE
#include "../kernel/krnl_vadd.cpp"   // 内核整 TU（decoder/decoder_sst/merge/encoder/类型/常量）
#include "../host/zf_sst_decode.h"   // zf_format.h + host 解码比对（Oracle/KRec/InternalKeyCmp）

using hls::stream;

// ============================================================================
// 小工具
// ============================================================================
static std::vector<ap_uint<32>> BytesToWords(const std::vector<uint8_t>& b) {
  std::vector<ap_uint<32>> w((b.size() + 3) / 4 + 2 + 1024, 0);
  for (size_t i = 0; i < b.size(); ++i)
    w[i / 4].range((i % 4) * 8 + 7, (i % 4) * 8) = b[i];
  return w;
}
static std::vector<uint8_t> WordsToBytes128(const ap_uint<128>* w, size_t nbytes) {
  size_t nw = (nbytes + 15) / 16;
  std::vector<uint8_t> b(nbytes);
  for (size_t i = 0; i < nw; ++i)
    for (int j = 0; j < 16; ++j) {
      size_t o = i * 16 + j;
      if (o < nbytes) b[o] = uint8_t(w[i].range(j * 8 + 7, j * 8));
    }
  return b;
}
static std::string Hexes(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string h;
  h.reserve(s.size() * 2);
  for (size_t i = 0; i < s.size(); ++i) {
    h.push_back(d[uint8_t(s[i]) >> 4]);
    h.push_back(d[uint8_t(s[i]) & 0xf]);
  }
  return h;
}

// ---- §14.6 v2 footer 合成（53B，与引擎 FooterBuilder 同几何）----
inline void PutVarint64LE(std::vector<uint8_t>& v, uint64_t x) {
  while (x >= 0x80) { v.push_back(uint8_t(x) | 0x80); x >>= 7; }
  v.push_back(uint8_t(x));
}
// data_size = data 区字节（含各块 trailer）；index_size = index 区字节（含 index trailer）
static std::vector<uint8_t> SyntheticFooter(uint64_t data_size, uint64_t index_size) {
  std::vector<uint8_t> f;
  f.reserve(53);
  f.push_back(1);                  // checksum type: kCRC32c
  PutVarint64LE(f, 0); PutVarint64LE(f, 0);               // metaindex handle (0,0) 丢弃
  PutVarint64LE(f, data_size);
  PutVarint64LE(f, index_size - 5);                       // index content（不含其 5B trailer）
  // zero-pad 到 part3 起点（字节 41），版本 u32 LE + magic u64 LE
  if (f.size() > 41) { fprintf(stderr, "synthetic footer overflow\n"); exit(2); }
  f.resize(41, 0);
  uint64_t magic = 0x88e241b785f4cff7ULL;
  uint32_t ver = 2;
  for (int i = 0; i < 4; ++i) f.push_back(uint8_t(ver >> (8 * i)));
  for (int i = 0; i < 8; ++i) f.push_back(uint8_t(magic >> (8 * i)));
  if (f.size() != 53) { fprintf(stderr, "synthetic footer size %zu != 53\n", f.size()); exit(2); }
  return f;
}

// ============================================================================
// AB 场景：A 口 staged + B 链文件 的内容由"预言机记录"统一刻画
// ============================================================================
// 每条预言机记录 (user,seq,type,value)。要求：全 union 内 (user,seq) 全局唯一
//（保证内部键严格递增、无 unshared=0 退化记录）；每 list 已按 InternalKeyCmp 排序。
static uint64_t G_SEQ = 1;

static zf::KRec MkRec(const std::string& user, uint8_t type, size_t vlen) {
  zf::KRec r;
  r.user = user;
  r.seq = G_SEQ++;   // 全局单调唯一 → 同 user 也可比序、无退化重复键
  r.type = type;
  if (vlen == 0) return r;  // deletion
  r.value.assign(vlen, '\0');
  for (size_t i = 0; i < vlen; ++i)
    r.value[i] = char(uint8_t(0x5A ^ user[i % user.size()] ^ ((i * 7 + uint8_t(r.seq)) & 0xFF)));
  return r;
}

// 把一组已排序记录构建成一个 A 口 staged（WAL 段 + slim 条目区）。
// 允许同口同 user 多版本（调用方自保证排序正确）。
static zf::Port BuildStaged(const std::vector<zf::KRec>& recs, uint32_t part, uint32_t gen) {
  zf::Port P;
  P.part = part;
  P.gen = gen;
  uint64_t off = 0;
  for (const auto& r : recs) {
    zf::Port::Entry e;
    e.rec = r;
    e.wal_offset = off;
    P.entries.push_back(e);
    auto fr = zf::BuildZfRecord(uint16_t(part), r.type, 0, r.user, r.value, r.seq);
    P.wal.insert(P.wal.end(), fr.begin(), fr.end());
    off += fr.size();
  }
  for (const auto& e : P.entries) {
    const std::string ik = zf::InternalKey(e.rec.user, e.rec.seq, e.rec.type);
    zf::PutVarint32(P.slim, zf::kInternalKeyLength);
    P.slim.insert(P.slim.end(), ik.begin(), ik.end());
    zf::PutVarint32(P.slim, 16);
    zf::PutFixed32LE(P.slim, P.part);
    zf::PutFixed32LE(P.slim, P.gen);
    zf::PutFixed64LE(P.slim, e.wal_offset);
  }
  return P;
}

// ============================================================================
// 一次整链 pipeline 运行（CPU 镜像，线程模型照抄 zf_cpu_sim.cpp）
// ============================================================================
struct Slot {
  uint32_t kind = 0;              // 0 = A staged；1 = B raw-SST 链
  uint64_t kv = 0;                // A: slim 条数；B: 0
  uint64_t file_size = 0;         // A: WAL 区字节；B: 链字节总数
  std::vector<uint8_t> bytes;     // staged 或 链内容（含描述表）
};

struct PipeOut {
  bool ok = false;
  uint64_t top_sst = 0, top_idx = 0, file_num = 0;
  std::vector<uint8_t> data_b, idx_b;
  std::vector<uint64_t> out;
  uint64_t pps[128] = {0};
};

// 4 口 decode_port → merge（子线程 kv_sum+1 次）→ encoder(keep_all_versions)
static PipeOut RunAB(const std::vector<Slot>& slots, uint64_t keep_all,
                     uint64_t kv_sum_total) {
  PipeOut r;
  Slot slot[4];
  for (uint32_t p = 0; p < 4; ++p) {
    slot[p] = (p < slots.size()) ? slots[p] : Slot{};
    if (slot[p].bytes.empty()) slot[p].file_size = 0;
  }
  std::vector<std::vector<ap_uint<32>>> words(4);
  for (uint32_t p = 0; p < 4; ++p) words[p] = BytesToWords(slot[p].bytes);

  stream<fifo_key_meta> km_in[4];
  stream<fifo_value_slice> val_in[4];
  stream<fifo_key_meta> km_merged;
  stream<ap_uint<2>> merge_res;

  // 1) 主线程把 4 个 decode_port 全部跑完（写无界流）
  for (uint32_t p = 0; p < 4; ++p)
    decode_port(words[p].data(), slot[p].kv, slot[p].file_size, slot[p].kind,
                km_in[p], val_in[p]);

  // 2) merge 子线程
  uint32_t N = uint32_t(kv_sum_total);
  std::atomic<bool> merge_done(false);
  std::thread merge_th([&] {
    for (uint32_t i = 0; i <= N; ++i) merge(km_in, km_merged, merge_res);
    merge_done = true;
  });

  // 3) encoder 主线程
  const uint64_t file_limit_u = 1ull << 26;
  std::vector<ap_uint<128>> sst((file_limit_u / 16) + 16);
  std::vector<ap_uint<128>> idx((index_block_buffer_size / 16) + 16);
  r.out.assign(PPS_KERNEL_SIZE + 2 * MAX_OUTPUT_FILE_NUM + 8, 0);
  encoder(km_merged, val_in, merge_res, ap_uint<32>(N), sst.data(), idx.data(),
          r.out.data(), ap_uint<40>(file_limit_u), ap_uint<32>(keep_all));
  merge_th.join();

  r.top_sst = r.out[PPS_KERNEL_SIZE];
  r.top_idx = r.out[PPS_KERNEL_SIZE + MAX_OUTPUT_FILE_NUM];
  r.file_num = r.out[PPS_KERNEL_SIZE + 2 * MAX_OUTPUT_FILE_NUM];
  for (int i = 0; i < 128; ++i) r.pps[i] = r.out[i];
  r.data_b = WordsToBytes128(sst.data(), size_t(r.top_sst));
  r.idx_b = WordsToBytes128(idx.data(), size_t(r.top_idx));
  r.ok = (r.file_num == 1);
  return r;
}

// ============================================================================
// B 链构造：把记录 list（可含同 user 多版本）预产成一个 §14.6 raw SST 字节
// ============================================================================
// 经 A staged → 空口 merge → encoder(mode=1) 得到 [data+index]，再补合成 footer。
// 保留 enc（RunAB 输出，含 PPS/data/index），供 AB-4 把该 B 内容以"前缀"形态落盘、
// 经引擎 zf_seal_check 封口直读，锚定 B 输入确为引擎可读格式（而非 decoder_sst 自说自话）。
struct RawSst {
  std::vector<uint8_t> file;   // [data][index][53B 合成 footer]（喂 decoder_sst 的完整字节）
  PipeOut enc;                 // data_b/idx_b/pps/top_sst/top_idx（→ 前缀 dump）
};
static RawSst EncodeRawSst(const std::vector<zf::KRec>& recs) {
  zf::Port P = BuildStaged(recs, 900, 1);
  std::vector<uint8_t> staged = P.Staged();
  Slot s;
  s.kind = 0;
  s.kv = P.kv();
  s.file_size = P.wal_bytes();
  s.bytes = staged;
  PipeOut o = RunAB({s}, 1, P.kv());
  if (!o.ok || o.file_num != 1) {
    fprintf(stderr, "EncodeRawSst: encoder pipeline failed (kv=%u data=%llu idx=%llu)\n",
            P.kv(), (unsigned long long)o.top_sst, (unsigned long long)o.top_idx);
    exit(2);
  }
  RawSst r;
  r.enc = std::move(o);
  r.file.insert(r.file.end(), r.enc.data_b.begin(), r.enc.data_b.end());
  r.file.insert(r.file.end(), r.enc.idx_b.begin(), r.enc.idx_b.end());
  auto foot = SyntheticFooter(r.enc.top_sst, r.enc.top_idx);
  r.file.insert(r.file.end(), foot.begin(), foot.end());
  return r;
}

// B 链缓冲：描述表 + 各文件字节（同一口，文件键范围互不相交且有序）。
// 布局与 decoder_sst 读法一致：[u64 K][K×{u64 file_off, u64 file_sz}][文件 1..K 字节]。
static std::vector<uint8_t> BuildChain(const std::vector<std::vector<uint8_t>>& files) {
  auto putU64 = [](std::vector<uint8_t>& v, size_t at, uint64_t x) {
    for (int i = 0; i < 8; ++i) v[at + size_t(i)] = uint8_t(x >> (8 * i));
  };
  uint64_t K = files.size();
  const uint64_t base = 8 + 16 * K;   // 文件 1 字节起点（描述区之后）
  std::vector<uint8_t> ch;
  ch.assign(size_t(base), 0);
  putU64(ch, 0, K);
  uint64_t body_off = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    putU64(ch, 8 + 16 * i, base + body_off);
    putU64(ch, 16 + 16 * i, files[i].size());
    body_off += files[i].size();
  }
  for (const auto& f : files) ch.insert(ch.end(), f.begin(), f.end());
  return ch;
}

// ============================================================================
// 场景 + 预言机
// ============================================================================
struct Scenario {
  std::string name;
  std::vector<std::vector<zf::KRec>> a;       // 每个 A 口（排序好的）记录
  std::vector<std::vector<zf::KRec>> bfiles;  // B 链每文件记录（排序好的）
  std::vector<zf::KRec> oracle;               // 全 union 排序（user 升 seq 降）
};

static void SortRecs(std::vector<zf::KRec>& v) {
  std::stable_sort(v.begin(), v.end(), zf::InternalKeyCmp());
}
// 从 A 口 list + B 文件 list 组装 oracle（校验全 union 无重复内部键）
static void Finalize(Scenario& sc) {
  std::vector<zf::KRec> all;
  for (auto& l : sc.a) { SortRecs(l); all.insert(all.end(), l.begin(), l.end()); }
  for (auto& f : sc.bfiles) { SortRecs(f); all.insert(all.end(), f.begin(), f.end()); }
  std::stable_sort(all.begin(), all.end(), zf::InternalKeyCmp());
  for (size_t i = 1; i < all.size(); ++i) {
    if (all[i].user == all[i - 1].user && all[i].seq == all[i - 1].seq) {
      fprintf(stderr, "scenario %s: duplicate (user,seq) at %zu\n", sc.name.c_str(), i);
      exit(2);
    }
  }
  sc.oracle = std::move(all);
}

// ---- 用户池 ----
static std::vector<std::string> MkUsers(size_t n) {
  std::mt19937_64 rng(20240901);
  return zf::GenDistinctUsers(rng, n);
}

// ============================================================================
// 用例定义
// ============================================================================
static std::vector<Scenario> MakeScenarios() {
  std::vector<Scenario> out;
  auto users = MkUsers(260);   // 全局排序 24B 用户池（各用例取一段）

  // ---- S0 仅 B，单文件，含同 user 多版本 + tombstone ----
  {
    Scenario sc;
    sc.name = "s0_b_single_mv";
    const std::string& u0 = users[0];
    const std::string& u1 = users[1];
    const std::string& u2 = users[2];
    const std::string& u3 = users[3];
    // u0: 3 版本(旧→新升序 seq) + 中间 tombstone
    std::vector<zf::KRec> b;
    b.push_back(MkRec(u0, zf::kTypeValue, 40));    // oldest
    b.push_back(MkRec(u0, zf::kTypeDeletion, 0));
    b.push_back(MkRec(u0, zf::kTypeValue, 1024));  // newest
    b.push_back(MkRec(u1, zf::kTypeValue, 7));
    b.push_back(MkRec(u2, zf::kTypeDeletion, 0));
    b.push_back(MkRec(u3, zf::kTypeValue, 1024));
    SortRecs(b);   // user asc / seq desc → 同 user 版本按 seq 降序相邻
    sc.bfiles.push_back(b);
    Finalize(sc);
    out.push_back(sc);
  }

  // ---- S1 仅 B，两文件链（user 范围不相交、有序），各文件 8/5 user，含删除 ----
  {
    Scenario sc;
    sc.name = "s1_b_chain";
    std::vector<zf::KRec> f1, f2;
    for (int i = 0; i < 8; ++i) f1.push_back(MkRec(users[10 + i], (i == 3) ? zf::kTypeDeletion : zf::kTypeValue, (i % 3) ? 100 : 700));
    for (int i = 0; i < 5; ++i) f2.push_back(MkRec(users[20 + i], zf::kTypeValue, 1024));
    SortRecs(f1);
    SortRecs(f2);
    sc.bfiles.push_back(f1);
    sc.bfiles.push_back(f2);
    Finalize(sc);
    out.push_back(sc);
  }

  // ---- S2 仅 A，1 口同 user 多版本 ----
  {
    Scenario sc;
    sc.name = "s2_a_only_mv";
    std::vector<zf::KRec> a0;
    a0.push_back(MkRec(users[40], zf::kTypeValue, 10));
    a0.push_back(MkRec(users[40], zf::kTypeValue, 512));
    a0.push_back(MkRec(users[41], zf::kTypeDeletion, 0));
    a0.push_back(MkRec(users[41], zf::kTypeValue, 800));
    a0.push_back(MkRec(users[42], zf::kTypeValue, 5));
    SortRecs(a0);
    sc.a.push_back(a0);
    Finalize(sc);
    out.push_back(sc);
  }

  // ---- S3 仅 A，3 口（跨口同 user 遮蔽 → mode1 保留两版本）----
  {
    Scenario sc;
    sc.name = "s3_a_3ports";
    for (int p = 0; p < 3; ++p) {
      std::vector<zf::KRec> ap;
      for (int i = 0; i < 6; ++i) ap.push_back(MkRec(users[50 + p * 6 + i], zf::kTypeValue, 60));
      sc.a.push_back(ap);
    }
    // 额外：users[50] 在 port0 已有，再往 port1/2 放更新版本
    sc.a[1].push_back(MkRec(users[50], zf::kTypeValue, 300));
    sc.a[2].push_back(MkRec(users[50], zf::kTypeValue, 900));
    Finalize(sc);
    out.push_back(sc);
  }

  // ---- S4 A+B：B 旧版 + A 新版 同 user 跨 A/B；A 含新 user；双方 tombstone ----
  {
    Scenario sc;
    sc.name = "s4_a_over_b";
    std::vector<zf::KRec> b;
    for (int i = 0; i < 10; ++i) b.push_back(MkRec(users[80 + i], zf::kTypeValue, 128));
    // B 里 u80..u84 各自再带更旧一版（在文件内构成多版本）
    for (int i = 0; i < 5; ++i) b.push_back(MkRec(users[80 + i], zf::kTypeValue, 16));
    SortRecs(b);
    sc.bfiles.push_back(b);
    // A 覆盖 u80..u84（新 seq）+ 全新 u90..u94
    std::vector<zf::KRec> a0, a1;
    for (int i = 0; i < 5; ++i) a0.push_back(MkRec(users[80 + i], (i == 2) ? zf::kTypeDeletion : zf::kTypeValue, 999));
    for (int i = 0; i < 5; ++i) a1.push_back(MkRec(users[90 + i], zf::kTypeValue, 77));
    SortRecs(a0); SortRecs(a1);
    sc.a.push_back(a0);
    sc.a.push_back(a1);
    Finalize(sc);
    out.push_back(sc);
  }

  // ---- S5 A+B 大样本：B 链 2 文件 + A 2 口；值长短混合、覆盖/新增/多版本 ----
  {
    Scenario sc;
    sc.name = "s5_bulk";
    // B: 两个范围不相交的文件（各 30 user）
    std::vector<zf::KRec> b1, b2;
    for (int i = 0; i < 30; ++i) {
      size_t vl = (i % 5 == 0) ? 1024 : (40 + (i % 4) * 90);
      b1.push_back(MkRec(users[100 + i], (i % 11 == 0) ? zf::kTypeDeletion : zf::kTypeValue, vl));
    }
    for (int i = 0; i < 30; ++i)
      b2.push_back(MkRec(users[130 + i], zf::kTypeValue, 250));
    SortRecs(b1); SortRecs(b2);
    sc.bfiles.push_back(b1);
    sc.bfiles.push_back(b2);
    // A: port0 覆盖 B 中部分 user（较新）、port1 新 user
    std::vector<zf::KRec> a0, a1;
    for (int i = 0; i < 12; ++i) {  // 覆盖 b1 前 12 user
      size_t vl = (i == 5) ? 0 : 600;   // 一个删除
      a0.push_back(MkRec(users[100 + i], (i == 5) ? zf::kTypeDeletion : zf::kTypeValue, vl));
    }
    // 其中 u100、u105 额外多一版在 port1（跨 A 口多版本）
    a1.push_back(MkRec(users[100], zf::kTypeValue, 111));
    a1.push_back(MkRec(users[105], zf::kTypeValue, 222));
    for (int i = 0; i < 10; ++i) a1.push_back(MkRec(users[160 + i], zf::kTypeValue, 33));
    SortRecs(a0); SortRecs(a1);
    sc.a.push_back(a0);
    sc.a.push_back(a1);
    Finalize(sc);
    out.push_back(sc);
  }

  // ---- S6 A+B：值长边界集（1/15/16/17/31/32/33/47/48/63/64/65/1024），跨 A/B ----
  {
    Scenario sc;
    sc.name = "s6_vlen_bounds";
    static const size_t vlens[] = {1, 15, 16, 17, 31, 32, 33, 47, 48, 63, 64, 65, 1024};
    const size_t nv = sizeof(vlens) / sizeof(vlens[0]);
    std::vector<zf::KRec> b, a0;
    // B 文件里放一半边界值（含一个删除 → vlen=0）
    for (size_t i = 0; i < nv; ++i) {
      if (i % 2 == 0)
        b.push_back(MkRec(users[190 + i], (i == 4) ? zf::kTypeDeletion : zf::kTypeValue, vlens[i]));
    }
    SortRecs(b);
    sc.bfiles.push_back(b);
    // A 放另一半（含一跨 A/B 同 user 的更新版，值长 1024）
    for (size_t i = 1; i < nv; i += 2)
      a0.push_back(MkRec(users[190 + i], zf::kTypeValue, vlens[i]));
    SortRecs(a0);
    sc.a.push_back(a0);
    Finalize(sc);
    out.push_back(sc);
  }

  return out;
}

// ============================================================================
// dump + 对拍（CPU 预言机门）
// ============================================================================
static const char* g_dump_dir = nullptr;

static std::vector<std::string> CheckOutput(const Scenario& sc, const PipeOut& o) {
  // 复用 M3 CompareWorkload：逐块验 masked crc、逐条 ik/vlen/value 位置比对，另抽查
  // PPS 计数/键极值/raw 尺寸。Workload.port 不被其使用，故只需填 expected=全版本预言机。
  zf::Workload wl;
  wl.expected = sc.oracle;
  return zfdecode::CompareWorkload(wl, o.data_b, o.idx_b, o.pps, size_t(o.top_sst));
}

// 写一个 zf_seal_check 可消费的"前缀"（[PPS+data+index]，与 zf_cpu_sim.cpp 同布局）
// + 逐行 `hex(32B ik)\thex(值)` 的 .expect（该前缀应解出的记录集）。
static void WriteZfPrefix(const std::string& pfx, const std::vector<uint8_t>& data_b,
                          const std::vector<uint8_t>& idx_b, const uint64_t* pps,
                          uint64_t top_sst, uint64_t top_idx,
                          const std::vector<zf::KRec>& oracle) {
  FILE* f = fopen(pfx.c_str(), "wb");
  if (!f) { printf("  dump open fail %s\n", pfx.c_str()); return; }
  fwrite("ZFPRFX1\n", 1, 8, f);
  uint64_t ndel = 0;
  for (const auto& r : oracle) ndel += r.IsDeletion() ? 1 : 0;
  uint64_t hdr[3] = {top_sst, top_idx, ndel};
  fwrite(hdr, 8, 3, f);
  fwrite(pps, sizeof(uint64_t), PPS_KERNEL_SIZE, f);
  fwrite(data_b.data(), 1, data_b.size(), f);
  fwrite(idx_b.data(), 1, idx_b.size(), f);
  fclose(f);
  std::string exp = pfx.substr(0, pfx.size() - strlen(".prefix")) + ".expect";
  FILE* e = fopen(exp.c_str(), "wb");
  if (!e) { printf("  dump open fail %s\n", exp.c_str()); return; }
  for (const auto& r : oracle) {
    std::string ik = zf::InternalKey(r.user, r.seq, r.type);
    std::string line = Hexes(ik) + "\t" + Hexes(r.value) + "\n";
    fwrite(line.data(), 1, line.size(), e);
  }
  fclose(e);
  printf("  dumped %s (+ .expect)\n", pfx.c_str());
}

static void DumpCase(const std::string& name, const PipeOut& o,
                     const std::vector<zf::KRec>& oracle) {
  if (!g_dump_dir) return;
  std::string pfx = std::string(g_dump_dir) + "/" + name + ".prefix";
  WriteZfPrefix(pfx, o.data_b, o.idx_b, o.out.data(), o.top_sst, o.top_idx, oracle);
}

static bool RunCaseOnce(const Scenario& sc, const char* dump_label) {
  // ---- B 文件字节（各文件 = encoder 预产的完整 raw-SST）----
  std::vector<RawSst> braw;
  for (auto& f : sc.bfiles) braw.push_back(EncodeRawSst(f));
  // ---- 组装口 ----
  std::vector<Slot> slots;
  uint32_t port_gen = 0;
  for (size_t ai = 0; ai < sc.a.size() && slots.size() < 4; ++ai) {
    zf::Port P = BuildStaged(sc.a[ai], 1000 + uint32_t(port_gen), 300 + uint32_t(port_gen));
    ++port_gen;
    Slot s;
    s.kind = 0;
    s.kv = P.kv();
    s.file_size = P.wal_bytes();
    s.bytes = P.Staged();
    slots.push_back(s);
  }
  uint64_t b_total = 0;
  if (!sc.bfiles.empty() && slots.size() < 4) {
    for (const auto& rb : braw) b_total += rb.file.size();
    Slot s;
    s.kind = 1;
    s.kv = 0;
    s.file_size = 8 + 16 * sc.bfiles.size() + b_total;
    std::vector<std::vector<uint8_t>> bbytes;
    for (const auto& rb : braw) bbytes.push_back(rb.file);
    s.bytes = BuildChain(bbytes);
    slots.push_back(s);
  }
  if (slots.empty()) { fprintf(stderr, "%s: empty case\n", sc.name.c_str()); return false; }

  uint64_t kv_sum = sc.oracle.size();
  PipeOut o = RunAB(slots, /*keep_all=*/1, kv_sum);

  uint64_t oracle_n = sc.oracle.size();
  printf("case %s  A_ports=%zu B_files=%zu rec_total=%llu expect=%llu "
         "file_num=%llu data=%llu idx=%llu\n",
         sc.name.c_str(), sc.a.size(), sc.bfiles.size(), (unsigned long long)kv_sum,
         (unsigned long long)oracle_n, (unsigned long long)o.file_num,
         (unsigned long long)o.top_sst, (unsigned long long)o.top_idx);
  if (!o.ok || o.file_num != 1) {
    printf("  FAIL file_num=%llu (expect 1)\n", (unsigned long long)o.file_num);
    return false;
  }
  auto errs = CheckOutput(sc, o);
  if (!errs.empty()) {
    for (auto& e : errs) printf("  FAIL %s\n", e.c_str());
    return false;
  }
  if (dump_label) {
    // 主输出前缀（A+B 产物）+ 每个 B 输入文件前缀（引擎可读锚定）
    DumpCase(std::string(dump_label), o, sc.oracle);
    for (size_t fi = 0; fi < braw.size(); ++fi) {
      std::string name = std::string(dump_label) + "_binput" + std::to_string(fi);
      WriteZfPrefix(std::string(g_dump_dir) + "/" + name + ".prefix", braw[fi].enc.data_b,
                    braw[fi].enc.idx_b, braw[fi].enc.out.data(), braw[fi].enc.top_sst,
                    braw[fi].enc.top_idx, sc.bfiles[fi]);
    }
  }
  printf("  OK  %llu records match oracle (all versions)\n", (unsigned long long)oracle_n);
  return true;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc) g_dump_dir = argv[++i];
    else { fprintf(stderr, "usage: zf_cpu_sim_ab [-o dump_dir]\n"); return 2; }
  }
  auto scen = MakeScenarios();
  int npass = 0;
  char label[64];
  constexpr int kCaseTimeoutSec = 60;   // 防止 decoder 少发→merge/encoder 阻塞死锁时整跑挂死
  for (size_t i = 0; i < scen.size(); ++i) {
    snprintf(label, sizeof(label), "ab_%02zu_%s", i, scen[i].name.c_str());
    const char* dl = g_dump_dir ? label : nullptr;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {                       // 子进程跑该 case
      bool ok = RunCaseOnce(scen[i], dl);
      fflush(stdout);
      _exit(ok ? 0 : 1);
    }
    int status = 0;
    signal(SIGALRM, [](int) {});
    alarm(kCaseTimeoutSec);
    pid_t r = waitpid(pid, &status, 0);
    bool timed_out = (r < 0);             // EINTR：alarm 到点打断
    alarm(0);
    if (timed_out) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      printf("case %s: TIMEOUT %ds (decoder under-emit? deadlock)\n", scen[i].name.c_str(),
             kCaseTimeoutSec);
    }
    bool pass = false;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) pass = true;
    else if (WIFSIGNALED(status))
      printf("case %s: killed by signal %d\n", scen[i].name.c_str(), WTERMSIG(status));
    if (pass) ++npass;
  }
  printf("==== %d/%zu cases PASS ====\n", npass, scen.size());
  return npass == int(scen.size()) ? 0 : 1;
}
