// ZeroFlush FPGA 加速器 —— host 端内核输出解码 + 预言机比对（纯 C++）
//
// 依据 encoder 落盘几何（经 encoder_dump 实证）：
//   * 数据区（字节流）= [block_0][5B 块尾trailer][block_1]... 其中 block_i 的
//     handle(size) 不含 trailer；块内尾 = restart偏移[n×u32] + num u32（在 size-4），
//     记录区 = [0, size-(1+num)*4)。
//   * 数据块记录 = [shared varint][unshared varint][vlen varint][key 后缀][value]，
//     前缀压缩、running key 逐条重组，块首 shared 必为 0。
//   * index 块 = [entry...][restart 偏移[n×u32]][num u32 @ len-9][5B trailer]；
//     entry = [shared varint][unshared varint][key(unshared 字节)][offset varint64]
//             [size varint32]；key 即该块最后键 bytes0..7。
//   * PPS(file0) 位于 output_data[0..128)：entries@1、datasize@2、rawkeysize@3、
//     rawvaluesize@4、indexoffset@5、minseq@6、maxseq@7、smallestlen@8、largestlen@9、
//     smallest key@10..13、largest key@14..17（每键 4×u64=32B 自然序）。
//     output_data[512]=top_sst_index[0]（数据区字节长）、[516]=index 区字节长。
#ifndef ZF_SST_DECODE_H
#define ZF_SST_DECODE_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "zf_format.h"

namespace zfdecode {

// ---- 字节流工具 ----
inline uint32_t Varint32(const std::vector<uint8_t>& b, size_t& p) {
  uint32_t r = 0;
  int sh = 0;
  while (true) {
    uint8_t x = b.at(p++);
    r |= uint32_t(x & 0x7f) << sh;
    if (!(x & 0x80)) break;
    sh += 7;
    if (sh >= 35) throw std::runtime_error("varint32 overflow");
  }
  return r;
}
inline uint64_t Varint64(const std::vector<uint8_t>& b, size_t& p) {
  uint64_t r = 0;
  int sh = 0;
  while (true) {
    uint8_t x = b.at(p++);
    r |= uint64_t(x & 0x7f) << sh;
    if (!(x & 0x80)) break;
    sh += 7;
    if (sh >= 70) throw std::runtime_error("varint64 overflow");
  }
  return r;
}
inline uint32_t U32LE(const std::vector<uint8_t>& b, size_t o) {
  return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) |
         (uint32_t(b[o + 3]) << 24);
}

// 解码出的文件内记录（内存态，与预言机 zf::KRec 逐字段可比）
struct OutRec {
  zf::KRec rec;   // user/seq/type/value
  std::string ik; // 完整 32B 内部键（自然字节）
};

// 从 index 区解析出每个数据块的 (offset, size)
inline std::vector<std::pair<uint64_t, uint32_t>> ParseIndexHandles(
    const std::vector<uint8_t>& idx, size_t num_blocks) {
  if (idx.size() < 9) throw std::runtime_error("index region too small");
  uint32_t nrest = U32LE(idx, idx.size() - 9);  // num u32（trailer 前 4 字节）
  // 记录区结束 = idx.len - 5(trailer) - 4(num) - 4*nrest
  size_t entries_end = idx.size() - 5 - 4 - 4 * size_t(nrest);
  std::vector<std::pair<uint64_t, uint32_t>> out;
  size_t p = 0;
  while (out.size() < num_blocks) {
    if (p >= entries_end) throw std::runtime_error("index entries ran out early");
    (void)Varint32(idx, p);   // shared
    uint32_t unshared = Varint32(idx, p);
    p += unshared;            // 跳过 key（encoder 恒 8 字节）
    uint64_t off = Varint64(idx, p);
    uint32_t size = Varint32(idx, p);
    out.emplace_back(off, size);
  }
  return out;
}

// 解码数据块；block = 单个块内容字节（长度=handle size，不含块尾 trailer）
inline void DecodeBlock(const std::vector<uint8_t>& block, std::vector<OutRec>& recs) {
  size_t S = block.size();
  uint32_t nrest = U32LE(block, S - 4);   // num u32
  size_t end = S - (size_t(1) + nrest) * 4;  // 记录区结束
  size_t p = 0;
  std::string runkey;  // 32B running key
  while (p < end) {
    uint32_t shared = Varint32(block, p);
    uint32_t unshared = Varint32(block, p);
    uint32_t vlen = Varint32(block, p);
    // 前缀重组（running key；块首记录 shared=0 → 整键）
    std::string key = runkey.substr(0, shared) +
                      std::string(block.begin() + p, block.begin() + p + unshared);
    p += unshared;
    std::string value(block.begin() + p, block.begin() + p + vlen);
    p += vlen;
    runkey = key;
    if (key.size() != 32) throw std::runtime_error("unexpected key length in data block");
    OutRec r;
    r.ik = key;
    r.rec.user.assign(key, 0, 24);
    uint64_t footer = 0;
    for (int i = 7; i >= 0; --i) footer = (footer << 8) | uint8_t(key[24 + i]);
    r.rec.type = uint8_t(footer & 0xff);
    r.rec.seq = footer >> 8;
    r.rec.value = std::move(value);
    recs.push_back(std::move(r));
  }
  if (p != end) throw std::runtime_error("data block decode did not reach block end");
}

