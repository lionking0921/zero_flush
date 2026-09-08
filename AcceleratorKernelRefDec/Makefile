# =====================================================================
# Vitis 2022.2 build for the zero_flush zf-decoder compaction kernel
# Target: xilinx_u2_gen3x4_xdma_gc_2_202110_1 (SmartSSD U2)
#
# Usage:
#   make TARGET=sw_emu xclbin     # software emulation .xclbin (correctness)
#   make TARGET=hw     xclbin     # real bitstream .xclbin (hours)
#   make TARGET=sw_emu host       # build host test binary
#   make TARGET=sw_emu run        # run host test under emulation
#   make TARGET=hw     run        # run host test on the real card
# =====================================================================

SHELL := /bin/bash

PLATFORM := /opt/xilinx/platforms/xilinx_u2_gen3x4_xdma_gc_2_202110_1/xilinx_u2_gen3x4_xdma_gc_2_202110_1.xpfm
TARGET   ?= sw_emu
KERNEL   := krnl_vadd
VITIS_SETTINGS := /tools/Xilinx/Vitis/2022.2/settings64.sh
XRT_SETUP     := /opt/xilinx/xrt/setup.sh
XRT_INCLUDE   := /opt/xilinx/xrt/include
XRT_LIB       := /opt/xilinx/xrt/lib

KSRC  := kernel/krnl_vadd.cpp
KINC  := kernel
HOST_SRCS := host/main_zf.cpp
BUILD := build/$(TARGET)
XO    := $(BUILD)/$(KERNEL).xo
XCLBIN := $(BUILD)/$(KERNEL).xclbin
HOSTBIN := host/test_host_$(TARGET)
LOG   := $(BUILD)/$(KERNEL).log

VPP   := v++

# common kernel-compile/link flags (recipe recovered from the prebuilt
# reference xclbin command line)
KFLAGS := -t $(TARGET) --platform $(PLATFORM) --define KERNEL_NAME=$(KERNEL) --hls.clock 200000000:$(KERNEL)

# link-stage kernel clock override (MHz), hw only.
# 2026-09-08 (M1, CRC removed): user decision -- kernel clock LOCKED to 300 MHz.
# --kernel_frequency 300 at -l is the setting that fixes the final kernel clock
# that P&R must close and XRT programs on card.  --hls.clock above (200 MHz) only
# constrains HLS synthesis scheduling, NOT the -l link clock; the HLS-200 RTL has
# enough slack that P&R closes at 300 MHz (proven recipe: reference a69cf31 kernel
# validated on card at 300 MHz with the same HLS-200 .xo).  If timing closure or
# route congestion fails at 300, fall back to 200 MHz:
#     make TARGET=hw KFREQ_MHZ=200 xclbin
KFREQ_MHZ ?= 300
LFREQ := $(if $(KFREQ_MHZ),--kernel_frequency $(KFREQ_MHZ),)

.PHONY: all xclbin host run clean

all: xclbin host

xclbin: $(XCLBIN)

$(XO): $(KSRC) $(KINC)/krnl_host.h | $(BUILD)
	source $(VITIS_SETTINGS); set -o pipefail; \
	v++ $(KFLAGS) -c -k $(KERNEL) -I $(KINC) $(KSRC) -o $@ 2>&1 | tee $(LOG)
	@echo "== $(XO) built =="

$(XCLBIN): $(XO)
	source $(VITIS_SETTINGS); set -o pipefail; \
	v++ $(KFLAGS) $(LFREQ) -l $(XO) -o $@ 2>&1 | tee -a $(LOG)
	@echo "== $(XCLBIN) built =="

host: $(HOSTBIN)

$(HOSTBIN): $(HOST_SRCS) host/zf_format.h host/zf_sst_decode.h kernel/krnl_host.h
	source $(XRT_SETUP); \
	mkdir -p host; \
	g++ -std=c++17 -O2 -g -DCL_HPP_TARGET_OPENCL_VERSION=120 -DCL_HPP_MINIMUM_OPENCL_VERSION=120 \
	    -DCL_TARGET_OPENCL_VERSION=120 \
	    -I host -I kernel -I $(XRT_INCLUDE) -L $(XRT_LIB) -Wl,-rpath,$(XRT_LIB) \
	    $(HOST_SRCS) -o $@ -lxilinxopencl -lpthread
	@echo "== $(HOSTBIN) built =="

run: $(XCLBIN) $(HOSTBIN)
	source $(XRT_SETUP); \
	export XCL_EMULATION_MODE=$(if $(filter hw,$(TARGET)),,sw_emu); \
	./$(HOSTBIN) -x $(XCLBIN) -d 0

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf build

.PHONY: force
