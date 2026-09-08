// encoder_dump.cpp —— 把未改动的参考 encoder 在纯 C++ 下跑一遍，转储其落盘字节，
// 用于确认 data block / index block / PPS 的确切字节几何（host 解码的事实依据）。
//
// 用法：
//   g++ -std=c++17 -I ../kernel -I /tools/Xilinx/Vitis_HLS/2022.2/include \
//       encoder_dump.cpp -o /tmp/encoder_dump && /tmp/encoder_dump
#include "../kernel/krnl_vadd.cpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// 把一个 32B internal key 的字节填成 keystring（c[i] 自然字节镜像）
static keystring MakeKey(const uint8_t k[32]) {
  keystring ks;
  ks.length = KEY_LENGTH;  // 32
  for (int i = 0; i < KEY_ARRAY_LENGTH; ++i) {
    ks.c[i] = 0;
    for (int b = 0; b < 16; ++b) {
      ks.c[i].range(b * 8 + 7, b * 8) = k[16 * i + b];
    }
  }
  return ks;
}

static void PutU64LE(std::vector<uint8_t>& v, uint64_t x) {
  for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
static void PutU32LE(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

// ap_uint<128> 数组 → 字节流（little-endian）
static std::vector<uint8_t> WordsToBytes(const ap_uint<128>* w, size_t nwords) {
  std::vector<uint8_t> b(nwords * 16);
  for (size_t i = 0; i < nwords; ++i)
    for (int j = 0; j < 16; ++j) b[i * 16 + j] = uint8_t(w[i].range(j * 8 + 7, j * 8));
  return b;
}

static void HexDump(const std::vector<uint8_t>& b, size_t from, size_t n, const char* tag) {
  printf("== %s (%zu..%zu) ==\n", tag, from, from + n);
  for (size_t i = from; i < from + n && i < b.size(); ++i) {
    printf("%02x ", b[i]);
    if ((i - from) % 16 == 15) printf("\n");
  }
  printf("\n");
}

int main() {
  // ---- 3 条内部键记录，升序 ----
  struct R { uint8_t k[32]; uint32_t vlen; uint8_t seed; };
  std::vector<R> recs;
  const uint8_t user_a[24] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,
                              0xCC,0xDD,0xEE,0xFF,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17};
  const uint8_t user_b[24] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,
                              0xCC,0xDD,0xEE,0xFF,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x18};
  const uint8_t user_c[24] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,
                              0xCC,0xDD,0xEE,0xFF,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x19};
  auto push = [&](const uint8_t user[24], uint64_t seq, uint32_t vlen, uint8_t seed) {
    R r; memcpy(r.k, user, 24);
    uint64_t packed = (seq << 8) | 1;  // type = kTypeValue
    for (int i = 0; i < 8; ++i) r.k[24 + i] = uint8_t(packed >> (8 * i));
    r.vlen = vlen; r.seed = seed;
    recs.push_back(r);
  };
  push(user_a, 100, 1024, 0xA0);
  push(user_b, 200, 5, 0xB0);     // 短值：测试 value-length varint 1 字节分支
  push(user_c, 300, 1024, 0xC0);

  hls::stream<fifo_key_meta> input_km;
  hls::stream<fifo_value_slice> input_value[4];
  hls::stream<ap_uint<2> > merge_result;

  const uint32_t kv_sum = recs.size();
  for (auto& r : recs) {
    fifo_key_meta km;
    km.key = MakeKey(r.k);
    km.value_length = r.vlen;
    input_km.write(km);
    merge_result.write(0);   // 都来自 decoder0
    // 2 片 512B value（≤1024），按 1024 全量给，短值后半零填充
    std::vector<uint8_t> vb(1024, 0);
    for (uint32_t i = 0; i < r.vlen; ++i) vb[i] = uint8_t(r.seed + i);
    for (int s = 0; s < VALUE_LENGTH / 512; ++s) {
      fifo_value_slice sl;
      for (int w = 0; w < 32; ++w) {
        ap_uint<128> t = 0;
        for (int j = 0; j < 16; ++j)
          t.range(j * 8 + 7, j * 8) = vb[s * 512 + w * 16 + j];
        sl.c[w] = t;
      }
      input_value[0].write(sl);
    }
  }
  // encoder 在循环后还要吞一个 MAX 哨兵（读 km + result）
  {
    fifo_key_meta mx;
    set_max_key(mx.key);
    input_km.write(mx);
    merge_result.write(0);
  }

  // 输出缓冲
  const size_t sst_words = (1u << 20);            // 16MB words
  std::vector<ap_uint<128> > sst(sst_words);
  const size_t idx_words = (index_block_buffer_size / 16);
  std::vector<ap_uint<128> > idx(idx_words);
  std::vector<uint64_t> out(PPS_KERNEL_SIZE + 2 * MAX_OUTPUT_FILE_NUM + 8, 0);

  ap_uint<40> file_limit = 1 << 24;   // 16MB 单文件上限，逼单输出文件
  encoder(input_km, input_value, merge_result, kv_sum, sst.data(), idx.data(), out.data(), file_limit);

  printf("kv_sum=%u\n", kv_sum);
  // ---- output_data (PPS + 尾部) ----
  printf("PPS[0..17]: ");
  for (int i = 0; i < 18; ++i) printf("%llu ", (unsigned long long)out[i]);
  printf("\n");
  printf("top_sst_index[0]=%llu top_index_len[0]=%llu output_file_num=%llu\n",
         (unsigned long long)out[PPS_KERNEL_SIZE],
         (unsigned long long)out[PPS_KERNEL_SIZE + MAX_OUTPUT_FILE_NUM],
         (unsigned long long)out[PPS_KERNEL_SIZE + 2 * MAX_OUTPUT_FILE_NUM]);

  uint64_t data_len = out[PPS_KERNEL_SIZE];             // 数据区字节长
  uint64_t idx_off  = out[PPS_INDEXBLOCK_OFFSET_OFF];   // 应 == data_len
  printf("idx_offset(PPS5)=%llu\n", (unsigned long long)idx_off);

  std::vector<uint8_t> data_b = WordsToBytes(sst.data(), (data_len + 15) / 16);
  data_b.resize(data_len);
  uint64_t idx_len = out[PPS_KERNEL_SIZE + MAX_OUTPUT_FILE_NUM];
  std::vector<uint8_t> idx_b = WordsToBytes(idx.data(), (idx_len + 15) / 16);
  idx_b.resize(idx_len);

  HexDump(data_b, 0, 96, "data bytes");
  HexDump(idx_b, 0, 96, "index bytes");
  return 0;
}
