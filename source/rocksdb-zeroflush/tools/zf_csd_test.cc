//  Copyright (c) 2026, ZeroFlush-RocksDB.
//  ZeroFlush F-3：CSD 双后端等价 + 真卡物化测试（tools 目标，链 XRT/OpenCL）。
//
//  同一份确定性可写集（user key 恰 24B、value ≤ 1024B —— A-only 卸载语料档）分别
//  开两个 ZeroFlush DB：
//    - csd=off（host 参照：引擎 BuildTable 产分区 SST）；
//    - csd=on （csd_materialize=true；若 --xclbin 可用则真实卸载到 U2 卡 A+B kernel，
//               否则经会话工厂回落 host）。
//  两库写入同一序列 → 触发物化（小分区阈值，直装路径）→ 全键读回：
//    1) 单迭代器全扫描（user key 序 + value）逐条一致 == host 参照；
//    2) 全键 Get 一致；
//    3) csd 库 Close+Reopen 后重扫仍 == host 参照（CSD 产物 SST 经引擎 reader
//       逐块 CRC 打开/直读，字节错即 Corruption → 集合差异被捕获）；
//    4) 计数断言：无设备 → csd_files==0 且 csd_fallbacks>0（卸载请求已回落 host，
//       零静默错排）；有设备且语料合格 → csd_files>0（真实 offload 闭环）。
//
//  用法：
//    zf_csd_test                      # host 回落等价（无需设备/xclbin）
//    zf_csd_test --xclbin P [--device N]   # 真实 U2 卸载等价（E-1 xclbin 产物）
//  退出码 = 失败断言数（0 = 全绿）。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "zeroflush/csd_backend.h"
#include "zeroflush/zeroflush_db.h"

namespace {

int g_failures = 0;

void Report(const std::string& name, bool ok, const std::string& detail = "") {
  fprintf(stderr, "[%s] %s%s\n", ok ? "PASS" : "FAIL", name.c_str(),
          detail.empty() ? "" : (" — " + detail).c_str());
  if (!ok) ++g_failures;
}

void CleanDB(const std::string& dbname) {
  std::string cmd = "rm -rf '" + dbname + "'";
  if (std::system(cmd.c_str()) != 0) {
    fprintf(stderr, "[WARN] failed to %s\n", cmd.c_str());
  }
}

rocksdb::Options MakeOptions() {
  rocksdb::Options opt;
  opt.create_if_missing = true;
  opt.compression = rocksdb::kNoCompression;
  return opt;
}

// 读取 ZeroFlush 指标（rocksdb.zeroflush.<name>）。
uint64_t ZfMetric(rocksdb::DB* db, const std::string& name) {
  std::string val;
  if (!db->GetProperty("rocksdb.zeroflush." + name, &val)) return 0;
  return ::strtoull(val.c_str(), nullptr, 10);
}

// 等待全部已封存 epoch 物化完成（sealed == materialized 且多拍稳定）。
bool WaitAllMaterialized(rocksdb::DB* db, uint64_t timeout_ms = 60000) {
  uint64_t last_m = 0, last_s = 0;
  int stable = 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const uint64_t m = ZfMetric(db, "epochs_materialized");
    const uint64_t s = ZfMetric(db, "epochs_sealed");
    if (s == m && m == last_m && s == last_s) {
      if (++stable >= 5) return true;
    } else {
      stable = 0;
    }
    last_m = m;
    last_s = s;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

// 单迭代器全量扫描 → 逐条 "key\tvalue"（user key 字节序，两库同序可比）。
bool DumpAll(rocksdb::DB* db, std::vector<std::string>* out) {
  out->clear();
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions()));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out->emplace_back(it->key().ToString());
    out->back().push_back('\t');
    out->back().append(it->value().ToString());
  }
  return it->status().ok();
}

