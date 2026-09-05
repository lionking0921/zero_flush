//  Copyright (c) 2026, ZeroFlush-RocksDB.
//
//  ZeroFlush host 侧 "封口器"实现：给 FPGA 已写好的 [data+index 前缀] 追写
//  properties/metaindex/footer 三块（FPGA_SLIM_MEMTABLE_BYTEFORMAT.md §16.5）。
//
//  逐字段镜像引擎 block_based_table_builder.cc 的
//   Rep 构造(1360-1394) + Add()(1566-1570,1642-1656) + Flush/WriteIndexBlock +
//   WritePropertiesBlock(2498-2616) + Finish(2887-2915) 在"写 properties 块之前"
//  时点对 rep_->props 的赋值；properties/metaindex/footer 本体复用它自己的
//   PropertyBlockBuilder/MetaIndexBuilder/FooterBuilder，保证 byte-exact。
//
//  锁档（§14.6，见 zf_seal.h）：format_version==2、kCRC32c、kBinarySearch、
//  kNoCompression、无 filter / prefix / merge / 显式 compression-manager /
//  外部 property collector（internal 与 user 两层都要空）。不满足返回
//  InvalidArgument，绝不偷偷降级。

#include "zeroflush/zf_seal.h"

#include <cstdint>
#include <string>
#include <vector>

#include "file/writable_file_writer.h"
#include "options/cf_options.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "rocksdb/table_properties.h"
#include "table/format.h"
#include "table/meta_blocks.h"
#include "table/table_builder.h"
#include "util/compression.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

