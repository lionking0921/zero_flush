// exp_tombstone.cpp —— 手工构造极小 workload，隔离 encoder 对删除标记(空值/type0)的落盘行为
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#define HLS_STREAM_THREAD_SAFE
#include "../kernel/krnl_vadd.cpp"
#include "../host/zf_format.h"
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
static std::string MkUser(uint8_t v) { return std::string(24, char(v)); }

static void Run(const char* tag, zf::Workload& wl) {
  uint32_t kv_sum = wl.total_kv();
  std::vector<std::vector<uint8_t> > staged(4);
  std::vector<std::vector<ap_uint<32> > > words(4);
  ap_uint<32>* bufs[4];
  ap_uint<40> fsz[4];
  for (uint32_t p = 0; p < 4; ++p) {
    if (p < wl.port.size()) staged[p] = wl.port[p].Staged();
    words[p] = BytesToWords(staged[p]);
    bufs[p] = words[p].data();
    fsz[p] = ap_uint<40>(p < wl.port.size() ? wl.port[p].wal_bytes() : 0);
  }
  stream<fifo_key_meta> km_in[MAX_INPUT_FILE_NUM];
  stream<fifo_value_slice> val_in[MAX_INPUT_FILE_NUM];
  stream<fifo_key_meta> km_merged;
  stream<ap_uint<2> > merge_res;
  for (uint32_t p = 0; p < MAX_INPUT_FILE_NUM; ++p)
    decoder(bufs[p], ap_uint<32>(p < wl.port.size() ? wl.port[p].kv() : 0), fsz[p], km_in[p], val_in[p]);
  std::thread th([&] { for (uint32_t i = 0; i <= kv_sum; ++i) merge(km_in, km_merged, merge_res); });
  ap_uint<40> fl = 1ull << 26;
  std::vector<ap_uint<128> > sst((1ull << 26) / 16 + 16), idx(index_block_buffer_size / 16 + 16);
  std::vector<uint64_t> out(PPS_KERNEL_SIZE + 8 + 8, 0);
  encoder(km_merged, val_in, merge_res, kv_sum, sst.data(), idx.data(), out.data(), fl);
  th.join();
  uint64_t data_len = out[PPS_KERNEL_SIZE], idx_len = out[PPS_KERNEL_SIZE + MAX_OUTPUT_FILE_NUM];
  std::vector<uint8_t> db = WordsToBytes128(sst.data(), data_len);
  // 打印所有数据块记录头 + footer，粗览
  printf("[%s] kv_sum=%u data=%llu\n", tag, kv_sum, (unsigned long long)data_len);
  size_t p0 = 0;
  while (p0 < db.size()) {
    size_t num = 0;
    if (db.size() - p0 < 4) break;
    for (int i = 0; i < 4; ++i) num |= uint32_t(db[db.size() - 4 + i]) << (8 * i);
    printf("  raw-data-bytes[0..40]: ");
    for (size_t i = p0; i < db.size() && i < p0 + 40; ++i) printf("%02x", db[i]);
    printf("\n");
    break;  // 只看头部即可
  }
}

int main() {
  auto U = MkUser(0x41);
  {
    zf::Workload wl; zf::Port P; P.part = 1; P.gen = 1;
    // 单条 deletion，seq=5
    zf::KRec d; d.user = U; d.seq = 5; d.type = zf::kTypeDeletion;
    P.entries.push_back({d, 0});
    auto fr = zf::BuildZfRecord(1, d.type, 0, d.user, d.value, d.seq);
    P.wal = fr; P.slim.clear();
    std::string ik = zf::InternalKey(d.user, d.seq, d.type);
    zf::PutVarint32(P.slim, 32); P.slim.insert(P.slim.end(), ik.begin(), ik.end());
    zf::PutVarint32(P.slim, 16);
    zf::PutFixed32LE(P.slim, 1); zf::PutFixed32LE(P.slim, 1); zf::PutFixed64LE(P.slim, 0);
    wl.port.push_back(P);
    // oracle: 应保留该 deletion（仅此一条）——内部键 footer=(5<<8|0)
    wl.expected.push_back(d);
    Run("del-only-seq5", wl);
  }
  {
    zf::Workload wl; zf::Port P; P.part = 2; P.gen = 2;
    // 同 user 两条：value seq=10（旧）与 deletion seq=20（新）；order: del(20) 先
    zf::KRec v; v.user = U; v.seq = 10; v.type = zf::kTypeValue; v.value = std::string(4, 'V');
    zf::KRec d; d.user = U; d.seq = 20; d.type = zf::kTypeDeletion;
    // entries 必须已按内部键序：user 同 seq 降序 => d(20) 在前
    P.entries.push_back({d, 0}); P.entries.push_back({v, 0});
    uint64_t off = 0;
    for (auto& e : P.entries) {
      e.wal_offset = off;
      auto fr = zf::BuildZfRecord(2, e.rec.type, 0, e.rec.user, e.rec.value, e.rec.seq);
      P.wal.insert(P.wal.end(), fr.begin(), fr.end());
      off += fr.size();
    }
    for (auto& e : P.entries) {
      std::string ik = zf::InternalKey(e.rec.user, e.rec.seq, e.rec.type);
      zf::PutVarint32(P.slim, 32); P.slim.insert(P.slim.end(), ik.begin(), ik.end());
      zf::PutVarint32(P.slim, 16);
      zf::PutFixed32LE(P.slim, 2); zf::PutFixed32LE(P.slim, 2); zf::PutFixed64LE(P.slim, e.wal_offset);
    }
    wl.port.push_back(P);
    wl.expected.push_back(d);   // 新版本 deletion 保留
    Run("del-new(20)+val-old(10)", wl);
  }
  return 0;
}
