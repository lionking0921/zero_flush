// main_zf.cpp —— ZeroFlush aux-sort 内核的 OpenCL(cl2.hpp) host 测试（sw_emu / hw 通用）
//
// device 层用 U2 真机验证过的 OpenCL 路径（参考 CoKV vadd.cpp：cl::Buffer +
// enqueueMapBuffer 直读写 + enqueueTask）。早期 XRT native(xrt::bo) 版本在真卡上
// 内核无法正确访问/回读输出（C2H DMA 0x400、map 不可见），OpenCL 路径规避该问题。
//
// 每个输入口设备缓冲 = [WAL 段字节区 (wal_bytes)] [slim 条目区 (kv_i)]，即 Port::Staged() 原样。
// host_data[24]: [0..3]=buf_offset(0)  [4]=sst 总预算 [5..8]=wal_bytes[i]
//               [9]=Σkv  [10..13]=kv_i  [14]=init(0)
//               [15]=mode(0=M3 去重 / 1=A+B 全版本)  [16..19]=port_kind[i]
//                    (0=A staged(decoder), 1=B raw-SST 链(decoder_sst))  [20..23]=保留
// 本文件是 M3 风格 host：全部 4 口 = A staged、mode=0（默认全 0），行为与 M3 一致。
// A+B（B 口 + mode=1）的真 host 场景属下个里程碑，AB 软件验证走 test/zf_cpu_sim_ab.cpp。
// 校验：解码内核产物，与去重排序后的 KV 预言机逐条比对（== zf_cpu_sim 的 CPU 语义，经真实内核）。
//
// 用法：
//   main_zf -x build/hw/krnl_vadd.xclbin [-d <dev>] [seed ports base dup]...   （无参数 = 默认种子集）
#include <CL/cl2.hpp>
#include <CL/cl_ext_xilinx.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "zf_sst_decode.h"
#include "krnl_host.h"

#define ALIGN_TO_4K(x) (((x) + 4095) & ~4095ULL)

// ---- OpenCL 上下文：device/kernel 用栈局部，避免 exit-handler 期析构 ----
struct OcCtx {
  cl::Device dev;
  cl::Context ctx;
  cl::CommandQueue q;
  cl::Kernel krn;

  static OcCtx Make(const std::string& xclbin, int devidx) {
    cl_int err;
    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);
    cl::Device device;
    bool found = false;
    for (auto& p : platforms) {
      if (p.getInfo<CL_PLATFORM_NAME>() == "Xilinx") {
        std::vector<cl::Device> devs;
        p.getDevices(CL_DEVICE_TYPE_ACCELERATOR, &devs);
        if (devs.size() > (size_t)devidx) { device = devs[devidx]; found = true; break; }
      }
    }
    if (!found) { fprintf(stderr, "no Xilinx accel device idx %d\n", devidx); exit(2); }
    OcCtx c;
    c.dev = device;
    c.ctx = cl::Context(device, nullptr, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) { fprintf(stderr, "context err %d\n", err); exit(2); }
    c.q = cl::CommandQueue(c.ctx, device, 0, &err);
    if (err != CL_SUCCESS) { fprintf(stderr, "queue err %d\n", err); exit(2); }
    std::ifstream bf(xclbin, std::ifstream::binary);
    bf.seekg(0, bf.end); size_t nb = bf.tellg(); bf.seekg(0, bf.beg);
    std::vector<unsigned char> bin(nb); bf.read((char*)bin.data(), nb); bf.close();
    cl::Program::Binaries bins(1, bin);
    cl::Program prog(c.ctx, {device}, bins, nullptr, &err);
    if (err != CL_SUCCESS) { fprintf(stderr, "program err %d\n", err); exit(2); }
    c.krn = cl::Kernel(prog, "krnl_vadd", &err);
    if (err != CL_SUCCESS) { fprintf(stderr, "kernel err %d\n", err); exit(2); }
    return c;
  }
};

static cl::Buffer MakeBuf(OcCtx& c, size_t bytes) {
  cl_int err;
  cl::Buffer b(c.ctx, CL_MEM_READ_WRITE, bytes, nullptr, &err);
  if (err != CL_SUCCESS) { fprintf(stderr, "buffer alloc %zu err %d\n", bytes, err); exit(2); }
  return b;
}

// 整块 map：前 len 字节来自 src（src 为空则清零），剩余 0 填充。规避多次 map 错位。
static void MapBlob(OcCtx& c, cl::Buffer& b, size_t total, const uint8_t* src, size_t len) {
  cl_int err;
  void* m = c.q.enqueueMapBuffer(b, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE, 0, total,
                                 nullptr, nullptr, &err);
  if (err != CL_SUCCESS) { fprintf(stderr, "map %zu err %d\n", total, err); exit(2); }
  uint8_t* p = reinterpret_cast<uint8_t*>(m);
  if (src && len) memcpy(p, src, len);
  if (len < total) memset(p + len, 0, total - len);
  c.q.enqueueUnmapMemObject(b, m);
}