void DumpLogTail(const std::string& dbname, size_t nbytes = 4096) {
  const std::string path = dbname + "/LOG";
  FILE* f = ::fopen(path.c_str(), "rb");
  if (f == nullptr) return;
  ::fseek(f, 0, SEEK_END);
  const long sz = ::ftell(f);
  const long off = sz > static_cast<long>(nbytes)
                       ? sz - static_cast<long>(nbytes)
                       : 0;
  ::fseek(f, off, SEEK_SET);
  std::string buf(static_cast<size_t>(sz - off), '\0');
  const size_t got = ::fread(&buf[0], 1, buf.size(), f);
  buf.resize(got);
  ::fclose(f);
  fprintf(stderr, "----- LOG tail -----\n%s\n--------------------\n",
          buf.c_str());
}

// 确定性可写集：key = 分区前缀(ka/kc/ke/kg) + 22 位零填充序号（user key 恰 24B，
// 跨 epoch 键区间不重叠、分区内严格递增 → 全量直装前提，与 BulkLoadZeroL0 同构）。
// value = 确定性填充，长度 16..920（≤1024B，A-only 卸载语料档全合格）。
const char* kPrefix[4] = {"ka", "kc", "ke", "kg"};
const int kEpochs = 10, kPerPart = 48;

void MakeKey(int e, int i, int p, std::string* k) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%s%022lld", kPrefix[p],
           (long long)(e * 100 + i));
  k->assign(buf, 24);  // user key 恰 24 字节
}

void MakeValue(int e, int i, int p, std::string* v) {
  const int len = 16 + ((e * 7 + p * 13 + i * 5) % 900);  // 16..915
  v->resize(static_cast<size_t>(len));
  for (int j = 0; j < len; ++j) {
    (*v)[j] = static_cast<char>(0x21 + ((e * 31 + i * 17 + p * 3 + j) % 90));
  }
}

// 一个 ZeroFlush DB 的选项（csd off/on 唯一区别在 csd_materialize）。
zeroflush::ZeroFlushOptions MakeZfo(bool csd_on, const std::string& xclbin,
                                    uint32_t dev) {
  zeroflush::ZeroFlushOptions zfo;
  zfo.partitions = 4;
  zfo.routing_mode = zeroflush::ZeroFlushOptions::RoutingMode::kStatic;
  zfo.static_boundaries = {"kb", "kd", "kf"};
  zfo.partition_target_bytes = 8 << 10;  // 8KB：单分区超限即封存
  zfo.epoch_target_bytes = 8 << 10;      // 副触发
  if (csd_on) {
    zfo.csd_materialize = true;
    zfo.csd_xclbin = xclbin;
    zfo.csd_device = dev;
  }
  return zfo;
}

// 写入完整工作负载（确定性、两库一致）。
bool WriteLoad(rocksdb::DB* db, const char* tag) {
  std::string k, v;
  for (int e = 0; e < kEpochs; ++e) {
    for (int i = 0; i < kPerPart; ++i) {
      for (int p = 0; p < 4; ++p) {
        MakeKey(e, i, p, &k);
        MakeValue(e, i, p, &v);
        rocksdb::Status s =
            db->Put(rocksdb::WriteOptions(), rocksdb::Slice(k), rocksdb::Slice(v));
        if (!s.ok()) {
          Report(tag, false, "put e" + std::to_string(e) +
                                 " i" + std::to_string(i) +
                                 " p" + std::to_string(p) + ": " +
                                 s.ToString());
          return false;
        }
      }
    }
  }
  return true;
}

struct CsdCounters {
  uint64_t files = 0, attempts = 0, fallbacks = 0;
};

