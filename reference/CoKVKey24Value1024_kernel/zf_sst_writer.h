// zf_sst_writer.h -- CoKV-native SST writer (reference decoder byte-format, header-only, CPU-only).
//
// Produces one CoKV SST file whose byte layout the reference decoder
// (reference/CoKVKey24Value1024_kernel/krnl_vadd.cpp decoder()) parses:
//   file = [data blocks (each content + 5B zero trailer)] [index content + 5B zero trailer] [53B footer]
//   data entry  : varint(sh) varint(us) varint(vl) [us key bytes] [vl value bytes]   (vl = 1024 fixed)
//   index entry : varint(sh) varint(us) [us key bytes] varint64(block_off) varint32(block_size)
//                 (we use sh=0 us=0 -> decoder window reads all fields, no vl varint)
//   both blocks: content = entries + restart array (u32 per restart entry) + num_restarts u32
//   restart interval = 1 -> every entry carries full key (sh=0), canonical & decoder-agnostic.
//   footer 53B   : byte0 = 0x00 checksum (decoder skips), varint(0) varint(0) varint(idx_off)
//                  varint(idx_size), zero pad, version u32 LE, magic u64 LE.
// Keys: 24B zero-padded decimal of a global counter (lex==numeric order) + 8B LE (seq<<8)|1.
#ifndef ZF_SST_WRITER_H_
#define ZF_SST_WRITER_H_

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "zf_sst_defs.h"

namespace zf {

// ---- byte sink -------------------------------------------------------------
struct Sink {
    virtual void write(const void* data, size_t n) = 0;
    virtual ~Sink() {}
};
struct VecSink : Sink {
    std::vector<uint8_t>& v;
    explicit VecSink(std::vector<uint8_t>& out) : v(out) {}
    void write(const void* d, size_t n) override {
        const uint8_t* p = static_cast<const uint8_t*>(d);
        v.insert(v.end(), p, p + n);
    }
};
struct FileSink : Sink {
    FILE* f;
    explicit FileSink(FILE* fp) : f(fp) {}
    void write(const void* d, size_t n) override {
        if (n) fwrite(d, 1, n, f);
    }
};

// ---- LEB128 helpers --------------------------------------------------------
inline void put_varint(std::vector<uint8_t>& v, uint64_t val) {
    while (val >= 0x80) {
        v.push_back(static_cast<uint8_t>((val & 0x7f) | 0x80));
        val >>= 7;
    }
    v.push_back(static_cast<uint8_t>(val));
}
inline void put_varint(Sink& s, uint64_t val) {
    uint8_t tmp[10];
    int n = 0;
    while (val >= 0x80) { tmp[n++] = static_cast<uint8_t>((val & 0x7f) | 0x80); val >>= 7; }
    tmp[n++] = static_cast<uint8_t>(val);
    s.write(tmp, n);
}
inline void put_u32_le(Sink& s, uint32_t x) { s.write(&x, 4); }
inline void put_u32_le(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

// ---- key / value generators ------------------------------------------------
// internal key (32B) = 24B user + 8B LE ((seq<<8)|1), seqno == counter (unique ascending).
inline void make_internal_key(uint64_t counter, uint8_t out[32]) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%024llu", static_cast<unsigned long long>(counter));
    memcpy(out, buf, 24);               // 24-digit zero-padded decimal (lex==numeric order)
    uint64_t packed = (counter << 8) | 1;  // (seqno<<8)|kTypeValue(1); byte0 == type
    for (int i = 0; i < 8; ++i) out[24 + i] = static_cast<uint8_t>(packed >> (8 * i));
}
// deterministic pseudo value (1024B), reproducible from counter.
inline void make_value(uint64_t counter, uint8_t out[1024]) {
    uint64_t x = counter * 0x9E3779B97F4A7C15ull + 0xBF58476D1CE4E5B9ull;
    for (int i = 0; i < 1024; i += 8) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        uint64_t y = x * 0x2545F4914F6CDD1Dull;
        memcpy(out + i, &y, 8);
    }
}

// ---- one CoKV SST writer ----------------------------------------------------
class SstWriter {
public:
    SstWriter(Sink& sink) : s_(sink) {}

