// zf_bench.cc -- OpenCL test_bench for the CoKV reference compaction kernel
// (compaction_k32v1024_20250825.xclbin, kernel "krnl_vadd").
//
// Modes:
//   (1) real-file validation :  --sst PATH:ENTRIES (repeat up to 4)  -> one launch over those files
//   (2) generated run        :  --gen-total N [--launch L]           -> loop: build 1..4 CoKV files
//                               per launch in memory over a global ascending key range, drive kernel,
//                               read PPS oracle, aggregate kernel_ev to N.
//
// ABI (verified against reference krnl_vadd.cpp):
//   arg0-3 : input file byte buffers (file bytes from offset 0; exact byte/entry counts in host_data)
//   arg4   : host_data[15] u64   [0..3]=0 offset, [4]=budget(->kernel file_limit=ALIGN4K(budget/4)),
//            [5..8]=per-file real bytes, [9]=sum entries, [10..13]=per-file entries, [14]=0
//   arg5   : output data buffer  (>= 4*file_limit)
//   arg6   : index buffer        (index_block_buffer_size = 8MB, krnl_host.h -- do not enlarge)
//   arg7   : PPS buffer u64      (>= PPS_KERNEL_SIZE+20)
//   Output oracle: out file k window entries = pps[k*128 + PPS_ENTRIES_OFF(1)];
//   out file count = pps[PPS_KERNEL_SIZE(512)+8].  Validate sum_k == sum inputs.
#include <CL/cl2.hpp>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "zf_sst_defs.h"
#include "zf_sst_writer.h"

using zf::align4k;
using Clock = std::chrono::high_resolution_clock;
using fsec = std::chrono::duration<double>;

static const char* errName(cl_int e);
#define ERR_EXIT(msg) do { fprintf(stderr, "FATAL: %s (err=%d %s)\n", msg, err, errName(err)); exit(1); } while (0)
static const char* errName(cl_int e) {
    switch (e) {
        case CL_SUCCESS: return "CL_SUCCESS";
        case CL_BUILD_PROGRAM_FAILURE: return "BUILD_PROGRAM_FAILURE";
        case CL_OUT_OF_RESOURCES: return "OUT_OF_RESOURCES";
        case CL_MEM_OBJECT_ALLOCATION_FAILURE: return "MEM_OBJECT_ALLOCATION_FAILURE";
        case CL_INVALID_BUFFER_SIZE: return "INVALID_BUFFER_SIZE";
        case CL_INVALID_MEM_OBJECT: return "INVALID_MEM_OBJECT";
        case CL_INVALID_KERNEL_ARGS: return "INVALID_KERNEL_ARGS";
        default: return "?";
    }
}

struct InFile { std::string path; uint64_t entries; uint64_t counter; };  // path empty => generate from counter

static cl::Device pick_device(int want) {
    std::vector<cl::Platform> plats;
    cl::Platform::get(&plats);
    for (auto& p : plats) {
        if (p.getInfo<CL_PLATFORM_NAME>() != "Xilinx") continue;
        std::vector<cl::Device> devs;
        p.getDevices(CL_DEVICE_TYPE_ACCELERATOR, &devs);
        if (!devs.empty()) return devs[std::min<int>(want, (int)devs.size() - 1)];
    }
    fprintf(stderr, "no Xilinx accelerator device found\n");
    exit(1);
}

// file bytes (zero padded to 4K); real_size = unpadded byte length.
static std::vector<uint8_t> load_padded(const std::string& path, uint64_t& real_size) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) { fprintf(stderr, "cannot open %s\n", path.c_str()); exit(1); }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    real_size = raw.size();
    std::vector<uint8_t> v((size_t)align4k(real_size ? real_size : 1), 0);
    if (real_size) memcpy(v.data(), raw.data(), real_size);
    return v;
}
static std::vector<uint8_t> gen_padded(uint64_t base_counter, uint64_t nentries, uint64_t& real_size) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(nentries * 1070u));
    zf::VecSink sink(raw);
    zf::SstWriter w(sink);
    for (uint64_t i = 0; i < nentries; ++i) w.add_entry(base_counter + i);
    real_size = w.finish();
    raw.resize((size_t)align4k(real_size ? real_size : 1), 0);
    return raw;
}