CsdCounters ReadCsdCounters(rocksdb::DB* db) {
  CsdCounters c;
  c.files = ZfMetric(db, "csd_files");
  c.attempts = ZfMetric(db, "csd_attempts");
  c.fallbacks = ZfMetric(db, "csd_fallbacks");
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  fprintf(stderr, "============================================================\n");
  fprintf(stderr, " ZeroFlush F-3 CSD dual-backend equivalence test\n");
  fprintf(stderr, "============================================================\n");

  std::string xclbin;
  uint32_t dev = 0;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--xclbin") && i + 1 < argc) {
      xclbin = argv[++i];
    } else if (!std::strcmp(argv[i], "--device") && i + 1 < argc) {
      dev = static_cast<uint32_t>(::strtoul(argv[++i], nullptr, 10));
    } else {
      fprintf(stderr, "unknown arg %s\n", argv[i]);
      return 2;
    }
  }

  const std::string kHostDb = "/tmp/zf_csd_host";
  const std::string kCsdDb = "/tmp/zf_csd_dev";
  CleanDB(kHostDb);
  CleanDB(kCsdDb);

  // ---- 注册设备会话工厂（xclbin 缺失 → 工厂返回 null → 自动回落 host）----
  zeroflush::RegisterZeroFlushCsdOpenclSessionFactory();

  zeroflush::ZeroFlushOptions zfo_host = MakeZfo(false, "", 0);
  zeroflush::ZeroFlushOptions zfo_csd = MakeZfo(true, xclbin, dev);
  std::unique_ptr<rocksdb::DB> host, csd;
  rocksdb::Status s;
  s = zeroflush::Open(MakeOptions(), zfo_host, kHostDb, &host);
  if (!s.ok()) {
    Report("open host", false, s.ToString());
    return g_failures ? g_failures : 1;
  }
  s = zeroflush::Open(MakeOptions(), zfo_csd, kCsdDb, &csd);
  if (!s.ok()) {
    Report("open csd", false, s.ToString());
    return g_failures ? g_failures : 1;
  }
  Report("open host+csd", true);

  if (!WriteLoad(host.get(), "host writes") ||
      !WriteLoad(csd.get(), "csd writes")) {
    return g_failures ? g_failures : 1;
  }
  Report("dual load written", true);

  // ---- 触发并等待物化完成（写结束 sealed 不再增长）----
  if (!WaitAllMaterialized(host.get())) {
    DumpLogTail(kHostDb);
    Report("host materialize", false,
           "sealed=" + std::to_string(ZfMetric(host.get(), "epochs_sealed")) +
               " materialized=" +
               std::to_string(ZfMetric(host.get(), "epochs_materialized")));
  } else {
    Report("host materialize", true);
  }
  if (!WaitAllMaterialized(csd.get())) {
    DumpLogTail(kCsdDb);
    Report("csd materialize", false,
           "sealed=" + std::to_string(ZfMetric(csd.get(), "epochs_sealed")) +
               " materialized=" +
               std::to_string(ZfMetric(csd.get(), "epochs_materialized")));
  } else {
    Report("csd materialize", true);
  }

  // ---- 全键 Get 逐条一致 ----
  const int total = kEpochs * kPerPart * 4;
  std::string k, v, hv, cv;
  size_t get_mismatch = 0;
  for (int e = 0; e < kEpochs && get_mismatch == 0; ++e) {
    for (int i = 0; i < kPerPart && get_mismatch == 0; ++i) {
      for (int p = 0; p < 4; ++p) {
        MakeKey(e, i, p, &k);
        MakeValue(e, i, p, &v);
        rocksdb::Status h = host->Get(rocksdb::ReadOptions(), k, &hv);
        rocksdb::Status c = csd->Get(rocksdb::ReadOptions(), k, &cv);
        if (!h.ok() || !c.ok() || hv != v || cv != v || hv != cv) {
          ++get_mismatch;
          if (get_mismatch <= 3) {
            fprintf(stderr, "  get mismatch key=%s host=%s csd=%s\n", k.c_str(),
                    h.ToString().c_str(), c.ToString().c_str());
          }
        }
      }
    }
  }
  Report("per-key Get == oracle & host==csd (" + std::to_string(total) + " keys)",
         get_mismatch == 0,
         get_mismatch ? std::to_string(get_mismatch) + " mismatches" : "");

  // ---- 单迭代器全扫描一致 ----
  std::vector<std::string> host_scan, csd_scan;
  bool hs = DumpAll(host.get(), &host_scan);
  bool cs = DumpAll(csd.get(), &csd_scan);
  const bool scan_ok = hs && cs && host_scan.size() == csd_scan.size() &&
                       host_scan == csd_scan;
  Report("full-scan equivalence (" + std::to_string(host_scan.size()) +
             " entries host==csd)",
         scan_ok,
         !scan_ok
             ? "host=" + std::to_string(host_scan.size()) +
                   " csd=" + std::to_string(csd_scan.size())
             : "");

  // ---- 计数断言（须在 Close/Reopen 之前读取：reopen 会新建 ZeroFlushContext，
  // csd_* 计数归零 —— 计数反映首次打开的物化会话）----
  const CsdCounters hc = ReadCsdCounters(host.get());
  const CsdCounters cc = ReadCsdCounters(csd.get());
  fprintf(stderr, "  csd counters  host: files=%llu attempts=%llu fallbacks=%llu\n",
          (unsigned long long)hc.files, (unsigned long long)hc.attempts,
          (unsigned long long)hc.fallbacks);
  fprintf(stderr, "  csd counters  csd : files=%llu attempts=%llu fallbacks=%llu\n",
          (unsigned long long)cc.files, (unsigned long long)cc.attempts,
          (unsigned long long)cc.fallbacks);

  if (xclbin.empty()) {
    // host 回落等价：csd=on 的卸载请求必须全部回落（files==0），且确实走到
    // 卸载尝试点（fallbacks>0）——证明接缝可达且不静默错排。
    Report("csd=on no-device: files==0 && fallbacks>0",
           cc.files == 0 && cc.fallbacks > 0,
           "files=" + std::to_string(cc.files) +
               " fallbacks=" + std::to_string(cc.fallbacks));
  } else {
    // 有 xclbin：若设备会话就绪且语料合格，真实 offload 必须产出文件。
    // 会话不可用（探测失败）→ 等价仍成立（自动回落），不判失败但如实报告。
    Report("csd=on with xclbin: offload produced files",
           cc.files > 0,
           "files=" + std::to_string(cc.files) +
               " attempts=" + std::to_string(cc.attempts) +
               " fallbacks=" + std::to_string(cc.fallbacks));
  }

  // ---- CSD 产物经引擎 reader 重开直读（CRC 强校验路径）----
  if (scan_ok) {
    csd.reset();  // Close：CSD 产物安装于 L0/base，reopen 强制从 SST 读
    s = zeroflush::Open(MakeOptions(), zfo_csd, kCsdDb, &csd);
    if (!s.ok()) {
      Report("csd reopen", false, s.ToString());
    } else {
      std::vector<std::string> csd_scan2;
      bool ok2 = DumpAll(csd.get(), &csd_scan2);
      const bool equal = ok2 && csd_scan2.size() == host_scan.size() &&
                         csd_scan2 == host_scan;
      Report("csd reopen rescan == host (CSD SST CRC read)",
             equal, !equal ? "entry mismatch after reopen" : "");
    }
  }

  host.reset();
  csd.reset();
  CleanDB(kHostDb);
  CleanDB(kCsdDb);
  // 显式释放设备会话（main 作用域内）：若留到 atexit 析构，clReleaseKernel 在
  // XRT context_mgr 拆除后执行 → SEGV（真卡实测）。此时已无后台物化线程。
  zeroflush::ShutdownZeroFlushCsdSession();

  fprintf(stderr, "============================================================\n");
  if (g_failures == 0) {
    fprintf(stderr, " ALL PASSED\n");
  } else {
    fprintf(stderr, " %d FAILED\n", g_failures);
  }
  fprintf(stderr, "============================================================\n");
  return g_failures;
}