// 读回：阻塞 map + memcpy
static void MapRead(OcCtx& c, cl::Buffer& b, size_t total, void* dst) {
  cl_int err;
  void* m = c.q.enqueueMapBuffer(b, CL_TRUE, CL_MAP_READ, 0, total, nullptr, nullptr, &err);
  if (err != CL_SUCCESS) { fprintf(stderr, "map-read %zu err %d\n", total, err); exit(2); }
  memcpy(dst, m, total);
  c.q.enqueueUnmapMemObject(b, m);
}

// (里程碑 3) 设备产物落盘：与 test/zf_cpu_sim.cpp 的 dump 同布局，供引擎侧
// tools/zf_seal_check 封口/直读对拍消费：
//   prefix: 8B magic "ZFPRFX1\n" + u64 data_size + u64 index_size
//           + u64 num_deletions + PPS_KERNEL_SIZE×u64 + data 字节 + index 字节
//   expect: 逐行 `hex(32B 内部键)\t hex(值)`（文件内部键序）
static const char* g_dump_dir = nullptr;

static void DumpCase(const zf::Workload& wl, const std::vector<uint8_t>& data_b,
                     const std::vector<uint8_t>& idx_b, const uint64_t* out,
                     uint64_t top_sst, uint64_t top_idx, uint64_t seed,
                     uint32_t ports, size_t base, size_t dup) {
  if (!g_dump_dir) return;
  auto hexes = [](const std::string& s) {
    std::string h; char buf[4];
    for (size_t i = 0; i < s.size(); ++i) { snprintf(buf, 4, "%02x", (unsigned char)s[i]); h += buf; }
    return h;
  };
  char name[256];
  snprintf(name, sizeof(name), "%s/case_%llu_%u_%zu_%zu.prefix", g_dump_dir,
           (unsigned long long)seed, ports, base, dup);
  FILE* f = fopen(name, "wb");
  if (!f) { printf("  dump open fail %s\n", name); return; }
  fwrite("ZFPRFX1\n", 1, 8, f);
  uint64_t ndel = 0;
  for (const auto& r : wl.expected) ndel += r.IsDeletion() ? 1 : 0;
  uint64_t hdr[3] = {top_sst, top_idx, ndel};
  fwrite(hdr, 8, 3, f);
  fwrite(out, sizeof(uint64_t), PPS_KERNEL_SIZE, f);
  fwrite(data_b.data(), 1, data_b.size(), f);
  fwrite(idx_b.data(), 1, idx_b.size(), f);
  fclose(f);
  std::string efile = std::string(name, strlen(name) - strlen(".prefix")) + ".expect";
  FILE* e = fopen(efile.c_str(), "wb");
  if (!e) { printf("  dump open fail %s\n", efile.c_str()); return; }
  for (const auto& r : wl.expected) {
    std::string ik = zf::InternalKey(r.user, r.seq, r.type);
    std::string line = hexes(ik) + "\t" + hexes(r.value) + "\n";
    fwrite(line.data(), 1, line.size(), e);
  }
  fclose(e);
  printf("  dumped %s\n", name);
}

