//  Copyright (c) 2026, ZeroFlush-RocksDB.
//  ZeroFlush F-3：CSD 设备会话（XRT/OpenCL）—— 只进 zf_csd_test 等带设备目标。
//
//  编译（照 AcceleratorKernelSstV2/Makefile 的 host 规则）：
//    g++ -std=c++17 -DCL_HPP_TARGET_OPENCL_VERSION=120 -DCL_HPP_MINIMUM_OPENCL_VERSION=120
//        -DCL_TARGET_OPENCL_VERSION=120 -I /opt/xilinx/xrt/include
//        -L /opt/xilinx/xrt/lib -lxilinxopencl ...
//  引擎 lib 恒不编译本 TU（XRT-free）；本文件只实现 csd_backend.h 的抽象，把
//  ZfCsdSlot[4] 逐字搬到真实内核（mode=1 A+B），回读 pps/data/index。
//
//  设备路径与 AcceleratorKernelSstV2/host/main_zf.cpp RunAbDevice 逐字同源：
//  cl::Buffer + enqueueMapBuffer 直读写 + enqueueTask（已在真 U2 卡验证；规避
//  xrt::bo 的 C2H 0x400 问题）。PPS/元区布局常量镜像 kernel/krnl_host.h：
//    MAX_OUTPUT_FILE_NUM=4, PPS_KERNEL_SINGEL_SIZE=128, PPS_KERNEL_SIZE=512；
//  pps_dev（2048×u64）中：out[512]=top_sst, out[516]=top_idx, out[520]=file_num，
//  首文件 PPS 槽在 out[0..128)。host_data[24] 布局见 RunAbDevice。
//
//  失败语义：任何 CL 错误 / 探不到设备 / xclbin 缺失 → 工厂返回 nullptr（引擎
//  回落 host），RunAb 失败返回 !ok Status。绝不在库侧 exit。

#include "zeroflush/csd_backend.h"

#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl2.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rocksdb/status.h"
#include "zeroflush/zeroflush_db.h"  // ZeroFlushOptions（csd_xclbin/csd_device）

namespace zeroflush {
namespace {

// ---- 与 kernel/krnl_host.h 镜像的几何常量（改动须两侧同步）----
constexpr uint32_t kMaxInFiles = 4;       // MAX_INPUT_FILE_NUM
constexpr uint32_t kPpsPerFile = 128;     // PPS_KERNEL_SINGEL_SIZE
constexpr uint32_t kPpsWords = kPpsPerFile * 4;   // PPS_KERNEL_SIZE = 512
constexpr uint32_t kMaxOutFiles = 4;      // MAX_OUTPUT_FILE_NUM
constexpr uint32_t kPpsBufWords = 2048;   // pps_dev 缓冲（main_zf 同量）
static_assert(kPpsWords == 512, "PPS region must match kernel PPS_KERNEL_SIZE");

#define CSD_ALIGN_TO_4K(x) (((x) + 4095) & ~4095ULL)

// ---- OpenCL 上下文：device/kernel 软失败版（main_zf OcCtx::Make 不 exit）----
struct OcCtx {
  cl::Device dev;
  cl::Context ctx;
  cl::CommandQueue q;
  cl::Kernel krn;

