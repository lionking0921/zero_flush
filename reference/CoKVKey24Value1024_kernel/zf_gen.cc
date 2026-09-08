// zf_gen.cc -- standalone CoKV-native SST generator (CPU-only, no XRT dependency).
// Usage: zf_gen OUTDIR [--nf 1..4] [--each ENTRIES_PER_FILE] [--start COUNTER]
//   Writes NF CoKV .sst files (in0.sst..) into OUTDIR over a contiguous ascending key range.
//   Prints per-file path/bytes/entries and total, for use by zf_bench / sst_dump self-check.
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "zf_sst_defs.h"
#include "zf_sst_writer.h"

static void usage(const char* a0) {
    fprintf(stderr,
            "usage: %s OUTDIR [--nf N(1..4,def 1)] [--each ENTRIES(per file, def 1000)] "
            "[--start COUNTER(def 0)]\n", a0);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    std::string outdir = argv[1];
    int nf = 1;
    uint64_t each = 1000, start = 0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--nf") && i + 1 < argc) nf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--each") && i + 1 < argc) each = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) start = strtoull(argv[++i], nullptr, 10);
        else { usage(argv[0]); return 2; }
    }
    if (nf < 1 || nf > 4) { fprintf(stderr, "nf must be 1..4\n"); return 2; }

    uint64_t total_entries = 0, total_bytes = 0;
    for (int f = 0; f < nf; ++f) {
        std::string path = outdir + "/in" + std::to_string(f) + ".sst";
        FILE* fp = fopen(path.c_str(), "wb");
        if (!fp) { perror(path.c_str()); return 1; }
        zf::FileSink sink(fp);
        zf::SstWriter w(sink);
        uint64_t base = start + static_cast<uint64_t>(f) * each;
        for (uint64_t i = 0; i < each; ++i) w.add_entry(base + i);
        uint64_t bytes = w.finish();
        fclose(fp);
        total_entries += each;
        total_bytes += bytes;
        printf("%s\t%" PRIu64 " bytes\t%" PRIu64 " entries\n", path.c_str(), bytes, each);
    }
    printf("TOTAL\t%" PRIu64 " bytes\t%" PRIu64 " entries\n", total_bytes, total_entries);
    return 0;
}
