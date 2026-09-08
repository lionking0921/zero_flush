//  Copyright (c) 2026, ZeroFlush-RocksDB.
//  第 0 步 micro-bench：真卡上紧循环 50× ~4MB A-only RunAb，量主机 seam 每段开销。
//
//  复用引擎同款字节生产者（BuildCsdSlotAFromSorted → 与 materialize_job.cc
//  TryCsdDirectMaterialize 完全同参数），loop 同一份真实语料档 A 槽，逐次
//  sess->RunAb(slots, n, sst_bytes, idx_bytes, &out)。配合会话侧 ZF_CSD_PROFILE=1
//  的分段计时（csd_session_opencl.cc RunAb），直接回答「0.41s 里 OpenCL 开销 vs
//  kernel 真算各占几成」，并给 seam 顶格吞吐与 kernel MB/s 上限。
//
//  用法：
//    zf_runab_microbench --xclbin=P [--device=0] [--keys=3700] [--value=1024]
//                         [--runs=50] [--warm=3]
//  （进程内只建会话不建 DB；结束时显式 Shutdown 免 atexit SEGV。）

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "rocksdb/status.h"
#include "zeroflush/csd_backend.h"
#include "zeroflush/zeroflush_db.h"

namespace {

uint64_t NowUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now()
                                       .time_since_epoch())
                                   .count());
}

// 构造 32B 内部键：user 恰 24B（数值定宽，升序唯一）+ 8B footer。
// footer = packed=(seq<<8)|type 的小端字节 —— BuildCsdSlotAFromSorted 内部用
// DecodeFixed64(ik+24) 解出 (seq,type) 重建 ZF01 帧头，故须按它期望的字节序喂，
// 与引擎真实 internal-key 尾 8B 同规（CPU-sim/真机已门证该函数消费的语料档）。
void MakeInternalKey(uint64_t i, uint64_t seq, uint8_t type, std::string* out) {
  std::string k(24, '0');
  char buf[25];
  std::snprintf(buf, sizeof(buf), "%024llu",
                static_cast<unsigned long long>(i));
  k.assign(buf, 24);
  const uint64_t packed = (seq << 8) | type;
  for (int j = 0; j < 8; ++j) {
    k.push_back(static_cast<char>((packed >> (8 * j)) & 0xff));
  }
  out->swap(k);
}

struct Args {
  std::string xclbin;
  uint32_t device = 0;
  uint64_t keys = 3700;   // ~4MB 输出档（真实 fillrandom 每次物化量级）
  size_t value = 1024;    // ≤ kCsdMaxValueBytes
  uint32_t runs = 50;
  uint32_t warm = 3;
};

