// zf_seal_check.cc —— ZeroFlush FPGA aux-sort 内核产物 封口 + 引擎直读对拍
//
// (里程碑 3 阶段 D/E) 引擎树内小工具：消费内核/CPU-sim dump 的 "前缀"
//（[PPS+data+index]，format_version=2 / kCRC32c / kBinarySearch，data/index 每块
// 5B trailer 带 masked crc32c），用 ZfSealManifest + ZfSeal 封成完整 SST
//（追写 [properties][metaindex][footer]），再经引擎自身 SstFileReader / TableReader
// 字节直读回放：全键（含删除 tombstone、含 8B seq/type 尾）逐条以
// `hex(32B 内部键)\t hex(值)` 打印到 stdout，供外部与 CPU-sim 的 .expect 对拍；
// 校验档位即 §14.6 锁档。引擎 TableReader 逐块强校验 masked crc32c —— crc/格式
// 任一字节不对会在 Open/Iterator 期以 Corruption 暴露。
//
// 用法：
//   zf_seal_check <in.prefix> [out.sst]
//     in.prefix 布局：8B magic "ZFPRFX1\n" + u64 data_size + u64 index_size
//                    + u64 num_deletions + PPS_KERNEL_SIZE×u64 + data 字节 + index 字节
//     out.sst 缺省 = in.prefix 去后缀 + ".sst"（前缀目录内，勿写只读源目录）
//   正常退出码 0，且 stdout 可直接与 .expect diff（两处 hex 均为小写）。
//   注意：若宿主内核缺 IORING_SETUP_SINGLE_ISSUER/DEFER_TASKRUN，引擎 Env 首次 MultiRead
//   会往 stdout 打一行 "CreateIOUring failed: ..." 探测噪音 —— 对拍前先 grep -v 该行。
//
// 编译：cmake 目标 zf_seal_check（链 librocksdb）。引擎零源码改动。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "file/writable_file_writer.h"
#include "options/cf_options.h"
#include "rocksdb/comparator.h"
#include "rocksdb/file_system.h"
#include "rocksdb/options.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "rocksdb/table_properties.h"
#include "table/table_builder.h"
#include "zeroflush/zf_seal.h"

namespace ROCKSDB_NAMESPACE {
namespace {

// PPS 区 = output_data[0..PPS_KERNEL_SIZE)，其中 PPS_KERNEL_SIZE =
// PPS_KERNEL_SINGEL_SIZE(128) × MAX_OUTPUT_FILE_NUM(4) = 512 words = 4096B。
// 前缀文件把整段 512 u64 落盘；本工具解析 data/index 必须跳过这 4096B（否则错位 3072B）。
constexpr uint64_t kPpsWords = 512;  // == PPS_KERNEL_SIZE

std::string StripExt(const std::string& p) {
  size_t dot = p.rfind('.');
  return dot == std::string::npos ? p : p.substr(0, dot);
}

// ---- 前缀文件解析 ----
struct Prefix {
  uint64_t data_size = 0;
  uint64_t index_size = 0;
  uint64_t num_deletions = 0;
  std::vector<uint64_t> pps;    // kPpsWords 个
  std::vector<uint8_t> data;    // data_size 字节（含各块 5B trailer）
  std::vector<uint8_t> index;   // index_size 字节（含 index 块 5B trailer）
};

bool LoadPrefix(const std::string& path, Prefix* p) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    fprintf(stderr, "zf_seal_check: cannot open %s\n", path.c_str());
    return false;
  }
  auto rd = [&](void* d, size_t n) { return fread(d, 1, n, f) == n; };
  char magic[9] = {0};
  if (!rd(magic, 8) || strncmp(magic, "ZFPRFX1\n", 8) != 0) {
    fprintf(stderr, "zf_seal_check: %s: bad magic\n", path.c_str());
    fclose(f);
    return false;
  }
  uint64_t hdr[3];
  if (!rd(hdr, sizeof(hdr))) {
    fprintf(stderr, "zf_seal_check: %s: short header\n", path.c_str());
    fclose(f);
    return false;
  }
  p->data_size = hdr[0];
  p->index_size = hdr[1];
  p->num_deletions = hdr[2];
  p->pps.resize(kPpsWords);
  if (!rd(p->pps.data(), sizeof(uint64_t) * kPpsWords)) {
    fprintf(stderr, "zf_seal_check: %s: short pps\n", path.c_str());
    fclose(f);
    return false;
  }
  p->data.resize(p->data_size);
  p->index.resize(p->index_size);
  if (p->data_size && !rd(p->data.data(), p->data_size)) {
    fprintf(stderr, "zf_seal_check: %s: short data (want %llu)\n", path.c_str(),
            (unsigned long long)p->data_size);
    fclose(f);
    return false;
  }
  if (p->index_size && !rd(p->index.data(), p->index_size)) {
    fprintf(stderr, "zf_seal_check: %s: short index (want %llu)\n", path.c_str(),
            (unsigned long long)p->index_size);
    fclose(f);
    return false;
  }
  fclose(f);
  return true;
}

