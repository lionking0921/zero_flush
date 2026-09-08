// exp_layout.cpp —— 逐块解码：看坏记录是否为各块"首条/尾部重叠区" 且检查 handle 对齐
#define HLS_STREAM_THREAD_SAFE
#include "../kernel/krnl_vadd.cpp"
#include "../host/zf_sst_decode.h"
#include <cstdio>
using hls::stream;

static std::vector<ap_uint<32> > BytesToWords(const std::vector<uint8_t>& b) {
  std::vector<ap_uint<32> > w((b.size() + 3) / 4 + 2, 0);
  for (size_t i = 0; i < b.size(); ++i) w[i / 4].range((i % 4) * 8 + 7, (i % 4) * 8) = b[i];
  return w;
}
static std::vector<uint8_t> WordsToBytes128(const ap_uint<128>* w, size_t nbytes) {
  size_t nw = (nbytes + 15) / 16;
  std::vector<uint8_t> b(nbytes);
  for (size_t i = 0; i < nw; ++i)
    for (int j = 0; j < 16; ++j) { size_t o = i * 16 + j; if (o < nbytes) b[o] = uint8_t(w[i].range(j * 8 + 7, j * 8)); }
  return b;
}

int main(int argc, char** argv) {
  uint64_t seed = argc > 1 ? strtoull(argv[1], 0, 10) : 1;
  uint32_t ports = argc > 2 ? atoi(argv[2]) : 4;
  size_t base = argc > 3 ? atoi(argv[3]) : 60, dup = argc > 4 ? atoi(argv[4]) : 10;
  zf::Workload wl = zf::GenWorkload(seed, ports, base, dup);
  uint32_t kv_sum = wl.total_kv();
  std::vector<std::vector<uint8_t> > staged(4);
  std::vector<std::vector<ap_uint<32> > > words(4);
  for (uint32_t p = 0; p < 4; ++p) { if (p < wl.port.size()) staged[p] = wl.port[p].Staged(); words[p] = BytesToWords(staged[p]); }
  stream<fifo_key_meta> km_in[MAX_INPUT_FILE_NUM];
  stream<fifo_value_slice> val_in[MAX_INPUT_FILE_NUM];
  stream<fifo_key_meta> km_merged;
  stream<ap_uint<2> > merge_res;
  for (uint32_t p = 0; p < 4; ++p)
    decoder(words[p].data(), ap_uint<32>(p < wl.port.size() ? wl.port[p].kv() : 0),
            ap_uint<40>(p < wl.port.size() ? wl.port[p].wal_bytes() : 0), km_in[p], val_in[p]);
  std::thread th([&] { for (uint32_t i = 0; i <= kv_sum; ++i) merge(km_in, km_merged, merge_res); });
  ap_uint<40> fl = 1ull << 26;
  std::vector<ap_uint<128> > sst((1ull << 26) / 16 + 16), idx(index_block_buffer_size / 16 + 16);
  std::vector<uint64_t> out(PPS_KERNEL_SIZE + 8 + 8, 0);
  encoder(km_merged, val_in, merge_res, kv_sum, sst.data(), idx.data(), out.data(), fl);
  th.join();
  uint64_t top_sst = out[PPS_KERNEL_SIZE], top_idx = out[PPS_KERNEL_SIZE + MAX_OUTPUT_FILE_NUM];
  std::vector<uint8_t> data_b = WordsToBytes128(sst.data(), top_sst);
  std::vector<uint8_t> idx_b = WordsToBytes128(idx.data(), top_idx);

  printf("data=%llu blocks=%llu\n", (unsigned long long)top_sst, (unsigned long long)out[0]);
  auto handles = zfdecode::ParseIndexHandles(idx_b, out[0]);
  // 全部数据块逐条解码，但按块标记首条
  std::vector<zfdecode::OutRec> got;
  int gidx = 0;
  for (size_t b = 0; b < handles.size(); ++b) {
    uint64_t off = handles[b].first; uint32_t sz = handles[b].second;
    printf("block[%zu] off=%llu size=%u aligned16=%s\n", b, (unsigned long long)off, sz,
           (off % 16 == 0) ? "yes" : "NO");
    std::vector<uint8_t> blk(data_b.begin() + off, data_b.begin() + off + sz);
    size_t before = got.size();
    zfdecode::DecodeBlock(blk, got);
    for (size_t k = before; k < got.size(); ++k) {
      bool ok = (k + gidx < wl.expected.size()) &&
                got[k].rec.user == wl.expected[k + gidx].user &&
                got[k].rec.seq == wl.expected[k + gidx].seq &&
                got[k].rec.type == wl.expected[k + gidx].type;
      if (!ok || k == before) {
        printf("   rec#%zu (first=%d) %s exp seq=%016llx type=%d | got seq=%016llx type=%d\n",
               k + gidx, k == before ? 1 : 0, ok ? "ok" : "MISMATCH",
               (unsigned long long)(k + gidx < wl.expected.size() ? wl.expected[k + gidx].seq : ~0ull),
               (int)(k + gidx < wl.expected.size() ? wl.expected[k + gidx].type : -1),
               (unsigned long long)got[k].rec.seq, (int)got[k].rec.type);
      }
    }
    gidx += int(got.size() - before);
  }
  printf("decoded total=%d expect=%zu\n", gidx, wl.expected.size());
  // 对照：用 DecodeSST + CompareWorkload 再解一遍
  printf("=== CompareWorkload view ===\n");
  auto got2 = zfdecode::DecodeSST(data_b, idx_b, out.data(), size_t(top_sst));
  printf("DecodeSST total=%zu\n", got2.size());
  auto errs = zfdecode::CompareWorkload(wl, data_b, idx_b, out.data(), size_t(top_sst));
  for (auto& e : errs) printf("  FAIL %s\n", e.c_str());
  // 打印 block1 内 DecodeSST 给出的 rec（对比上面手解）
  printf("DecodeSST recs[0..15]:\n");
  for (size_t i = 0; i < got2.size() && i < 16; ++i)
    printf("  [%zu] seq=%016llx type=%d vlen=%zu\n", i, (unsigned long long)got2[i].rec.seq,
           (int)got2[i].rec.type, got2[i].rec.value.size());
  return 0;
}
