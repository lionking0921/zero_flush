// exp_putkey_data.cpp —— 隔离单测 putkey_data:穷举 (shared, index) 组合,
// 验证 key 后缀字节精确落在 bram[index .. index+unshared),与期望自然字节串逐字节一致。
#define HLS_STREAM_THREAD_SAFE
#include "../kernel/krnl_vadd.cpp"
#include <cstdio>
#include <cstring>

static std::string BramBytes(ap_uint<128>* b, size_t nw, size_t lo, size_t hi) {
  std::string s;
  for (size_t i = lo; i < hi; ++i) {
    size_t w = i >> 4, by = i & 15;
    s += char(uint8_t(b[w].range(by * 8 + 7, by * 8)));
  }
  return s;
}

int main() {
  // 构造已知 keystring:自然字节 00 01 .. 1f,length 32
  keystring key;
  key.length = 32;
  for (int b = 0; b < 32; ++b) {
    int ci = b >> 4, by = b & 15;
    key.c[ci].range(by * 8 + 7, by * 8) = b;
  }
  int fails = 0;
  for (uint32_t shared = 0; shared < 32; ++shared) {
    for (uint32_t index = 0; index < 80; ++index) {
      ap_uint<128> bram[16];
      for (auto& w : bram) w = 0;
      putkey_data(key, ap_uint<8>(shared), bram, ap_uint<16>(index));
      uint32_t un = 32 - shared;
      // 期望:bram 字节 [index, index+un) = key 自然字节 [shared, 32)
      bool ok = true;
      std::string got, exp;
      for (uint32_t k = 0; k < un; ++k) {
        uint8_t g = uint8_t(bram[(index + k) >> 4].range((((index + k) & 15) * 8) + 7, ((index + k) & 15) * 8));
        uint8_t e = uint8_t(shared + k);
        got += char(g); exp += char(e);
        if (g != e) ok = false;
      }
      // 确认写入区外没被污染
      for (uint32_t k = 0; k < index; ++k) {
        uint8_t g = uint8_t(bram[k >> 4].range(((k & 15) * 8) + 7, (k & 15) * 8));
        if (g != 0) ok = false;
      }
      if (!ok) {
        if (fails < 30) {
          printf("FAIL shared=%u index=%u (move=%d left_of=%u right_of=%u)\n  exp=%s\n  got=%s\n",
                 shared, index, int(int(shared & 15) - int(index & 15)), shared & 15, index & 15,
                 [&]{std::string h; for (unsigned char c : exp){char b[4];snprintf(b,4,"%02x",c);h+=b;} return h;}().c_str(),
                 [&]{std::string h; for (unsigned char c : got){char b[4];snprintf(b,4,"%02x",c);h+=b;} return h;}().c_str());
        }
        ++fails;
      }
    }
  }
  printf("total fails: %d / 2560\n", fails);
  return fails ? 1 : 0;
}