bool ParseArgs(int argc, char** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto has = [&](const char* k) {
      return s.rfind(k, 0) == 0 && s.size() > std::strlen(k);
    };
    auto val = [&](const char* k) { return s.substr(std::strlen(k)); };
    if (has("--xclbin=")) {
      a->xclbin = val("--xclbin=");
    } else if (has("--device=")) {
      a->device = static_cast<uint32_t>(std::atoi(val("--device=").c_str()));
    } else if (has("--keys=")) {
      a->keys = static_cast<uint64_t>(std::strtoull(
          val("--keys=").c_str(), nullptr, 10));
    } else if (has("--value=")) {
      a->value = static_cast<size_t>(
          std::strtoull(val("--value=").c_str(), nullptr, 10));
    } else if (has("--runs=")) {
      a->runs = static_cast<uint32_t>(
          std::atoi(val("--runs=").c_str()));
    } else if (has("--warm=")) {
      a->warm = static_cast<uint32_t>(
          std::atoi(val("--warm=").c_str()));
    } else {
      fprintf(stderr, "unknown arg: %s\n", s.c_str());
      return false;
    }
  }
  return !a->xclbin.empty();
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    fprintf(stderr,
            "usage: %s --xclbin=P [--device=0] [--keys=3700] [--value=1024]"
            " [--runs=50] [--warm=3]\n",
            argv[0]);
    return 2;
  }
  if (args.value > 1024) {
    fprintf(stderr, "value %zu > 1024 (kCsdMaxValueBytes)\n", args.value);
    return 2;
  }
  // 语料档：keys 条 internal key（user 24B 唯一升序 + seq/type footer），
  // value 各 1024B —— 引擎同款 A 槽生产者（资格门内置校验 user==24/value≤1024）。
  std::vector<std::string> keys, values;
  keys.reserve(args.keys);
  values.reserve(args.keys);
  for (uint64_t i = 0; i < args.keys; ++i) {
    std::string ik;
    MakeInternalKey(i, /*seq=*/1000, /*type=*/1 /*kTypeValue*/, &ik);
    keys.push_back(std::move(ik));
    values.emplace_back(args.value, static_cast<char>('a' + (i & 15)));
  }
  zeroflush::ZfCsdSlot slot;
  uint64_t deletions = 0;
  if (!zeroflush::BuildCsdSlotAFromSorted(/*part=*/0, /*gen=*/0, keys, values,
                                          &deletions, &slot)) {
    fprintf(stderr, "BuildCsdSlotAFromSorted failed (keys=%llu value=%zu)\n",
            (unsigned long long)args.keys, args.value);
    return 2;
  }
  const uint64_t n = args.keys;
  const uint64_t staged_size = slot.file_size;
  const uint64_t sst_bytes = 8 * staged_size + 64 * 1024;  // 引擎同式（见 J 报告 §6.1）
  const uint64_t idx_bytes = sst_bytes / 4 + 1024 * 1024;
  fprintf(stderr,
          "payload: keys=%llu staged=%lluB (%llu B/key) sst_budget=%lluB"
          " idx_budget=%lluB deletions=%llu\n",
          (unsigned long long)n, (unsigned long long)staged_size,
          (unsigned long long)(staged_size / (n ? n : 1)),
          (unsigned long long)sst_bytes, (unsigned long long)idx_bytes,
          (unsigned long long)deletions);

  // 建会话（weak→strong 插件自动注册；需 csd_materialize + csd_xclbin 才过闸）。
  zeroflush::ZeroFlushOptions zfo;
  zfo.csd_materialize = true;
  zfo.csd_xclbin = args.xclbin;
  zfo.csd_device = args.device;
  std::shared_ptr<zeroflush::ZfCsdSession> sess =
      zeroflush::CreateZfCsdSession(zfo);
  if (!sess || !sess->Available()) {
    fprintf(stderr, "no available CSD device session (xclbin=%s dev=%u)\n",
            args.xclbin.c_str(), args.device);
    return 1;
  }
  fprintf(stderr, "session ready; runs=%u warm=%u\n", args.runs, args.warm);

  zeroflush::ZfCsdSlot slots[4];
  slots[0] = slot;  // slots[1..3] 空 = A-only，镜像 TryCsdDirectMaterialize
  auto run_once = [&](bool measure) -> bool {
    zeroflush::ZfCsdOutput out;
    uint64_t t0 = measure ? NowUs() : 0;
    ROCKSDB_NAMESPACE::Status s =
        sess->RunAb(slots, n, sst_bytes, idx_bytes, &out);
    uint64_t t1 = measure ? NowUs() : 0;
    if (!s.ok()) {
      fprintf(stderr, "RunAb error: %s\n", s.ToString().c_str());
      return false;
    }
    // 单文件契约复核（镜像引擎判拒门）：不满足说明语料档/预算有问题，须停。
    if (out.file_num != 1 || out.pps[1] != n || out.data.empty() ||
        out.index.empty()) {
      fprintf(stderr,
              "RunAb contract fail: files=%llu pps1=%llu expect=%llu"
              " data=%zu index=%zu\n",
              (unsigned long long)out.file_num,
              (unsigned long long)out.pps[1], (unsigned long long)n,
              out.data.size(), out.index.size());
      return false;
    }
    if (measure) {
      const uint64_t us = t1 - t0;
      const uint64_t obytes = out.data.size() + out.index.size();
      fprintf(stderr,
              "[bench] run tot=%lluus (%.3f ms) out_sst+idx=%lluB"
              " (%.1f MB/s seam)\n",
              (unsigned long long)us, us / 1000.0,
              (unsigned long long)obytes,
              obytes / (us / 1e6) / (1024.0 * 1024.0));
    }
    return true;
  };

  for (uint32_t i = 0; i < args.warm; ++i) {
    if (!run_once(false)) return 1;
  }
  uint64_t fail = 0;
  for (uint32_t i = 0; i < args.runs; ++i) {
    if (!run_once(true)) ++fail;
    if (fail > 0 && i > 0 && fail > i / 2) {
      fprintf(stderr, "too many failures (%llu) — abort\n",
              (unsigned long long)fail);
      return 1;
    }
  }
  fprintf(stderr, "[bench] done: %u runs, %llu failed\n", args.runs,
          (unsigned long long)fail);
  // 显式释放设备会话（main 作用域内析构 cl 对象，防 atexit SEGV —— 见 header 注）。
  zeroflush::ShutdownZeroFlushCsdSession();
  return fail ? 1 : 0;
}
