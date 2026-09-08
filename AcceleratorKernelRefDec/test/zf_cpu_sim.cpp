// zf_cpu_sim.cpp —— CPU 端整链数据流仿真：4×zf-decoder → merge → encoder
//
// 目的：在纯 C++ 下逐字镜像内核 DATAFLOW 拓扑，验证新的 zf-decoder（+ zf_* 解析器）
// 与 merge/encoder 协同后产出的 SST 与预言机一致 —— 这是 sw_emu/hw 之前的快速迭代环。
//
// 执行模型（利用 hls::stream 纯 C++ 语义：写无界、读空则阻塞于条件变量）：
//   * 主线程先把 4 个 decoder 依次跑完（把每口 km 流 + value 流全部喂满）；
//   * 起一个 merge 子线程：while(i++ <= kv_sum) merge(...)；—— 恰好 kv_sum+1 次调用
//     （encoder 需要 kv_sum 条真实记录 + 1 条尾哨兵 MAX，merge 第 kv_sum+1 次输出 MAX，
//      且最后一次拉流恰好把各口最后的 MAX 令牌读完，不会阻塞）；
//   * 主线程调用 encoder(...)，阻塞读 km 输出直到 merge 子线程产出；
//   * join merge 子线程，重建 data/index/PPS，逐条比对预言机。
//
// 用法：
//   g++ -std=c++17 -O1 -pthread -DHLS_STREAM_THREAD_SAFE \
//       -I ../kernel -I ../host -I /tools/Xilinx/Vitis_HLS/2022.2/include \
//       zf_cpu_sim.cpp -o /tmp/zf_cpu_sim && /tmp/zf_cpu_sim
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#define HLS_STREAM_THREAD_SAFE
#include "../kernel/krnl_vadd.cpp"   // 内核整 TU（decoder/merge/encoder/类型/常量）
#include "../host/zf_sst_decode.h"   // zf_format.h + host 解码比对

using hls::stream;

// 字节流 -> ap_uint<32> 字数组（LE），尾部补足够 padding 字：
// +2 防未对齐直读越界；+1024 使顺序窗预取读整窗（≤ wal+1023B）绝不越出 vector。
static std::vector<ap_uint<32> > BytesToWords(const std::vector<uint8_t>& b) {
  std::vector<ap_uint<32> > w((b.size() + 3) / 4 + 2 + 1024, 0);
  for (size_t i = 0; i < b.size(); ++i) {
    w[i / 4].range((i % 4) * 8 + 7, (i % 4) * 8) = b[i];
  }
  return w;
}
// ap_uint<128> 数组（n 字节）-> 字节流
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

struct DecArg {
  ap_uint<32>* buf;
  ap_uint<32> kv;
  ap_uint<40> file_size;
};