std::string Hex(const Slice& s) {
  static const char* d = "0123456789abcdef";
  std::string h;
  h.reserve(s.size() * 2);
  for (size_t i = 0; i < s.size(); ++i) {
    h.push_back(d[(uint8_t)s[i] >> 4]);
    h.push_back(d[(uint8_t)s[i] & 0xf]);
  }
  return h;
}

int Run(const std::string& in, const std::string& out_sst) {
  Prefix pfx;
  if (!LoadPrefix(in, &pfx)) return 2;
  if (pfx.pps.size() < kPpsWords) return 2;
  const uint64_t* pps = pfx.pps.data();

  // ---- §14.6 锁定档位选项（封口与直读共用同一份） ----
  Options opts;
  opts.compression = kNoCompression;  // 默认 Options 是 Snappy，必须显式关掉
  BlockBasedTableOptions bbt;
  bbt.format_version = 2;
  bbt.checksum = kCRC32c;
  bbt.index_type = BlockBasedTableOptions::kBinarySearch;
  // 其余保持默认：无 filter、无 partition filter、block_restart_interval=16、
  // index_block_restart_interval=1。
  opts.table_factory.reset(NewBlockBasedTableFactory(bbt));

  // ---- ZfSealManifest：host 从 PPS 计数组装 ----
  ZfSealManifest m;
  m.data_size = pfx.data_size;
  m.index_size = pfx.index_size;
  m.num_deletions = pfx.num_deletions;
  m.num_entries = pps[1];
  m.num_data_blocks = pps[0];
  m.raw_key_size = pps[3];
  m.raw_value_size = pps[4];
  // pps[6]/[7] = 全体记录 footer 极小/极大值（seq<<8|type），seq = >>8。
  m.key_largest_seqno = pps[7] >> 8;
  m.key_smallest_seqno = pps[6] >> 8;
  // num_merge_operands/filter/range_deletion/uniform_blocks 均 0 —— 本档无这些。

  if (m.index_size < 5) {
    fprintf(stderr, "zf_seal_check: index too small (%llu)\n",
            (unsigned long long)m.index_size);
    return 2;
  }

  // ---- 打开输出并写 [data+index] 前缀，随后 ZfSeal 封口 ----
  std::unique_ptr<FSWritableFile> file;
  FileOptions fo;
  IOStatus ios = FileSystem::Default()->NewWritableFile(out_sst, fo, &file,
                                                        nullptr);
  if (!ios.ok()) {
    fprintf(stderr, "zf_seal_check: NewWritableFile %s: %s\n", out_sst.c_str(),
            ios.ToString().c_str());
    return 2;
  }
  SystemClock* clock = SystemClock::Default().get();
  WritableFileWriter writer(std::move(file), out_sst, fo, clock);

  ImmutableOptions iopt(opts);
  MutableCFOptions mcf(opts);
  ReadOptions ro;
  WriteOptions wo;
  int64_t now = 0;
  Env::Default()->GetCurrentTime(&now).PermitUncheckedError();
  if (now == 0) now = 1;
  TableBuilderOptions tbopt(iopt, mcf, ro, wo, iopt.internal_comparator,
                            /*internal_tbl_prop_coll_factories*/ nullptr,
                            kNoCompression, opts.compression_opts,
                            /*cf_id*/ 0, /*cf_name*/ "default",
                            /*level*/ 0, now, /*is_bottommost*/ false,
                            TableFileCreationReason::kFlush,
                            /*oldest_key_time*/ 0,
                            /*file_creation_time*/ static_cast<uint64_t>(now),
                            /*db_id*/ "", /*db_session_id*/ "",
                            /*target_file_size*/ 0,
                            /*cur_file_num*/ 1,
                            kMaxSequenceNumber);
  ZfSealOptions zfopt;
  zfopt.tboptions = &tbopt;
  zfopt.table_options = &bbt;

  if (pfx.data_size) {
    ios = writer.Append(IOOptions(),
                        Slice(reinterpret_cast<const char*>(pfx.data.data()),
                              pfx.data.size()));
    if (!ios.ok()) {
      fprintf(stderr, "zf_seal_check: append data: %s\n", ios.ToString().c_str());
      return 2;
    }
  }
  if (pfx.index_size) {
    ios = writer.Append(IOOptions(),
                        Slice(reinterpret_cast<const char*>(pfx.index.data()),
                              pfx.index.size()));
    if (!ios.ok()) {
      fprintf(stderr, "zf_seal_check: append index: %s\n", ios.ToString().c_str());
      return 2;
    }
  }
  Status s = ZfSeal(zfopt, m, &writer);
  if (!s.ok()) {
    fprintf(stderr, "zf_seal_check: ZfSeal: %s\n", s.ToString().c_str());
    return 2;
  }
  ios = writer.Close(IOOptions());
  if (!ios.ok()) {
    fprintf(stderr, "zf_seal_check: close: %s\n", ios.ToString().c_str());
    return 2;
  }

  // ---- 引擎 TableReader 字节直读 ----
  fprintf(stderr,
          "sealed %s: data=%llu index=%llu entries=%llu del=%llu blocks=%llu\n",
          out_sst.c_str(), (unsigned long long)pfx.data_size,
          (unsigned long long)pfx.index_size, (unsigned long long)m.num_entries,
          (unsigned long long)m.num_deletions,
          (unsigned long long)m.num_data_blocks);

  SstFileReader reader(opts);
  s = reader.Open(out_sst);
  if (!s.ok()) {
    fprintf(stderr, "zf_seal_check: SstFileReader.Open: %s\n",
            s.ToString().c_str());
    return 2;
  }
  // 逐块强校验 masked crc32c（覆盖 index + 每个 data 块）。
  s = reader.VerifyChecksum(ReadOptions());
  if (!s.ok()) {
    fprintf(stderr, "zf_seal_check: VerifyChecksum: %s\n", s.ToString().c_str());
    return 2;
  }
  s = reader.VerifyNumEntries(ReadOptions());
  if (!s.ok()) {
    fprintf(stderr, "zf_seal_check: VerifyNumEntries: %s\n",
            s.ToString().c_str());
    return 2;
  }

  uint64_t n = 0;
  std::unique_ptr<Iterator> it = reader.NewTableIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    Slice k = it->key();
    Slice v = it->value();
    std::string line = Hex(k);
    line.push_back('\t');
    line += Hex(v);
    line.push_back('\n');
    fwrite(line.data(), 1, line.size(), stdout);
    ++n;
  }
  s = it->status();
  if (!s.ok()) {
    fprintf(stderr, "zf_seal_check: iterate: %s\n", s.ToString().c_str());
    return 2;
  }
  if (n != m.num_entries) {
    fprintf(stderr,
            "zf_seal_check: read-back count %llu != manifest num_entries %llu\n",
            (unsigned long long)n, (unsigned long long)m.num_entries);
    return 2;
  }
  std::shared_ptr<const TableProperties> tp = reader.GetTableProperties();
  if (tp) {
    fprintf(stderr,
            "  props: comparator=%s data_size=%llu index_size=%llu raw_key=%llu "
            "raw_val=%llu min_seq=%llu max_seq=%llu\n",
            tp->comparator_name.c_str(), (unsigned long long)tp->data_size,
            (unsigned long long)tp->index_size,
            (unsigned long long)tp->raw_key_size,
            (unsigned long long)tp->raw_value_size,
            (unsigned long long)tp->key_smallest_seqno,
            (unsigned long long)tp->key_largest_seqno);
  }
  fprintf(stderr, "read-back %llu entries, CRC+parse clean\n",
          (unsigned long long)n);
  fflush(stdout);
  return 0;
}

}  // namespace
}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: zf_seal_check <in.prefix> [out.sst]\n");
    return 2;
  }
  std::string in = argv[1];
  std::string out = (argc >= 3) ? argv[2] : (ROCKSDB_NAMESPACE::StripExt(in) + ".sst");
  return ROCKSDB_NAMESPACE::Run(in, out);
}
