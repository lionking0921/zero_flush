// exp_decoder.cpp —— 单独跑 4 个 decoder，把发出的 km 内部键与各口 slim 条目逐字比对
#define HLS_STREAM_THREAD_SAFE
#include "../kernel/krnl_vadd.cpp"
#include "../host/zf_format.h"
#include <cstdio>
#include <string>
#include <vector>
using hls::stream;

static std::vector<ap_uint<32> > BytesToWords(const std::vector<uint8_t>& b) {
  std::vector<ap_uint<32> > w((b.size() + 3) / 4 + 2, 0);
  for (size_t i = 0; i < b.size(); ++i) w[i / 4].range((i % 4) * 8 + 7, (i % 4) * 8) = b[i];
  return w;
}
static std::string KeyToBytes(const keystring& k) {
  std::string s;
  size_t n = k.length.to_uint();
  for (size_t i = 0; i < n; ++i) {
    size_t ci = i / 16, b = i % 16;
    s += char(uint8_t(k.c[ci].range(b * 8 + 7, b * 8)));
  }
  return s;
}

int main(int argc, char** argv) {
  uint64_t seed = argc > 1 ? strtoull(argv[1], 0, 10) : 1;
  uint32_t ports = argc > 2 ? atoi(argv[2]) : 4;
  size_t base = argc > 3 ? atoi(argv[3]) : 60, dup = argc > 4 ? atoi(argv[4]) : 10;
  zf::Workload wl = zf::GenWorkload(seed, ports, base, dup);

  for (uint32_t p = 0; p < MAX_INPUT_FILE_NUM; ++p) {
    bool has = p < wl.port.size();
    auto staged = has ? wl.port[p].Staged() : std::vector<uint8_t>();
    auto words = BytesToWords(staged);
    stream<fifo_key_meta> km;
    stream<fifo_value_slice> val;
    decoder(words.data(), ap_uint<32>(has ? wl.port[p].kv() : 0),
            ap_uint<40>(has ? wl.port[p].wal_bytes() : 0), km, val);

    int n = has ? int(wl.port[p].kv()) : 0;
    printf("=== port %u n=%d ===\n", p, n);
    bool sig = is_signal_key(km.read().key);
    if (!sig) printf("  !! first not SIGNAL\n");
    int bad = 0;
    for (int i = 0; i < n; ++i) {
      fifo_key_meta m = km.read();
      std::string got = KeyToBytes(m.key);
      std::string exp = zf::InternalKey(wl.port[p].entries[i].rec.user,
                                        wl.port[p].entries[i].rec.seq,
                                        wl.port[p].entries[i].rec.type);
      uint32_t evl = uint32_t(wl.port[p].entries[i].rec.value.size());
      bool ok = (got == exp) && (m.value_length.to_uint() == evl);
      if (!ok) {
        ++bad;
        auto hx = [](const std::string& s) {
          std::string h; char b[4];
          for (size_t k = 0; k < s.size(); ++k) { snprintf(b, 4, "%02x", (unsigned char)s[k]); h += b; }
          return h;
        };
        printf("  [%d] MISMATCH exp=%s got=%s vl(%u vs %u)\n", i, hx(exp).c_str(),
               hx(got).c_str(), evl, m.value_length.to_uint());
      }
    }
    // 流尾应为 MAX
    fifo_key_meta tail = km.read();
    printf("  tail is_max=%d, %d mismatch\n", is_max_key(tail.key) ? 1 : 0, bad);
  }
  return 0;
}
