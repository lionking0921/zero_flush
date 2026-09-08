// zf_sst_defs.h -- host-side numeric constants mirroring CoKV krnl_host.h (no ap_uint dependency)
// Values must stay in sync with reference/CoKVKey24Value1024_kernel/krnl_host.h + host_process.h.
#ifndef ZF_SST_DEFS_H_
#define ZF_SST_DEFS_H_

#include <cstdint>

namespace zf {

static constexpr uint64_t kIndexBlockBufferSize = 1024u * 1024 * 8;  // 8MB (krnl_host.h:4) -- do not enlarge
static constexpr int    kMaxOutputFileNum      = 4;
static constexpr int    kMaxInputFileNum       = 4;
static constexpr int    kKeyLength             = 32;   // KEY_LENGTH  = 1<<5
static constexpr int    kUserKeyLength         = 24;   // KEY_LENGTH - SEQ_LENGTH
static constexpr int    kSeqLength             = 8;
static constexpr int    kValueLength           = 1024; // VALUE_LENGTH = 1<<10
static constexpr int    kFooterSize            = 53;

// PPS layout (krnl_host.h:49-60); each output file window is 128 u64 wide.
static constexpr int    kPpsSingleSize         = 128;
static constexpr int    kPpsKernelSize         = kPpsSingleSize * kMaxOutputFileNum;  // 512
static constexpr int    kPpsDatablockNumOff    = 0;
static constexpr int    kPpsEntriesOff         = 1;
static constexpr int    kPpsDataSizeOff        = 2;
static constexpr int    kPpsRawKeySizeOff      = 3;
static constexpr int    kPpsRawValueSizeOff    = 4;
static constexpr int    kPpsIndexOffsetOff     = 5;
// Host read-back: entries of output file k at pps[k*128 + kPpsEntriesOff],
// data bytes  at pps[kPpsKernelSize + k], index bytes at pps[kPpsKernelSize + kMaxOutputFileNum + k],
// output file count at pps[kPpsKernelSize + 2*kMaxOutputFileNum].
static constexpr int    kPpsOutFileNumOff      = kPpsKernelSize + 2 * kMaxOutputFileNum; // 520

static constexpr uint64_t kFooterMagic         = 9863518390377041911ULL;  // putFooter
static constexpr uint64_t kFooterVersion       = 5;

// block encoding knobs
static constexpr uint64_t kDataBlockCloseBefore = 4080;  // flush current data block when content >= this before next entry
static constexpr int      kDataRestartInterval  = 1;      // every entry sh=0 (canonical, decoder-agnostic)
static constexpr uint64_t kDataRestartSize      = 4;      // restart offsets written every 16 like rocksdb? see writer

inline uint64_t align4k(uint64_t x) { return (x + 4095u) & ~4095ull; }
inline uint64_t align_to(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

}  // namespace zf

#endif