  static bool TryMake(const std::string& xclbin, int devidx, OcCtx* out) {
    cl_int err;
    std::vector<cl::Platform> platforms;
    if (cl::Platform::get(&platforms) != CL_SUCCESS || platforms.empty()) {
      return false;  // 无 OpenCL 平台
    }
    cl::Device device;
    bool found = false;
    for (auto& p : platforms) {
      if (p.getInfo<CL_PLATFORM_NAME>() == "Xilinx") {
        std::vector<cl::Device> devs;
        if (p.getDevices(CL_DEVICE_TYPE_ACCELERATOR, &devs) == CL_SUCCESS &&
            devs.size() > static_cast<size_t>(devidx)) {
          device = devs[devidx];
          found = true;
          break;
        }
      }
    }
    if (!found) {
      fprintf(stderr, "[ZF csd] no Xilinx accelerator device idx=%d\n", devidx);
      return false;
    }
    OcCtx c;
    c.dev = device;
    c.ctx = cl::Context(device, nullptr, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) return false;
    c.q = cl::CommandQueue(c.ctx, device, 0, &err);
    if (err != CL_SUCCESS) return false;
    std::ifstream bf(xclbin, std::ifstream::binary);
    if (!bf) {
      fprintf(stderr, "[ZF csd] cannot open xclbin %s\n", xclbin.c_str());
      return false;
    }
    bf.seekg(0, bf.end);
    size_t nb = static_cast<size_t>(bf.tellg());
    bf.seekg(0, bf.beg);
    std::vector<unsigned char> bin(nb);
    if (nb == 0 || !bf.read(reinterpret_cast<char*>(bin.data()), nb)) return false;
    bf.close();
    cl::Program::Binaries bins(1, bin);
    cl::Program prog(c.ctx, {device}, bins, nullptr, &err);
    if (err != CL_SUCCESS) return false;
    c.krn = cl::Kernel(prog, "krnl_vadd", &err);
    if (err != CL_SUCCESS) {
      fprintf(stderr, "[ZF csd] kernel err %d (expect krnl_vadd A+B)\n", err);
      return false;
    }
    *out = std::move(c);
    return true;
  }
};

cl::Buffer MakeBuf(OcCtx& c, size_t bytes, bool* ok) {
  cl_int err;
  cl::Buffer b(c.ctx, CL_MEM_READ_WRITE, bytes, nullptr, &err);
  if (err != CL_SUCCESS) {
    fprintf(stderr, "[ZF csd] buffer alloc %zu err %d\n", bytes, err);
    *ok = false;
  }
  return b;
}

bool MapBlob(OcCtx& c, cl::Buffer& b, size_t total, const uint8_t* src,
             size_t len) {
  cl_int err;
  void* m = c.q.enqueueMapBuffer(b, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE, 0,
                                 total, nullptr, nullptr, &err);
  if (err != CL_SUCCESS) return false;
  uint8_t* p = reinterpret_cast<uint8_t*>(m);
  if (src && len) std::memcpy(p, src, len);
  if (len < total) std::memset(p + len, 0, total - len);
  c.q.enqueueUnmapMemObject(b, m);
  return true;
}

bool MapRead(OcCtx& c, cl::Buffer& b, size_t total, void* dst) {
  cl_int err;
  void* m = c.q.enqueueMapBuffer(b, CL_TRUE, CL_MAP_READ, 0, total, nullptr,
                                 nullptr, &err);
  if (err != CL_SUCCESS) return false;
  std::memcpy(dst, m, total);
  c.q.enqueueUnmapMemObject(b, m);
  return true;
}

// ---- 会话实现（一次 run = 一次 enqueueTask；OcCtx 建好即设备可用）----
class ZfCsdSessionOpencl final : public ZfCsdSession {
 public:
  explicit ZfCsdSessionOpencl(OcCtx&& c) : c_(std::move(c)) {}

  bool Available() const override { return true; }  // 建出即探测成功