// 整体解码：data 为数据区字节（长 top_sst），idx 为 index 区字节。
inline std::vector<OutRec> DecodeSST(const std::vector<uint8_t>& data,
                                     const std::vector<uint8_t>& idx,
                                     const uint64_t pps[128], size_t top_sst) {
  size_t num_blocks = size_t(pps[0]);
  auto handles = ParseIndexHandles(idx, num_blocks);
  std::vector<OutRec> recs;
  for (auto& h : handles) {
    uint64_t off = h.first;
    uint32_t size = h.second;
    if (off + size > data.size() && top_sst > 0 && off + size > top_sst)
      throw std::runtime_error("block handle out of data region");
    std::vector<uint8_t> block(data.begin() + off, data.begin() + off + size);
    DecodeBlock(block, recs);
  }
  return recs;
}

// 与预言机逐条比对；返回问题描述列表（空 = 全绿）
inline std::vector<std::string> CompareWorkload(const zf::Workload& wl,
                                                const std::vector<uint8_t>& data,
                                                const std::vector<uint8_t>& idx,
                                                const uint64_t pps[128], size_t top_sst) {
  std::vector<std::string> errs;
  auto got = DecodeSST(data, idx, pps, top_sst);

  // ---- 记录条数与逐条 KV 比对 ----
  if (got.size() != wl.expected.size()) {
    errs.push_back("record count: got " + std::to_string(got.size()) + " expected " +
                   std::to_string(wl.expected.size()));
  }
  size_t n = std::min(got.size(), wl.expected.size());
  for (size_t i = 0; i < n; ++i) {
    const zf::KRec& e = wl.expected[i];
    const zf::KRec& g = got[i].rec;
    bool same = (e.user == g.user) && (e.seq == g.seq) && (e.type == g.type) &&
                (e.value == g.value);
    if (!same) {
      std::string msg = "rec[" + std::to_string(i) + "] mismatch: expected(" +
                        zf::InternalKey(e.user, e.seq, e.type).substr(0, 8) + ",seq " +
                        std::to_string(e.seq) + ",type " + std::to_string(e.type) +
                        ",vlen " + std::to_string(e.value.size()) + ") got(user " +
                        std::to_string(g.user.size()) + " bytes,seq " + std::to_string(g.seq) +
                        ",type " + std::to_string(g.type) + ",vlen " +
                        std::to_string(g.value.size()) + ")";
      errs.push_back(msg);
      if (errs.size() > 8) break;
    }
  }

  // ---- PPS 抽查 ----
  if (pps[1] != wl.expected.size())
    errs.push_back("PPS.entries " + std::to_string(pps[1]) + " != " +
                   std::to_string(wl.expected.size()));
  if (wl.expected.empty()) {
    errs.push_back("workload expected empty");
    return errs;
  }
  // 内部键序首/末、seq 极值
  const zf::KRec& first = wl.expected.front();
  const zf::KRec& last = wl.expected.back();
  std::string ik0 = zf::InternalKey(first.user, first.seq, first.type);
  std::string ik1 = zf::InternalKey(last.user, last.seq, last.type);
  // smallest key 从 pps[10..13]（每 u64 LE）
  uint8_t sk[32];
  for (int i = 0; i < 4; ++i)
    for (int b = 0; b < 8; ++b) sk[i * 8 + b] = uint8_t(pps[10 + i] >> (8 * b));
  if (memcmp(sk, ik0.data(), 32) != 0)
    errs.push_back("PPS.smallest key mismatch");
  uint8_t lk[32];
  for (int i = 0; i < 4; ++i)
    for (int b = 0; b < 8; ++b) lk[i * 8 + b] = uint8_t(pps[14 + i] >> (8 * b));
  if (memcmp(lk, ik1.data(), 32) != 0)
    errs.push_back("PPS.largest key mismatch");
  uint64_t expect_min = (first.seq << 8) | first.type;
  uint64_t expect_max = (last.seq << 8) | last.type;
  // 极值应为全体 seq 的 min/max
  for (const auto& e : wl.expected) {
    uint64_t pv = (e.seq << 8) | e.type;
    expect_min = std::min(expect_min, pv);
    expect_max = std::max(expect_max, pv);
  }
  if (pps[6] != expect_min || pps[7] != expect_max)
    errs.push_back("PPS min/max seq mismatch (" + std::to_string(pps[6]) + "/" +
                   std::to_string(pps[7]) + " vs " + std::to_string(expect_min) + "/" +
                   std::to_string(expect_max) + ")");
  // raw 尺寸统计（期望侧全部 32B 键）
  uint64_t rawk = 0, rawv = 0;
  for (const auto& e : wl.expected) { rawk += 32; rawv += e.value.size(); }
  if (pps[3] != rawk) errs.push_back("PPS.rawkeysize mismatch");
  if (pps[4] != rawv) errs.push_back("PPS.rawvaluesize mismatch");
  return errs;
}

}  // namespace zfdecode
#endif  // ZF_SST_DECODE_H