int main(int argc, char** argv) {
    std::string xclbin = "compaction_k32v1024_20250825.xclbin";
    int device = 0;
    std::vector<InFile> ssts;
    uint64_t gen_total = 0, launch_entries = 800000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* n) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", n); exit(2); }
            return argv[++i];
        };
        if (a == "--xclbin") xclbin = next("--xclbin");
        else if (a == "--device") device = atoi(next("--device"));
        else if (a == "--launch") launch_entries = strtoull(next("--launch"), nullptr, 10);
        else if (a == "--gen-total") gen_total = strtoull(next("--gen-total"), nullptr, 10);
        else if (a == "--sst") {
            std::string s = next("--sst");
            auto c = s.rfind(':');
            if (c == std::string::npos) { fprintf(stderr, "--sst expects PATH:ENTRIES\n"); exit(2); }
            ssts.push_back({ s.substr(0, c), strtoull(s.substr(c + 1).c_str(), nullptr, 10), 0 });
        } else {
            fprintf(stderr, "unknown arg %s\n", a.c_str());
            fprintf(stderr, "usage: %s --xclbin F [--device N] { --sst P:E [..x4] | --gen-total N [--launch L] }\n", argv[0]);
            return 2;
        }
    }
    if (ssts.size() > zf::kMaxInputFileNum) { fprintf(stderr, "at most %d --sst files\n", zf::kMaxInputFileNum); return 2; }
    if (ssts.empty() && gen_total == 0) { fprintf(stderr, "need --sst ... or --gen-total N\n"); return 2; }
    if (gen_total && launch_entries == 0) { fprintf(stderr, "--launch must be > 0\n"); return 2; }

    // ---- OpenCL setup -----------------------------------------------------
    cl_int err = CL_SUCCESS;
    cl::Device dev = pick_device(device);
    std::cout << "device: " << dev.getInfo<CL_DEVICE_NAME>() << "\n";
    cl::Context ctx(dev, nullptr, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) ERR_EXIT("context");
    cl::CommandQueue q(ctx, dev, CL_QUEUE_PROFILING_ENABLE, &err);
    if (err != CL_SUCCESS) ERR_EXIT("queue");

    std::ifstream bin(xclbin, std::ios::binary);
    if (!bin.good()) { fprintf(stderr, "cannot open xclbin %s\n", xclbin.c_str()); return 1; }
    bin.seekg(0, bin.end);
    size_t nb = bin.tellg();
    bin.seekg(0, bin.beg);
    std::vector<unsigned char> xbuf(nb);
    bin.read(reinterpret_cast<char*>(xbuf.data()), nb);
    bin.close();
    cl::Program::Binaries bins;
    bins.push_back(std::move(xbuf));
    cl::Program prog(ctx, {dev}, bins, nullptr, &err);
    if (err != CL_SUCCESS) ERR_EXIT("program from binary");
    cl::Kernel krnl(prog, "krnl_vadd", &err);
    if (err != CL_SUCCESS) ERR_EXIT("kernel krnl_vadd");

    cl::Buffer host_data_buf(ctx, CL_MEM_READ_WRITE, sizeof(uint64_t) * 15, nullptr, &err);
    if (err != CL_SUCCESS) ERR_EXIT("host_data buffer");
    cl::Buffer idx_buf(ctx, CL_MEM_READ_WRITE, zf::kIndexBlockBufferSize, nullptr, &err);
    if (err != CL_SUCCESS) ERR_EXIT("index buffer");
    const int kPpsU64 = zf::kPpsKernelSize + 20;
    cl::Buffer pps_buf(ctx, CL_MEM_READ_WRITE, sizeof(uint64_t) * kPpsU64, nullptr, &err);
    if (err != CL_SUCCESS) ERR_EXIT("pps buffer");
    krnl.setArg(6, idx_buf);
    krnl.setArg(7, pps_buf);

    // ---- launch plan ------------------------------------------------------
    struct Launch { InFile files[zf::kMaxInputFileNum]; int nfiles; };
    std::vector<Launch> launches;
    uint64_t global_base = 0;
    if (!ssts.empty()) {
        Launch L; L.nfiles = (int)ssts.size();
        for (int i = 0; i < L.nfiles; ++i) L.files[i] = ssts[i];
        launches.push_back(L);
    } else {
        uint64_t produced = 0;
        while (produced < gen_total) {
            uint64_t this_l = std::min(launch_entries, gen_total - produced);
            Launch L;
            L.nfiles = (int)std::min<uint64_t>(zf::kMaxInputFileNum, this_l);
            uint64_t rem = this_l, base = global_base;
            for (int f = 0; f < L.nfiles; ++f) {
                uint64_t e = rem / (L.nfiles - f);
                L.files[f] = { "", e, base };
                base += e; rem -= e;
            }
            global_base += this_l;
            launches.push_back(L);
            produced += this_l;
        }
    }

    // ---- run launches -----------------------------------------------------
    uint64_t agg_entries = 0, agg_in_bytes = 0;
    double agg_kernel_s = 0, agg_wall_s = 0, agg_gen_s = 0, agg_io_s = 0;
    int nbad = 0;
    printf("%-6s %10s %12s %10s %10s %10s %6s %9s %s\n",
           "launch", "entries", "in_bytes", "gen_ms", "kernel_ms", "wall_ms", "ofile", "out_ent", "ok");

    for (int li = 0; li < (int)launches.size(); ++li) {
        Launch& L = launches[li];
        auto twall0 = Clock::now();

        // generate/load inputs
        auto tg0 = Clock::now();
        std::vector<uint8_t> vecs[zf::kMaxInputFileNum];
        uint64_t real_sizes[zf::kMaxInputFileNum] = {0};
        uint64_t total_bytes = 0, total_entries = 0;
        for (int i = 0; i < L.nfiles; ++i) {
            uint64_t real = 0;
            vecs[i] = L.files[i].path.empty()
                          ? gen_padded(L.files[i].counter, L.files[i].entries, real)
                          : load_padded(L.files[i].path, real);
            real_sizes[i] = real;
            total_bytes += real;
            total_entries += L.files[i].entries;
        }
        double gen_s = std::chrono::duration_cast<fsec>(Clock::now() - tg0).count();
        agg_gen_s += gen_s;

        // host_data + buffers (ABI above)
        uint64_t budget = align4k(total_bytes + 5200u * zf::kMaxInputFileNum);
        uint64_t file_limit = align4k(budget / zf::kMaxOutputFileNum);
        uint64_t out_bytes = 4u * file_limit;                       // >= kernel write span (4 regions)

        std::vector<uint64_t> hd(15, 0);
        hd[4] = budget;
        for (int i = 0; i < L.nfiles; ++i) {
            hd[5 + i] = real_sizes[i];
            hd[10 + i] = L.files[i].entries;
        }
        hd[9] = total_entries;

        // stage inputs to device (blocking writes)
        auto tio0 = Clock::now();
        q.enqueueWriteBuffer(host_data_buf, CL_TRUE, 0, sizeof(uint64_t) * 15, hd.data());
        cl::Buffer in_buf[zf::kMaxInputFileNum];
        for (int i = 0; i < L.nfiles; ++i) {
            in_buf[i] = cl::Buffer(ctx, CL_MEM_READ_WRITE, vecs[i].size(), nullptr, &err);
            if (err != CL_SUCCESS) ERR_EXIT("input buffer");
            q.enqueueWriteBuffer(in_buf[i], CL_TRUE, 0, vecs[i].size(), vecs[i].data());
            krnl.setArg(i, in_buf[i]);
        }
        // unused input args -> dummy small buffer (kernel skips when size/entries == 0)
        for (int i = L.nfiles; i < zf::kMaxInputFileNum; ++i) {
            static uint8_t dummy[4096] = {0};
            cl::Buffer d(ctx, CL_MEM_READ_WRITE, sizeof(dummy), nullptr, &err);
            q.enqueueWriteBuffer(d, CL_TRUE, 0, sizeof(dummy), dummy);
            krnl.setArg(i, d);
        }
        cl::Buffer out_buf(ctx, CL_MEM_READ_WRITE, out_bytes, nullptr, &err);
        if (err != CL_SUCCESS) ERR_EXIT("output buffer");
        krnl.setArg(4, host_data_buf);
        krnl.setArg(5, out_buf);
        agg_io_s += std::chrono::duration_cast<fsec>(Clock::now() - tio0).count();

        // launch + profile (dataflow kernel runs as a single OpenCL task, see reference vadd.cpp:420)
        cl::Event ev;
        err = q.enqueueTask(krnl, nullptr, &ev);
        if (err != CL_SUCCESS) ERR_EXIT("enqueueTask");
        ev.wait();
        uint64_t t0 = ev.getProfilingInfo<CL_PROFILING_COMMAND_START>(&err);
        uint64_t t1 = ev.getProfilingInfo<CL_PROFILING_COMMAND_END>(&err);
        if (err != CL_SUCCESS) ERR_EXIT("profiling");
        double kernel_s = (double)(t1 - t0) / 1e9;

        // read PPS oracle
        std::vector<uint64_t> pps(kPpsU64, 0);
        q.enqueueReadBuffer(pps_buf, CL_TRUE, 0, sizeof(uint64_t) * kPpsU64, pps.data());
        q.finish();

        uint64_t out_num = pps[zf::kPpsOutFileNumOff];
        uint64_t out_entries = 0;
        for (uint64_t k = 0; k < out_num && k < zf::kMaxOutputFileNum; ++k)
            out_entries += pps[k * (uint64_t)zf::kPpsSingleSize + (uint64_t)zf::kPpsEntriesOff];
        bool ok = (out_entries == total_entries);
        if (!ok) ++nbad;

        double wall_s = std::chrono::duration_cast<fsec>(Clock::now() - twall0).count();
        agg_entries += total_entries;
        agg_in_bytes += total_bytes;
        agg_kernel_s += kernel_s;
        agg_wall_s += wall_s;

        printf("%-6d %10" PRIu64 " %12" PRIu64 " %10.1f %10.2f %10.1f %6" PRIu64 " %9" PRIu64 " %s\n",
               li, total_entries, total_bytes,
               gen_s * 1e3,
               kernel_s * 1e3,
               wall_s * 1e3,
               out_num, out_entries, ok ? "OK" : "MISMATCH");

        if (!ok) {
            fprintf(stderr, "launch %d: out_entries %" PRIu64 " != in %" PRIu64 "\n", li, out_entries, total_entries);
        }
        fflush(stdout);
    }

    // ---- aggregate --------------------------------------------------------
    printf("\nAGGREGATE  entries=%" PRIu64 " in_bytes=%" PRIu64 " gen_s=%.3f io_s=%.3f kernel_s=%.3f wall_s=%.3f\n",
           agg_entries, agg_in_bytes, agg_gen_s, agg_io_s, agg_kernel_s, agg_wall_s);
    if (agg_kernel_s > 0) {
        printf("  kernel throughput: %.1f ops/s   %.2f MB/s  (%.3f s total kernel)\n",
               (double)agg_entries / agg_kernel_s,
               (double)agg_in_bytes / agg_kernel_s / 1e6,
               agg_kernel_s);
    }
    if (agg_wall_s > 0) {
        printf("  end-to-end       : %.1f ops/s   %.2f MB/s  (%.3f s total wall incl host gen/io)\n",
               (double)agg_entries / agg_wall_s,
               (double)agg_in_bytes / agg_wall_s / 1e6,
               agg_wall_s);
    }
    printf(nbad ? "RESULT: FAIL (%d launch mismatches)\n" : "RESULT: PASS (all launches out_entries==in)\n", nbad);
    return nbad ? 1 : 0;
}
