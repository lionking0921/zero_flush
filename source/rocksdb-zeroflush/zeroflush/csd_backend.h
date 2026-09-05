//  Copyright (c) 2026, ZeroFlush-RocksDB.
//  ZeroFlush F-2 (阶段 F)：CSD-FPGA 物化卸载后端 —— 引擎侧协议/资格/会话抽象。
//
//  本模块是「真 U2 卡 A+B kernel 物化」在引擎树内的薄封装，职责：
//   1. 把封存代 A 数据打包成 kernel A staged 槽（[WAL 段字节][slim 条目区]），
//      B 数据打包成 kernel B raw-SST 链槽（[u64 K][K×{off,sz}][文件字节]）；
//   2. 提供 ZfCsdSession 抽象 + 进程级工厂注册（XRT 实现在单独 TU 编译进
//      带设备的测试目标；引擎 lib 恒不依赖 XRT —— 未注册 = 设备不可用，
//      调用方自动回落 host）；
//   3. 提供 PPS → ZfSealManifest 的字段装配（纯函数，engine 侧无加速器头）。
//  本模块只做「字节协议 + 会话接缝」，不接触 FileMetaData / VersionEdit /
//  TableBuilderOptions —— 那些由 materialize_job.cc 的卸载编排（TryCsdMaterialize）
//  在既有安装循环内完成。字节协议与 CPU-sim/真机 host（AcceleratorKernelSstV2/
//  host/main_zf.cpp + test/zf_cpu_sim_ab.cpp）逐字同源（见下方常量注释），
//  另以 engine 原生 D1 读缓冲 + 冻结 slim 索引为 A 侧事实源，杜绝自说自话。
//
//  编译：csd_backend.cc 进 librocksdb（SOURCES）。默认无 XRT 依赖。

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {
class Env;
struct ZfSealManifest;  // zeroflush/zf_seal.h（本头不引入，仅指针/引用）
}  // namespace ROCKSDB_NAMESPACE

