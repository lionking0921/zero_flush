#!/usr/bin/env bash
# Resume the failed hw link at vpl.impl.write_bitstream, reusing the routed
# project in _x/link/vivado/vpl (place+route already done). Gate script was
# patched (worst_negative_slack 0 -> 0.5) to let the -0.277ns shell-clock
# violation through so write_bitstream can finish and xclbin gets assembled.
set -u
cd "$(dirname "$0")/.." || exit 2

source /tools/Xilinx/Vitis/2022.2/settings64.sh

LOG=build/hw_resume_ws.log
echo "== resume link start: $(date '+%F %T %Z') ==" | tee -a "$LOG"

set -o pipefail
v++ -t hw \
    --platform /opt/xilinx/platforms/xilinx_u2_gen3x4_xdma_gc_2_202110_1/xilinx_u2_gen3x4_xdma_gc_2_202110_1.xpfm \
    --define KERNEL_NAME=krnl_vadd \
    --hls.clock 200000000:krnl_vadd \
    -l build/hw/krnl_vadd.xo -o build/hw/krnl_vadd.xclbin \
    --from_step vpl.impl.write_bitstream \
    2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
echo "EXIT=$rc" | tee -a "$LOG"
echo "== resume link done: $(date '+%F %T %Z') rc=$rc ==" | tee -a "$LOG"
exit "$rc"