    // write one entry; auto-flushes data blocks at the CoKV size threshold.
    void add_entry(uint64_t counter) {
        if (!block_.empty() && block_.size() >= kDataBlockCloseBefore) flush_block();
        block_restart_.push_back(static_cast<uint32_t>(block_.size()));  // interval=1: every entry a restart
        // entry: sh=0 us=32 vl=1024
        put_varint(block_, 0);
        put_varint(block_, kKeyLength);
        put_varint(block_, kValueLength);
        uint8_t key[32];
        make_internal_key(counter, key);
        block_.insert(block_.end(), key, key + kKeyLength);
        uint8_t val[1024];
        make_value(counter, val);
        block_.insert(block_.end(), val, val + kValueLength);
        ++entry_in_block_;
        ++entry_total_;
    }

    // flush last data block + index block + trailer + footer; returns total file bytes.
    uint64_t finish() {
        flush_block();                      // trailing partial data block
        flush_index();                      // index content + 5B trailer
        uint64_t file_size = file_pos_;
        (void)file_size;
        // put footer (53B) exactly like host_process.h putFooter
        uint8_t footer[53];
        memset(footer, 0, sizeof(footer));
        std::vector<uint8_t> f;
        put_varint(f, 0);                    // byte0: checksum type (kNoChecksum=0); decoder skips it
        put_varint(f, 0);                    // metaindex handle offset
        put_varint(f, 0);                    // metaindex handle size
        put_varint(f, index_off_);           // index handle offset
        put_varint(f, index_size_);          // index handle size
        // now f.size()-1 bytes filled past byte0; pad until begin+40 where begin = byte1
        size_t body = f.size() - 1;          // bytes after checksum so far
        while (body < 40) { f.push_back(0); ++body; }
        // body now == 40 (bytes 1..40)
        uint32_t version = static_cast<uint32_t>(kFooterVersion);
        for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(version >> (8 * i)));
        uint64_t magic = kFooterMagic;
        for (int i = 0; i < 8; ++i) f.push_back(static_cast<uint8_t>(magic >> (8 * i)));
        if (f.size() != 53) { /* assert */ }
        s_.write(f.data(), f.size());
        file_pos_ += f.size();
        return file_pos_;
    }

    uint64_t entries() const { return entry_total_; }

private:
    void flush_block() {
        if (block_.empty()) return;
        uint32_t num = static_cast<uint32_t>(block_restart_.size());
        uint64_t block_size = block_.size() + static_cast<uint64_t>(block_restart_.size()) * 4 + 4;
        // record index entry first (offset = absolute file position of this block content)
        {
            std::vector<uint8_t>& idx = index_;
            uint64_t entry_start = idx.size();
            put_varint(idx, 0);          // sh
            put_varint(idx, 0);          // us
            put_varint(idx, file_pos_);  // block offset (varint64)
            put_varint(idx, block_size); // block size   (varint32)
            index_restart_.push_back(static_cast<uint32_t>(entry_start));
            ++index_entries_;
        }
        // stream data block content
        s_.write(block_.data(), block_.size());
        file_pos_ += block_.size();
        for (uint32_t off : block_restart_) put_u32_le(s_, off);
        put_u32_le(s_, num);
        file_pos_ += static_cast<uint64_t>(block_restart_.size()) * 4 + 4;
        // 5B zero trailer (kNoChecksum), NOT included in block handle size
        uint64_t zero = 0;
        s_.write(&zero, 5);
        file_pos_ += 5;
        block_.clear();
        block_restart_.clear();
        entry_in_block_ = 0;
    }

    void flush_index() {
        uint32_t num = static_cast<uint32_t>(index_restart_.size());
        index_size_ = index_.size() + static_cast<uint64_t>(index_restart_.size()) * 4 + 4;
        index_off_ = file_pos_;
        s_.write(index_.data(), index_.size());
        file_pos_ += index_.size();
        for (uint32_t off : index_restart_) put_u32_le(s_, off);
        put_u32_le(s_, num);
        file_pos_ += static_cast<uint64_t>(index_restart_.size()) * 4 + 4;
        uint64_t zero = 0;
        s_.write(&zero, 5);                        // index trailer (ignored by decoder)
        file_pos_ += 5;
    }

    static int varint_len(uint64_t val) {
        int n = 0;
        do { val >>= 7; ++n; } while (val);
        return n;
    }

    Sink& s_;
    uint64_t file_pos_ = 0;
    uint64_t entry_total_ = 0;
    uint64_t entry_in_block_ = 0;
    // current data block
    std::vector<uint8_t> block_;
    std::vector<uint32_t> block_restart_;
    // index block (accumulated; entry appended at each data-block flush)
    std::vector<uint8_t> index_;
    std::vector<uint32_t> index_restart_;
    uint64_t index_entries_ = 0;
    uint64_t index_off_ = 0;
    uint64_t index_size_ = 0;
};

}  // namespace zf

#endif