namespace zeroflush {

class PartitionIndex;
struct ZeroFlushOptions;

// ---- kernel A/B 输入槽（host_data[24] 同语义；与 CPU-sim Slot 逐字一致）----
//   kind 0 = A staged（decoder）：bytes = [WAL 段字节][slim 条目区]；
//          kv = slim 条数；file_size = WAL 区字节。
//   kind 1 = B raw-SST 链（decoder_sst）：bytes = [u64 K][K×{u64 off,u64 sz}][文件…]；
//          kv = 0；file_size = 链字节总数。
struct ZfCsdSlot {
  uint32_t kind = 0;
  uint64_t kv = 0;
  uint64_t file_size = 0;
  std::vector<uint8_t> bytes;
};

// ---- 一次 A+B 的 kernel 输出（mode=1 全版本；单文件契约）----
struct ZfCsdOutput {
  uint64_t file_num = 0;   // 输出文件数（卸载要求 == 1）
  std::vector<uint8_t> data;   // data_size 字节（含各 5B trailer）
  std::vector<uint8_t> index;  // index_size 字节（含 index 块 5B trailer）
  uint64_t pps[512] = {0};     // PPS_KERNEL_SIZE words；首文件槽 [0..128)
};

// 会话：设备探测 + 一次 A+B run。实现方负责 cl::/xrt 路径（map 直读写，
// 已证真卡可用）。RunAb 出参 data/index 为 kernel 写回的前缀字节。
class ZfCsdSession {
 public:
  virtual ~ZfCsdSession() = default;
  // 设备可用性（Probe：能找到 xclbin/设备并建上下文/内核）。
  virtual bool Available() const = 0;
  // 跑一次 mode=1 A+B；slots 定长 4（空槽 bytes 为空）。sst/idx 为设备侧输出预算。
  // 成功时 out->file_num==1，data/index 已取回，pps[0..511] 填满。
  virtual ROCKSDB_NAMESPACE::Status RunAb(
      const ZfCsdSlot slots[4], uint64_t kv_sum, uint64_t sst_bytes,
      uint64_t idx_bytes, ZfCsdOutput* out) = 0;
};

// 进程级工厂（materialize_job 在卸载编排点调用 CreateZfCsdSession 取会话；
// 设备实现 csd_session_opencl.cc 仅在带 XRT 的测试/驱动目标中编译并注册）。
using ZfCsdSessionFactory =
    std::function<std::shared_ptr<ZfCsdSession>(const ZeroFlushOptions& zfo)>;
// 注册（可重复；后注册覆盖）。线程安全。
void RegisterZfCsdSessionFactory(ZfCsdSessionFactory f);
// 无注册 / 注册的工厂建不出可用会话 → 返回 nullptr（= 设备不可用，回落 host）。
std::shared_ptr<ZfCsdSession> CreateZfCsdSession(const ZeroFlushOptions& zfo);

// F-3：注册 XRT/OpenCL 设备会话工厂（csd_session_opencl.cc 的定义；该 TU 编译
// 时链 XRT，仅进 zf_csd_test 等带设备目标的链接图 —— 引擎 lib 不调用本声明，
// 无该 TU 链接时 CreateZfCsdSession 恒返回 nullptr = 设备不可用，自动回落 host）。
void RegisterZeroFlushCsdOpenclSessionFactory();

// F-3：显式释放进程级设备会话（csd_session_opencl.cc）。OpenCL/XRT 对象须在
// main 作用域内析构（main_zf 的 OcCtx 即如此）；若留在进程级 static 等到 atexit，
// clReleaseKernel 在 XRT context_mgr 拆除后执行 → SEGV（真卡实测）。带设备测试
// 在关闭全部 DB、确认无后台物化线程后调用一次。引擎 lib 不调用。
void ShutdownZeroFlushCsdSession();

// 卸载资格数值上限（保守；超出一律回落 host，正确性不依赖这些界）：
//  - kernel 单文件输出；≤4 输入口；每 A 口一个 (part,gen)。
constexpr int kCsdMaxPorts = 4;
constexpr int kCsdMaxChainFiles = 64;      // decoder_sst K ≤ 64
constexpr uint32_t kCsdMaxValueBytes = 1024;  // 值 ≤ 2×512B 片
constexpr uint32_t kCsdMaxUserKeyBytes = 24;  // 用户键 ≤ 24B（ik ≤ 32B）
// A staged 字节 / 条数经验上限（kernel 单口 staged 缓冲与 PPS 计数安全界；
// 超出回落 host——大分区不卸载）。
constexpr uint64_t kCsdMaxStagedBytes = 1u << 26;   // 64MB
constexpr uint64_t kCsdMaxRecords = 1u << 22;       // 4M 条

// 打包：读取 (part,gen) 封存 WAL 整段 + frozen slim 索引 → A 槽。
//   - ik.size()-8（用户键长）≤ kCsdMaxUserKeyBytes 且 >0；
//   - 每条 val_len ≤ kCsdMaxValueBytes（读 WAL 帧头 24B@+12）；
//   - locator 全部命中 WAL 段且 (part,gen) 匹配（完整性，同 aside）。
// 任一失败返回 false（调用方回落 host；不产生半成品槽）。
bool BuildCsdSlotA(ROCKSDB_NAMESPACE::Env* env, const std::string& wal_dir,
                   const std::shared_ptr<PartitionIndex>& idx, uint32_t part,
                   uint32_t gen, ZfCsdSlot* out);

// 打包（engine 物化接缝用）：给定**已按 internal comparator 有序**的 A 侧全版本
// internal keys + values（引擎物化 aside/WalScanner 产出的内存态），重建 kernel
// A staged 缓冲 —— 帧区 = 按序重建的 ZF01 帧（24B 头 + user + value + crc），
// slim 区 = 各条 (ik, locator)，locator.wal_offset = 帧在缓冲内的顺序偏移。
// 布局/编码与 AcceleratorKernelSstV2/host/zf_format.h 的 Port::Staged() 逐字一致
// （同一份已由 CPU-sim/引擎直读验证的字节协议）。
//   - 只接受 user key 恰 24B（ik 恰 32B）与 value ≤ 1024B 的输入（kernel 验证
//     语料均在此档；越界返回 false = 回落 host，不做未经验证的宽度外推）；
//   - *deletions 回填删除条数（ik 尾 type==kTypeDeletion，供 props 统计位，
//     kernel PPS 不报告删除数）。
bool BuildCsdSlotAFromSorted(uint32_t part, uint32_t gen,
                             const std::vector<std::string>& keys,
                             const std::vector<std::string>& values,
                             uint64_t* deletions, ZfCsdSlot* out);

// 整段读封存 WAL 文件字节（zf-wal-<part>-<gen>.log）。供 BuildCsdSlotA。
ROCKSDB_NAMESPACE::Status ReadSealedWal(ROCKSDB_NAMESPACE::Env* env,
                                        const std::string& wal_dir,
                                        uint32_t part, uint32_t gen,
                                        std::string* bytes);

// B 链槽：files = 有序文件字节（键范围互不相交且升序，须由调用方保证——
// MaterializeMergePartition 的 overlap_all 序即键序）。off 序列 = 前缀累加。
ZfCsdSlot BuildCsdSlotB(const std::vector<std::pair<uint64_t, std::string>>& files);

// PPS → ZfSealManifest 字段装配（首文件槽 pps[0..128)）。返回是否满足单文件
// 契约（num_entries 非 0、data/index 尺寸与 PPS 一致等；不符 = 不卸载）。
// num_deletions：kernel PPS 不报告删除数（引擎 props 的统计位），装配侧以
// 传入值填（0 时 = 引擎 reader 打开/直读不受影响，仅 TableProperties 统计）。
bool ZfCsdManifestFromPps(const uint64_t pps[512], uint64_t data_size,
                          uint64_t index_size, ROCKSDB_NAMESPACE::ZfSealManifest* m);

}  // namespace zeroflush
