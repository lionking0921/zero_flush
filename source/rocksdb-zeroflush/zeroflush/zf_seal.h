//  Copyright (c) 2026, ZeroFlush-RocksDB.
//  ZeroFlush host 侧 "封口器"：给 FPGA 已写好的 [data+index 前缀] 追写
//  properties/metaindex/footer 三块（FPGA_SLIM_MEMTABLE_BYTEFORMAT.md §16.5）。
//
//  设计约束（本模块只做"序列化镜像"，绝不重造轮子，保证 byte-exact）：
//   - 复用引擎自身 PropertyBlockBuilder / MetaIndexBuilder / FooterBuilder
//     / ComputeBuiltinChecksumWithLastByte（table/meta_blocks.h, table/format.h）；
//   - properties 内容 = "引擎 BlockBasedTableBuilder 在 WritePropertiesBlock()
//     时点会写出的完整 TableProperties"，由 (DB 选项 + ZfSealManifest) 装配，
//     装配规则逐字段镜像 block_based_table_builder.cc 的 ctor + Add() + Finish()；
//   - 目前仅支持 §14.6 锁定档位：format_version == 2、checksum = kCRC32c、
//     index_type = kBinarySearch、kNoCompression、无 filter / prefix / merge /
//     显式 compression-manager / 外部 property collector。其余配置返回
//     Status::InvalidArgument，不偷偷降级。
//
//  使用方（未来 XRT driver / 测试）流程：
//     1. FPGA 产出的 data+index 已写入文件；writer 定位到前缀末尾
//        （偏移 == manifest.data_size + manifest.index_size）。
//     2. 由 FPGA 计数 / 参考属性填 ZfSealManifest。
//     3. 调用 ZfSeal()：顺序追加 [properties][metaindex][footer]。
//  本模块不接触 FileMetaData / manifest / XRT —— 那些是后续接线点的职责。

#pragma once

#include <cstdint>
#include <string>

#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {

class WritableFileWriter;
class TableProperties;
struct TableBuilderOptions;
struct BlockBasedTableOptions;

// 由 FPGA output_data / 参考属性反填的文件内容侧统计（与引擎 props 字段一一对应，
// 见 include/rocksdb/table_properties.h）。字段缺失时保持默认即可（写侧按
// "是否为 0 / UINT64_MAX / 空" 决定该属性键是否落盘，与引擎 AddTableProperty 同规则）。
struct ZfSealManifest {
  // 磁盘几何（FPGA 知道；单位均为字节）
  //  data_size == data 区（含各 5B trailer）末尾偏移 == index 块起始偏移
  //             == props.data_size == footer.index_handle.offset
  //             == props.tail_start_offset（引擎 Finish() 在 Flush() 之后、
  //                WriteIndexBlock() 之前取 r->offset，builder.cc:2887）。
  uint64_t data_size = 0;
  //  index_size == index 块含 5B trailer 的落盘长度 == props.index_size
  //                == footer.index_handle.size + kBlockTrailerSize。
  //  [data+index] 前缀共 data_size+index_size 字节（FPGA 产出）；ZfSeal 在该
  //  偏移处续写 [properties][metaindex][footer]。
  uint64_t index_size = 0;

  // 条目统计（引擎 Add() 累加值；delete 计 num_deletions、value 计空值）
  uint64_t num_entries = 0;
  uint64_t num_deletions = 0;
  uint64_t num_merge_operands = 0;
  uint64_t num_filter_entries = 0;
  uint64_t num_data_blocks = 0;
  uint64_t num_range_deletions = 0;
  uint64_t num_uniform_blocks = 0;
  uint64_t raw_key_size = 0;   // 内部键字节总数（24B user key + 8B footer，含 delete）
  uint64_t raw_value_size = 0; // 值字节总数（tombstone 值长度 0）
  uint64_t filter_size = 0;

  // 内部键 seqno 范围：引擎 ctor 以 key_largest_seqno=0 起步取 max、
  // key_smallest_seqno=UINT64_MAX 起步取 min；空表时 smallest 保持 MAX（不写）。
  uint64_t key_largest_seqno = 0;
  uint64_t key_smallest_seqno = UINT64_MAX;
};

struct ZfSealOptions {
  // 必须是引擎侧同一批选项：构造属性用的列族 / comparator / compression /
  // 表属性收集器 / 文件号 / db 标识等全部取自已跑的 TableBuilderOptions；
  // 表格式（format_version / restart / index 类型）取 BlockBasedTableOptions。
  const TableBuilderOptions* tboptions = nullptr;
  const BlockBasedTableOptions* table_options = nullptr;
};

// 用 ZfSealManifest + DB 选项装配出"引擎 WritePropertiesBlock() 会写出的那份
// TableProperties"（校验用 / 供上层登记）。返回的 props 已含 options 派生字段
// （comparator / merge / prefix / collector / compression 名等）与文件侧统计。
Status ZfSealBuildTableProperties(const ZfSealOptions& options,
                                  const ZfSealManifest& manifest,
                                  TableProperties* out_props);

// 核心封口：writer 已定位到 [data+index] 前缀末尾（偏移 ==
// manifest.data_size + manifest.index_size）时调用，把
// [properties(含 5B trailer)][metaindex(含 5B trailer)][footer 53B]
// 顺序追写到 writer 当前偏移。writer 的写选项取自 options.tboptions->write_options。
Status ZfSeal(const ZfSealOptions& options, const ZfSealManifest& manifest,
              WritableFileWriter* writer);

// 便捷：从引擎 reader 解析出的参考 TableProperties 反填 manifest 的文件侧字段
// （测试 / 未来驱动对拍用）。不拷贝 db_id/选项派生字段——那些由 ZfSealOptions 携带。
void ZfSealManifestFromTableProperties(const TableProperties& props,
                                       ZfSealManifest* manifest);

}  // namespace ROCKSDB_NAMESPACE