namespace {

// SST 固定 5B block trailer：1B compression type + 4B checksum。
// == BlockBasedTable::kBlockTrailerSize（block_based_table_reader.h:78）
constexpr size_t kZfBlockTrailerSize = 5;

// 校验 §14.6 锁定档位。失败时 why 收到给上层看的具体字段。
bool ZfSealLockedConfigOk(const ZfSealOptions& options, std::string* why) {
  const TableBuilderOptions& tbo = *options.tboptions;
  const BlockBasedTableOptions& topt = *options.table_options;
  const ImmutableOptions& ioptions = tbo.ioptions;

#define ZF_GATE(cond, msg)              \
  do {                                  \
    if (!(cond)) {                      \
      *why = msg;                       \
      return false;                     \
    }                                   \
  } while (0)

  ZF_GATE(topt.format_version == 2,
          "only format_version == 2 is supported (context checksum / "
          "compression-manager / footer layouts are versioned)");
  ZF_GATE(topt.checksum == kCRC32c, "only kCRC32c checksum is supported");
  ZF_GATE(topt.index_type == BlockBasedTableOptions::kBinarySearch,
          "only kBinarySearch index is supported");
  ZF_GATE(topt.filter_policy == nullptr, "filter_policy must be nullptr");
  ZF_GATE(!topt.partition_filters,
          "partition_filters must be false (no filter block)");
  ZF_GATE(tbo.compression_type == kNoCompression,
          "compression_type must be kNoCompression");
  ZF_GATE(tbo.moptions.compression_manager == nullptr,
          "explicit compression manager not supported");
  ZF_GATE(tbo.moptions.prefix_extractor == nullptr,
          "prefix_extractor must be nullptr");
  ZF_GATE(tbo.ioptions.merge_operator == nullptr,
          "merge_operator must be nullptr");
  ZF_GATE(tbo.ioptions.table_properties_collector_factories.empty(),
          "table properties collector factories must be empty");
  ZF_GATE(tbo.internal_tbl_prop_coll_factories == nullptr ||
              tbo.internal_tbl_prop_coll_factories->empty(),
          "internal table properties collector factories must be empty");
#undef ZF_GATE

  return true;
}

// 装配"引擎在 WritePropertiesBlock() 时点持有的 rep_->props"的那份
// TableProperties（只差 properties 块序列化本身）。调用方再以
// PropertyBlockBuilder::AddTableProperty() 落盘。
Status ZfSealAssembleProperties(const ZfSealOptions& options,
                                const ZfSealManifest& zf, TableProperties* out) {
  std::string why;
  if (!ZfSealLockedConfigOk(options, &why)) {
    return Status::InvalidArgument("ZfSeal unsupported config: " + why);
  }
  const TableBuilderOptions& tbo = *options.tboptions;
  const BlockBasedTableOptions& topt = *options.table_options;
  const ImmutableOptions& ioptions = tbo.ioptions;

  TableProperties& props = *out;
  // 装配起点与引擎一致：默认构造的 TableProperties。下面只覆盖引擎会覆盖的
  // 字段（其余保持 TableProperties 默认值，与引擎 rep_->props 完全一致）。

  // == BlockBasedTableBuilder::Rep 构造（1360-1394）赋值 ==
  props.column_family_id = tbo.column_family_id;
  props.column_family_name = tbo.column_family_name;
  props.oldest_key_time = tbo.oldest_key_time;
  props.newest_key_time = tbo.newest_key_time;
  props.file_creation_time = tbo.file_creation_time;
  props.orig_file_number = tbo.cur_file_num;
  props.db_id = tbo.db_id;
  props.db_session_id = tbo.db_session_id;
  props.db_host_id = ioptions.db_host_id;
  props.format_version = topt.format_version;
  props.data_block_restart_interval = topt.block_restart_interval;
  props.index_block_restart_interval = topt.index_block_restart_interval;
  props.separate_key_value_in_data_block =
      topt.separate_key_value_in_data_block ? 1 : 0;
  ReifyDbHostIdProperty(ioptions.env, &props.db_host_id).PermitUncheckedError();
  // key seq 界：引擎 Add() 逐条取 max/min（1569-1570）。manifest 已含最终值。
  props.key_largest_seqno = zf.key_largest_seqno;
  props.key_smallest_seqno = zf.key_smallest_seqno;

  // == Add() 累加统计（1642-1656）==，均来自 FPGA 计数。
  props.raw_key_size = zf.raw_key_size;
  props.raw_value_size = zf.raw_value_size;
  props.num_entries = zf.num_entries;
  props.num_deletions = zf.num_deletions;
  props.num_merge_operands = zf.num_merge_operands;
  props.num_filter_entries = zf.num_filter_entries;
  props.num_data_blocks = zf.num_data_blocks;
  props.num_range_deletions = zf.num_range_deletions;
  props.num_uniform_blocks = zf.num_uniform_blocks;
  // 无并行压缩 → rejected/bypassed：引擎在 kNoCompression 下每个 data block
  // 写一次都计 compression_bypassed（builder.cc:2057-2072 compression_attempted
  // == false 分支），故 == num_data_blocks；rejected 恒 0（AddTableProperty 对 0
  // 不落盘，两侧一致）。
  props.num_data_blocks_compression_bypassed = zf.num_data_blocks;
  props.filter_size = zf.filter_size;
  // 文件侧几何：引擎 Finish() 在 Flush() 之后、WriteIndexBlock() 之前取
  // tail_start_offset（builder.cc:2887），故它== data 区末尾 == index 块起始
  // == props.data_size（不是 properties 块起始）。index 块由 FPGA 已写在
  // 该偏移，props.index_size == index 内容+5B trailer。
  props.data_size = zf.data_size;
  props.index_size = zf.index_size;
  props.tail_start_offset = zf.data_size;

  // == WritePropertiesBlock() 覆盖的选项派生字段（2503-2578）==
  props.filter_policy_name =
      topt.filter_policy != nullptr ? topt.filter_policy->Name() : "";
  props.comparator_name =
      ioptions.user_comparator != nullptr ? ioptions.user_comparator->Name()
                                          : "nullptr";
  props.merge_operator_name =
      ioptions.merge_operator != nullptr ? ioptions.merge_operator->Name()
                                         : "nullptr";
  props.prefix_extractor_name =
      tbo.moptions.prefix_extractor != nullptr
          ? tbo.moptions.prefix_extractor->AsString()
          : "nullptr";
  std::string property_collectors_names = "[";
  for (size_t i = 0; i < ioptions.table_properties_collector_factories.size();
       ++i) {
    if (i != 0) {
      property_collectors_names += ",";
    }
    property_collectors_names +=
        ioptions.table_properties_collector_factories[i]->Name();
  }
  property_collectors_names += "]";
  props.property_collectors_names = property_collectors_names;
  // kBinarySearch 下引擎用 ShortenedIndexBuilder：v2 索引键带 seq 的
  // separator（index_builder.h:257 must_use_separator_with_seq_ = (v<=2)），
  // 因此 index_key_is_user_key == false。
  props.index_key_is_user_key = false;
  // use_delta_encoding_for_index_values = (format_version>=4 && !block_align)
  // == false（v2）。
  props.index_value_is_delta_encoded = false;
  props.user_defined_timestamps_persisted =
      ioptions.persist_user_defined_timestamps ? 1 : 0;
  // v2 legacy compression naming：无压缩 → "NoCompression"。
  props.compression_name = CompressionTypeToString(tbo.compression_type);
  props.compression_options = CompressionOptionsToString(tbo.compression_opts);

  return Status::OK();
}

// 追加一个"未压缩块 + 5B trailer"：与引擎 WriteMaybeCompressedBlockImpl
// (2100-2242) 的 trailer 口径一致：trailer[0]=kNoCompression(0)；
// checksum = ComputeBuiltinChecksumWithLastByte(kCRC32c, content, type_byte)
// + ChecksumModifierForContext(base=0, ...)（v2 → 0）。
Status ZfSealAppendBlock(const ZfSealOptions& options, WritableFileWriter* file,
                         const IOOptions& io_opts, uint64_t* offset,
                         const Slice& contents, BlockHandle* handle) {
  const BlockBasedTableOptions& topt = *options.table_options;
  handle->set_offset(*offset);
  handle->set_size(contents.size());
  IOStatus ios = file->Append(io_opts, contents);
  if (!ios.ok()) {
    return ios;
  }
  std::array<char, kZfBlockTrailerSize> trailer;
  trailer[0] = static_cast<char>(kNoCompression);
  const uint32_t checksum = ComputeBuiltinChecksumWithLastByte(
      topt.checksum, contents.data(), contents.size(),
      /*last_byte*/ static_cast<char>(kNoCompression));
  EncodeFixed32(trailer.data() + 1, checksum);
  ios = file->Append(io_opts, Slice(trailer.data(), trailer.size()));
  if (!ios.ok()) {
    return ios;
  }
  *offset += contents.size() + kZfBlockTrailerSize;
  return Status::OK();
}

}  // namespace