// 跑一个 workload；返回 true = 全绿。dump_path 非空时把该 case 的
// [PPS+data+index] 前缀与期望条目各自落盘（供引擎 zf_seal_check 封口/直读对拍）：
//   prefix 文件布局：8B magic "ZFPRFX1\n" + u64 data_size + u64 index_size
//                     + u64 num_deletions + PPS_KERNEL_SIZE×u64 + data 字节
//                     + index 字节
//   expect 文件布局：逐行 `32B内部键hex + tab + 值hex`（文件内部键序，去重后每 user 一条）
static bool RunCase(uint64_t seed, uint32_t port_count, size_t base_keys, size_t dup_keys,
                    const char* dump_path = nullptr) {
  zf::Workload wl = zf::GenWorkload(seed, port_count, base_keys, dup_keys);
  uint32_t kv_sum = wl.total_kv();

  // 每口 staged 缓冲 + 参数（口 0..3；空口 file_size=0,kv=0）
  std::vector<std::vector<uint8_t> > staged(4);
  std::vector<std::vector<ap_uint<32> > > words(4);
  DecArg arg[4];
  for (uint32_t p = 0; p < 4; ++p) {
    if (p < wl.port.size()) {
      staged[p] = wl.port[p].Staged();
    }
    words[p] = BytesToWords(staged[p]);
    arg[p].buf = words[p].data();
    arg[p].file_size = ap_uint<40>(p < wl.port.size() ? wl.port[p].wal_bytes() : 0);
    arg[p].kv = ap_uint<32>(p < wl.port.size() ? wl.port[p].kv() : 0);
  }

  // ---- 流 ----
  stream<fifo_key_meta> km_in[MAX_INPUT_FILE_NUM];
  stream<fifo_value_slice> val_in[MAX_INPUT_FILE_NUM];
  stream<fifo_key_meta> km_merged;
  stream<ap_uint<2> > merge_res;

  // 1) 主线程先把 4 个 decoder 全部跑完（写无界流，不会阻塞）
  for (uint32_t p = 0; p < MAX_INPUT_FILE_NUM; ++p)
    decoder(arg[p].buf, arg[p].kv, arg[p].file_size, km_in[p], val_in[p]);

  // 2) merge 子线程：kv_sum+1 次（含尾哨兵 MAX）
  std::atomic<bool> merge_done(false);
  std::thread merge_th([&] {
    for (uint32_t i = 0; i <= kv_sum; ++i) merge(km_in, km_merged, merge_res);
    merge_done = true;
  });

  // 3) encoder 主线程消费
  const uint64_t file_limit_u = 1ull << 26;
  ap_uint<40> file_limit = file_limit_u;
  std::vector<ap_uint<128> > sst( (file_limit_u / 16) + 16 );          // 64MB+
  std::vector<ap_uint<128> > idx( (index_block_buffer_size / 16) + 16 );
  std::vector<uint64_t> out(PPS_KERNEL_SIZE + 2 * MAX_OUTPUT_FILE_NUM + 8, 0);
  encoder(km_merged, val_in, merge_res, kv_sum, sst.data(), idx.data(), out.data(), file_limit);
  merge_th.join();

  uint64_t top_sst = out[PPS_KERNEL_SIZE];
  uint64_t top_idx = out[PPS_KERNEL_SIZE + MAX_OUTPUT_FILE_NUM];
  uint64_t file_num = out[PPS_KERNEL_SIZE + 2 * MAX_OUTPUT_FILE_NUM];

  std::vector<uint8_t> data_b = WordsToBytes128(sst.data(), size_t(top_sst));
  std::vector<uint8_t> idx_b = WordsToBytes128(idx.data(), size_t(top_idx));

  // ---- (里程碑 3) 供引擎 harness 的转储：prefix + expect ----
  if (dump_path) {
    auto hexes = [](const std::string& s) {
      std::string h;
      char buf[4];
      for (size_t i = 0; i < s.size(); ++i) {
        snprintf(buf, 4, "%02x", (unsigned char)s[i]);
        h += buf;
      }
      return h;
    };
    std::string pfx = std::string(dump_path) + ".prefix";
    std::string exp = std::string(dump_path) + ".expect";
    FILE* f = fopen(pfx.c_str(), "wb");
    if (!f) { printf("dump open fail %s\n", pfx.c_str()); return false; }
    fwrite("ZFPRFX1\n", 1, 8, f);
    uint64_t ndel = 0;
    for (const auto& r : wl.expected) ndel += r.IsDeletion() ? 1 : 0;
    uint64_t hdr[3] = {top_sst, top_idx, ndel};
    fwrite(hdr, 8, 3, f);
    fwrite(out.data(), sizeof(uint64_t), PPS_KERNEL_SIZE, f);
    fwrite(data_b.data(), 1, data_b.size(), f);
    fwrite(idx_b.data(), 1, idx_b.size(), f);
    fclose(f);
    FILE* e = fopen(exp.c_str(), "wb");
    if (!e) { printf("dump open fail %s\n", exp.c_str()); return false; }
    for (const auto& r : wl.expected) {
      std::string ik = zf::InternalKey(r.user, r.seq, r.type);
      std::string line = hexes(ik) + "\t" + hexes(r.value) + "\n";
      fwrite(line.data(), 1, line.size(), e);
    }
    fclose(e);
    printf("  dumped %s (+ .expect)\n", pfx.c_str());
  }

  std::string head;
  for (size_t i = 0; i < 8; ++i)
    head += zf::InternalKey(wl.expected[i].user, wl.expected[i].seq, wl.expected[i].type)
                .substr(0, 2) + "..";
  printf("case seed=%llu ports=%u base=%zu dup=%zu kv_sum=%u expect=%zu file_num=%llu "
         "data=%llu idx=%llu\n",
         (unsigned long long)seed, port_count, base_keys, dup_keys, kv_sum,
         wl.expected.size(), (unsigned long long)file_num, (unsigned long long)top_sst,
         (unsigned long long)top_idx);

  if (file_num != 1) { printf("  FAIL file_num=%llu (expected 1)\n", (unsigned long long)file_num); return false; }
  auto errs = zfdecode::CompareWorkload(wl, data_b, idx_b, out.data(), size_t(top_sst));
  if (!errs.empty()) {
    for (const auto& e : errs) printf("  FAIL %s\n", e.c_str());
    // 明细：把失配处 got/expected 内部键（hex）+ vlen 打全
    auto got = zfdecode::DecodeSST(data_b, idx_b, out.data(), size_t(top_sst));
    auto hex = [](const std::string& s, size_t n) {
      std::string h;
      char buf[4];
      for (size_t i = 0; i < n && i < s.size(); ++i) { snprintf(buf, 4, "%02x", (unsigned char)s[i]); h += buf; }
      return h;
    };
    for (size_t i = 0; i < wl.expected.size() && i < got.size(); ++i) {
      const auto& e = wl.expected[i]; const auto& g = got[i].rec;
      if (!(e.user == g.user && e.seq == g.seq && e.type == g.type && e.value == g.value)) {
        printf("    [%zu] exp ik=%s.. type=%d vlen=%zu | got ik=%s.. type=%d vlen=%zu\n",
               i, hex(zf::InternalKey(e.user, e.seq, e.type), 32).c_str(), (int)e.type, e.value.size(),
               hex(got[i].ik, 32).c_str(), (int)g.type, g.value.size());
        // 反查该 user 的全部版本（各口 entries 内）
        printf("       versions of this user:");
        for (const auto& P : wl.port)
          for (const auto& en : P.entries)
            if (en.rec.user == g.user)
              printf(" (port%s seq=%016llx type=%d)",
                     (P.part ? "?" : ""), (unsigned long long)en.rec.seq, (int)en.rec.type);
        printf("\n");
      }
    }
    return false;
  }
  printf("  OK  %zu records match oracle\n", wl.expected.size());
  return true;
}

int main(int argc, char** argv) {
  // (seed, ports, base, dup) —— 覆盖 1/2/3/4 口、跨口同键遮蔽、删除标记、短值
  struct C { uint64_t seed; uint32_t ports; size_t base, dup; };
  std::vector<C> cases;
  const char* dump = nullptr;
  if (argc > 1) {  // 允许外部传单组：seed ports base dup [dump_path]
    cases.push_back({strtoull(argv[1], nullptr, 10), uint32_t(atoi(argv[2])),
                     size_t(atoi(argv[3])), size_t(atoi(argv[4]))});
    if (argc > 5) dump = argv[5];
  } else {
    cases = {{1, 4, 200, 30}, {2, 1, 300, 0},  {3, 3, 400, 50}, {4, 2, 150, 40},
             {5, 4, 60, 10},  {6, 2, 500, 70}, {7, 3, 90, 20},  {8, 4, 1000, 120}};
  }
  int npass = 0;
  for (const auto& c : cases)
    if (RunCase(c.seed, c.ports, c.base, c.dup, dump)) ++npass;
  printf("==== %d/%zu cases PASS ====\n", npass, cases.size());
  return npass == int(cases.size()) ? 0 : 1;
}