  ROCKSDB_NAMESPACE::Status RunAb(const ZfCsdSlot slots[4], uint64_t kv_sum,
                                  uint64_t sst_bytes, uint64_t idx_bytes,
                                  ZfCsdOutput* out) override {
    // 引擎物化以 materialize_parallelism 个 worker 并行调用本会话；内核为单实例
    // （nk=krnl_vadd:1），cl::CommandQueue / cl::Kernel 非线程安全 → 整段串行化。
    // 设备侧本就必须一次一 run（单内核），互斥不损吞吐。
    std::lock_guard<std::mutex> lock(run_mu_);
    if (out == nullptr) {
      return ROCKSDB_NAMESPACE::Status::InvalidArgument("null csd output");
    }
    const uint32_t nslots = CountSlots(slots);
    // ---- 输入口缓冲（4K 对齐；空口 = 全零 4KB，镜像 main_zf）----
    std::vector<cl::Buffer> inbuf;
    uint64_t wal_bytes[4] = {0, 0, 0, 0};
    uint64_t kv[4] = {0, 0, 0, 0};
    uint32_t kind[4] = {0, 0, 0, 0};
    for (uint32_t p = 0; p < 4; ++p) {
      if (p < nslots) {
        wal_bytes[p] = slots[p].file_size;
        kv[p] = slots[p].kv;
        kind[p] = slots[p].kind;
      }
      const std::vector<uint8_t>& src =
          (p < nslots) ? slots[p].bytes : kEmptyBytes();
      const size_t cap = std::max<size_t>(CSD_ALIGN_TO_4K(src.size()), 4096);
      bool ok = true;
      cl::Buffer b = MakeBuf(c_, cap, &ok);
      if (!ok) {
        return ROCKSDB_NAMESPACE::Status::IOError("csd in-buffer alloc");
      }
      if (!MapBlob(c_, b, cap, src.data(), src.size())) {
        return ROCKSDB_NAMESPACE::Status::IOError("csd in-buffer map");
      }
      inbuf.push_back(b);
    }
    // ---- 输出预算缓冲：容量 ≥ 预算（写满预算前 kernel 必须在界内）----
    const uint64_t sst_cap = CSD_ALIGN_TO_4K(sst_bytes);
    const uint64_t idx_cap = CSD_ALIGN_TO_4K(idx_bytes);
    bool ok = true;
    cl::Buffer sst_dev = MakeBuf(c_, std::max<uint64_t>(sst_cap, 4096), &ok);
    if (!ok) return ROCKSDB_NAMESPACE::Status::IOError("csd sst-buffer alloc");
    cl::Buffer idx_dev = MakeBuf(c_, std::max<uint64_t>(idx_cap, 4096), &ok);
    if (!ok) return ROCKSDB_NAMESPACE::Status::IOError("csd idx-buffer alloc");
    cl::Buffer pps_dev =
        MakeBuf(c_, sizeof(uint64_t) * kPpsBufWords, &ok);
    if (!ok) return ROCKSDB_NAMESPACE::Status::IOError("csd pps-buffer alloc");

    uint64_t host_data[24] = {0};
    host_data[4] = sst_bytes;  // sst 总预算（kernel 内部 4 等分，单文件用片 1）
    for (uint32_t p = 0; p < 4; ++p) {
      host_data[5 + p] = wal_bytes[p];
      host_data[10 + p] = kv[p];
    }
    host_data[9] = kv_sum;   // 输出记录总数（encoder 精确计数，不可下溢）
    host_data[15] = 1;       // mode=1 A+B 全版本
    for (uint32_t p = 0; p < 4; ++p) host_data[16 + p] = kind[p];
    cl::Buffer host_dev =
        MakeBuf(c_, sizeof(host_data), &ok);
    if (!ok) return ROCKSDB_NAMESPACE::Status::IOError("csd host-buffer alloc");
    if (!MapBlob(c_, host_dev, sizeof(host_data),
                 reinterpret_cast<const uint8_t*>(host_data),
                 sizeof(host_data))) {
      return ROCKSDB_NAMESPACE::Status::IOError("csd host-buffer map");
    }

    for (uint32_t p = 0; p < 4; ++p) c_.krn.setArg(int(p), inbuf[p]);
    c_.krn.setArg(4, host_dev);
    c_.krn.setArg(5, sst_dev);
    c_.krn.setArg(6, idx_dev);
    c_.krn.setArg(7, pps_dev);
    cl_int err = c_.q.enqueueTask(c_.krn, nullptr, nullptr);
    if (err == CL_SUCCESS) err = c_.q.finish();
    if (err != CL_SUCCESS) {
      fprintf(stderr, "[ZF csd] enqueueTask err %d\n", err);
      return ROCKSDB_NAMESPACE::Status::IOError("csd kernel task failed");
    }

    std::vector<uint64_t> meta(kPpsBufWords);
    if (!MapRead(c_, pps_dev, sizeof(uint64_t) * kPpsBufWords, meta.data())) {
      return ROCKSDB_NAMESPACE::Status::IOError("csd pps readback");
    }
    const uint64_t top_sst = meta[kPpsWords];           // out[512]
    const uint64_t top_idx = meta[kPpsWords + kMaxOutFiles];     // out[516]
    const uint64_t file_num =
        meta[kPpsWords + 2 * kMaxOutFiles];                       // out[520]
    std::vector<uint8_t> data_b(static_cast<size_t>(top_sst));
    std::vector<uint8_t> idx_b(static_cast<size_t>(top_idx));
    if (top_sst && !MapRead(c_, sst_dev, static_cast<size_t>(top_sst),
                            data_b.data())) {
      return ROCKSDB_NAMESPACE::Status::IOError("csd data readback");
    }
    if (top_idx && !MapRead(c_, idx_dev, static_cast<size_t>(top_idx),
                            idx_b.data())) {
      return ROCKSDB_NAMESPACE::Status::IOError("csd index readback");
    }
    std::memcpy(out->pps, meta.data(), sizeof(uint64_t) * kPpsWords);
    out->file_num = file_num;
    out->data = std::move(data_b);
    out->index = std::move(idx_b);
    return ROCKSDB_NAMESPACE::Status::OK();
  }