Status ZfSealBuildTableProperties(const ZfSealOptions& options,
                                  const ZfSealManifest& manifest,
                                  TableProperties* out_props) {
  if (options.tboptions == nullptr || options.table_options == nullptr) {
    return Status::InvalidArgument("ZfSealOptions must be populated");
  }
  return ZfSealAssembleProperties(options, manifest, out_props);
}

Status ZfSeal(const ZfSealOptions& options, const ZfSealManifest& manifest,
              WritableFileWriter* writer) {
  if (options.tboptions == nullptr || options.table_options == nullptr ||
      writer == nullptr) {
    return Status::InvalidArgument("ZfSeal: options/writer must be populated");
  }
  const TableBuilderOptions& tbo = *options.tboptions;
  const BlockBasedTableOptions& topt = *options.table_options;

  if (manifest.index_size < kZfBlockTrailerSize) {
    return Status::InvalidArgument(
        "ZfSeal: manifest.index_size too small for a block trailer");
  }

  // 装配 props 并序列化 properties 块（镜像 WritePropertiesBlock：
  // AddTableProperty + 内置 BlockBasedTablePropertiesCollector 的 Finish）。
  TableProperties props;
  Status s = ZfSealAssembleProperties(options, manifest, &props);
  if (!s.ok()) {
    return s;
  }

  IOOptions io_opts;
  IOStatus ios = WritableFileWriter::PrepareIOOptions(tbo.write_options, io_opts);
  if (!ios.ok()) {
    return ios;
  }

  // writer 已由调用方定位到前缀末尾；偏移自 manifest 推导并随写入推进。
  uint64_t offset = manifest.data_size + manifest.index_size;

  // 1. properties block + 5B trailer
  BlockHandle props_handle;
  {
    PropertyBlockBuilder pbb;
    pbb.AddTableProperty(props);
    // 镜像 builder.cc:1348-1352 内置 BlockBasedTablePropertiesCollector 在
    // Finish()（builder.cc:168-181）产出的三（或四）个 user 属性；这些键与
    // AddTableProperty 的键共同进入 pbb 内部有序 map，Finish 时按全键排序。
    UserCollectedProperties builtin;
    std::string index_type_val;
    PutFixed32(&index_type_val, static_cast<uint32_t>(topt.index_type));
    builtin[BlockBasedTablePropertyNames::kIndexType] =
        std::move(index_type_val);
    builtin[BlockBasedTablePropertyNames::kWholeKeyFiltering] =
        topt.whole_key_filtering ? "1" : "0";
    builtin[BlockBasedTablePropertyNames::kPrefixFiltering] =
        tbo.moptions.prefix_extractor != nullptr ? "1" : "0";
    if (topt.decouple_partitioned_filters) {
      builtin[BlockBasedTablePropertyNames::kDecoupledPartitionedFilters] = "1";
    }
    pbb.Add(builtin);
    Slice block = pbb.Finish();
    s = ZfSealAppendBlock(options, writer, io_opts, &offset, block,
                          &props_handle);
    if (!s.ok()) {
      return s;
    }
  }

  // 2. metaindex block + 5B trailer（仅含 kPropertiesBlockName；v2 下
  // index 句柄在 footer 中，FormatVersionUsesIndexHandleInFooter(2)==true）
  BlockHandle metaindex_handle;
  {
    MetaIndexBuilder mib;
    mib.Add(kPropertiesBlockName, props_handle);
    Slice block = mib.Finish();
    s = ZfSealAppendBlock(options, writer, io_opts, &offset, block,
                          &metaindex_handle);
    if (!s.ok()) {
      return s;
    }
  }

  // 3. footer（53B @ v2）：index_handle.offset == data_size，
  //    size == index 内容（不含 5B trailer）。
  BlockHandle index_handle(manifest.data_size,
                           manifest.index_size - kZfBlockTrailerSize);
  FooterBuilder footer;
  s = footer.Build(kBlockBasedTableMagicNumber, topt.format_version, offset,
                   topt.checksum, metaindex_handle, index_handle,
                   /*base_context_checksum*/ 0);
  if (!s.ok()) {
    return s;
  }
  ios = writer->Append(io_opts, footer.GetSlice());
  if (!ios.ok()) {
    return ios;
  }
  return Status::OK();
}

void ZfSealManifestFromTableProperties(const TableProperties& props,
                                       ZfSealManifest* manifest) {
  manifest->data_size = props.data_size;
  manifest->index_size = props.index_size;
  manifest->num_entries = props.num_entries;
  manifest->num_deletions = props.num_deletions;
  manifest->num_merge_operands = props.num_merge_operands;
  manifest->num_filter_entries = props.num_filter_entries;
  manifest->num_data_blocks = props.num_data_blocks;
  manifest->num_range_deletions = props.num_range_deletions;
  manifest->num_uniform_blocks = props.num_uniform_blocks;
  manifest->raw_key_size = props.raw_key_size;
  manifest->raw_value_size = props.raw_value_size;
  manifest->filter_size = props.filter_size;
  manifest->key_largest_seqno = props.key_largest_seqno;
  manifest->key_smallest_seqno = props.key_smallest_seqno;
}

}  // namespace ROCKSDB_NAMESPACE