// ---- 跑一个 workload；返回是否全绿 ----
static bool RunCase(OcCtx& c, uint64_t seed, uint32_t port_count,
                    size_t base_keys, size_t dup_keys) {
  zf::Workload wl = zf::GenWorkload(seed, port_count, base_keys, dup_keys);
  uint32_t kv_sum = wl.total_kv();

  std::vector<cl::Buffer> inbuf;
  uint64_t wal_bytes[4] = {0, 0, 0, 0};
  uint64_t kv[4] = {0, 0, 0, 0};
  for (uint32_t p = 0; p < 4; ++p) {
    std::vector<uint8_t> staged;   // 空口 = 空向量
    if (p < wl.port.size()) {
      staged = wl.port[p].Staged();
      wal_bytes[p] = wl.port[p].wal_bytes();
      kv[p] = wl.port[p].kv();
    }
    size_t cap = std::max<size_t>(ALIGN_TO_4K(staged.size()), 4096);
    cl::Buffer b = MakeBuf(c, cap);
    MapBlob(c, b, cap, staged.data(), staged.size());   // 整块清零填充，未使用区不残留
    inbuf.push_back(b);
  }

  const uint64_t kSstBytes = 16ull * 1024 * 1024;   // sst 输出设备缓冲预算（encoder max_file_size 关联）
  const uint64_t kIdxBytes = 2ull * 1024 * 1024;     // index 输出设备缓冲
  cl::Buffer sst_dev = MakeBuf(c, kSstBytes);
  cl::Buffer idx_dev = MakeBuf(c, kIdxBytes);
  cl::Buffer pps_dev = MakeBuf(c, sizeof(uint64_t) * 2048);

  uint64_t host_data[24] = {0};
  host_data[4] = kSstBytes;
  for (uint32_t p = 0; p < 4; ++p) {
    host_data[5 + p] = wal_bytes[p];
    host_data[10 + p] = kv[p];
  }
  host_data[9] = kv_sum;
  // [15]=0 mode(M3 去重)、[16..19]=0 port_kind(全 A staged)、[20..23]=0 保留 —— 默认即 M3 行为。
  cl::Buffer host_dev = MakeBuf(c, sizeof(host_data));
  MapBlob(c, host_dev, sizeof(host_data), (const uint8_t*)host_data, sizeof(host_data));

  for (uint32_t p = 0; p < 4; ++p) c.krn.setArg(int(p), inbuf[p]);
  c.krn.setArg(4, host_dev);
  c.krn.setArg(5, sst_dev);
  c.krn.setArg(6, idx_dev);
  c.krn.setArg(7, pps_dev);
  c.q.enqueueTask(c.krn, nullptr, nullptr);
  c.q.finish();

  // 读回 output_data（PPS + 各文件长度）
  std::vector<uint64_t> out(2048);
  MapRead(c, pps_dev, sizeof(uint64_t) * out.size(), out.data());
  uint64_t top_sst = out[PPS_KERNEL_SIZE];
  uint64_t top_idx = out[PPS_KERNEL_SIZE + MAX_OUTPUT_FILE_NUM];
  uint64_t file_num = out[PPS_KERNEL_SIZE + 2 * MAX_OUTPUT_FILE_NUM];

  printf("case seed=%llu ports=%u base=%zu dup=%zu kv_sum=%u expect=%zu file_num=%llu "
         "data=%llu idx=%llu\n",
         (unsigned long long)seed, port_count, base_keys, dup_keys, kv_sum, wl.expected.size(),
         (unsigned long long)file_num, (unsigned long long)top_sst, (unsigned long long)top_idx);

  if (file_num != 1) { printf("  FAIL file_num=%llu (expected 1)\n", (unsigned long long)file_num); return false; }

  std::vector<uint8_t> data_b(top_sst);
  std::vector<uint8_t> idx_b(top_idx);
  if (top_sst) MapRead(c, sst_dev, size_t(top_sst), data_b.data());
  if (top_idx) MapRead(c, idx_dev, size_t(top_idx), idx_b.data());

  auto errs = zfdecode::CompareWorkload(wl, data_b, idx_b, out.data(), size_t(top_sst));
  if (!errs.empty()) {
    for (const auto& e : errs) printf("  FAIL %s\n", e.c_str());
    return false;
  }
  DumpCase(wl, data_b, idx_b, out.data(), top_sst, top_idx, seed, port_count,
           base_keys, dup_keys);
  printf("  OK  %zu records match oracle\n", wl.expected.size());
  return true;
}

int main(int argc, char** argv) {
  std::string xclbin;
  int dev = 0;
  std::vector<std::string> rest;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-x") && i + 1 < argc) xclbin = argv[++i];
    else if (!std::strcmp(argv[i], "-d") && i + 1 < argc) dev = atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "-o") && i + 1 < argc) g_dump_dir = argv[++i];
    else rest.push_back(argv[i]);
  }
  if (xclbin.empty()) { std::cerr << "usage: main_zf -x <xclbin> [-d <dev>] [seed ports base dup]...\n"; return 2; }
  OcCtx c = OcCtx::Make(xclbin, dev);

  struct C { uint64_t seed; uint32_t ports; size_t base, dup; };
  std::vector<C> cases;
  for (size_t i = 0; i + 3 < rest.size(); i += 4) {
    cases.push_back({strtoull(rest[i].c_str(), nullptr, 10), uint32_t(atoi(rest[i + 1].c_str())),
                     size_t(atoi(rest[i + 2].c_str())), size_t(atoi(rest[i + 3].c_str()))});
  }
  if (cases.empty()) {
    cases = {{1, 4, 60, 10}, {2, 4, 120, 15}, {3, 3, 80, 8}, {4, 2, 150, 25},
             {5, 1, 100, 0},  {6, 4, 30, 5},   {7, 4, 70, 20}, {8, 3, 45, 6}};
  }
  int npass = 0;
  for (const auto& cse : cases)
    if (RunCase(c, cse.seed, cse.ports, cse.base, cse.dup)) ++npass;
  printf("==== %d/%zu cases PASS ====\n", npass, cases.size());
  fflush(stdout);
  return npass == (int)cases.size() ? 0 : 1;
}