 private:
  static uint32_t CountSlots(const ZfCsdSlot slots[4]) {
    for (uint32_t p = 0; p < 4; ++p) {
      if (slots[p].bytes.empty()) return p;  // 首个空口截断（编排点保证紧凑）
    }
    return 4;
  }
  static const std::vector<uint8_t>& kEmptyBytes() {
    static const std::vector<uint8_t> e;
    return e;
  }
  OcCtx c_;
  std::mutex run_mu_;  // 单内核实例：串行化 device run
};

}  // namespace

// ---- 进程级惰性单会话（一次构建；探测失败 → 恒返回 nullptr）----
namespace {
std::mutex g_session_mu;
std::shared_ptr<ZfCsdSession> g_session;
bool g_session_tried = false;
}  // namespace

void RegisterZeroFlushCsdOpenclSessionFactory() {
  RegisterZfCsdSessionFactory([](const ZeroFlushOptions& zfo) {
    std::lock_guard<std::mutex> lock(g_session_mu);
    if (!g_session_tried) {
      g_session_tried = true;
      if (!zfo.csd_xclbin.empty()) {
        OcCtx ctx;
        if (OcCtx::TryMake(zfo.csd_xclbin, static_cast<int>(zfo.csd_device),
                           &ctx)) {
          g_session = std::make_shared<ZfCsdSessionOpencl>(std::move(ctx));
          fprintf(stderr, "[ZF csd] device session ready (xclbin=%s dev=%u)\n",
                  zfo.csd_xclbin.c_str(), zfo.csd_device);
        } else {
          fprintf(stderr,
                  "[ZF csd] device session unavailable → host fallback\n");
        }
      } else {
        fprintf(stderr,
                "[ZF csd] csd_xclbin empty → device unavailable (host)\n");
      }
    }
    return g_session;
  });
}

// 显式释放设备会话：必须在 main 作用域内析构 OpenCL/XRT 对象（若留到进程
// atexit，clReleaseKernel 会在 XRT context_mgr 拆除后触发 → SEGV，真卡实测）。
// 调用方须保证无并发 RunAb 在途（先关闭全部 DB / 无后台物化线程）。
void ShutdownZeroFlushCsdSession() {
  std::lock_guard<std::mutex> lock(g_session_mu);
  g_session.reset();  // 析构 ZfCsdSessionOpencl → OcCtx 的 cl 对象，仍在作用域内
}

}  // namespace zeroflush
