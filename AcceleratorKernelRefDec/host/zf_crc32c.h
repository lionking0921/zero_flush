// ZeroFlush FPGA 加速器 —— CRC-32C（Castagnoli 0x82F63B78）host 参考实现（纯 C++，头文件内联）
//
// 语义与引擎（../source/rocksdb-zeroflush/util/crc32c.cc 的 ExtendImpl）逐字节一致：
//   Value(data,n) = Extend(0, data, n) —— 反射算法，init/xorout 均 0xFFFFFFFF；
//   Extend(prev, ...) 的 prev 为上一次完成值（链式追加数据段）；
//   Mask(x) = rotl32(x,17) + 0xa282ead8；Unmask 为其逆。
//   SST 每块 5B trailer 的 checksum = Mask( ExtendByte(Value(块内容), 0x00) )，
//   其中 0x00 = 压缩类型字节（kNoCompression），与引擎 ComputeBuiltinChecksumWithLastByte 一致。
//
// 提供两种算法（表驱动 + 逐位）互相校验，确保与 kernel 内嵌实现（AcceleratorKernelSstV2
// kernel/krnl_vadd.cpp 内的 zf_crc32c 区）结果一致。头文件内联，仅 host/test 使用（kernel 不用）。
#ifndef ZF_CRC32C_H
#define ZF_CRC32C_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace zfcrc {

constexpr uint32_t kPoly = 0x82F63B78u;         // CRC-32C（Castagnoli，反射）
constexpr uint32_t kMaskDelta = 0xa282ead8u;

// 反射查表：tbl[i] 为初态 i 的 8 位尾更新表
inline const uint32_t* Table() {
  static uint32_t t[256] = {0};
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1) ? kPoly : 0);
      t[i] = crc;
    }
    init = true;
  }
  return t;
}

// 表驱动：把字节段追加到"已完成值"prev 之后，返回新完成值。
inline uint32_t Extend(uint32_t prev, const uint8_t* data, size_t n) {
  const uint32_t* tab = Table();
  uint32_t l = prev ^ 0xffffffffu;  // 引擎 ExtendImpl：先撤销 xorout
  for (size_t i = 0; i < n; ++i) {
    uint32_t c = (l & 0xff) ^ data[i];
    l = tab[c] ^ (l >> 8);
  }
  return l ^ 0xffffffffu;
}
inline uint32_t Extend(uint32_t prev, const char* data, size_t n) {
  return Extend(prev, reinterpret_cast<const uint8_t*>(data), n);
}
inline uint32_t ExtendByte(uint32_t prev, uint8_t b) { return Extend(prev, &b, 1); }
inline uint32_t Value(const uint8_t* data, size_t n) { return Extend(0, data, n); }
inline uint32_t Value(const char* data, size_t n) { return Value(reinterpret_cast<const uint8_t*>(data), n); }

// 逐位参考（用于交叉验证表驱动）
inline uint32_t ValueBitwise(const uint8_t* data, size_t n) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1) ? kPoly : 0);
  }
  return crc ^ 0xffffffffu;
}

inline uint32_t Rotl17(uint32_t x) { return (x << 17) | (x >> 15); }
inline uint32_t Mask(uint32_t crc) { return Rotl17(crc) + kMaskDelta; }
inline uint32_t Unmask(uint32_t masked) {
  uint32_t rot = masked - kMaskDelta;
  return (rot >> 17) | (rot << 15);  // rotl17 的逆 = rotr17
}

// 单块 checksum = Mask( Extend(Value(content), type_byte=0x00) ) —— §14.6 kCRC32c 语义
inline uint32_t BlockChecksum(const uint8_t* content, size_t n) {
  return Mask(ExtendByte(Value(content, n), 0x00));
}
inline uint32_t BlockChecksum(const char* content, size_t n) {
  return BlockChecksum(reinterpret_cast<const uint8_t*>(content), n);
}

}  // namespace zfcrc
#endif  // ZF_CRC32C_H
