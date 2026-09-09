#!/bin/bash
# =====================================================================
# hw_build_auto.sh — AcceleratorKernelRefDec (decoder burst) HW xclbin 自动构建
#
# 策略（用户 2026-09-09 指令，全程自动不需确认）：
#   Stage A  : hw HLS -c 产 .xo（200MHz HLS 调度；HLS 合法性门禁，失败=代码问题，降频无用）
#   Stage B  : link @300 MHz（默认）。产出即成功。
#   Stage C  : B 布线失败 → 清 _x link scratch，降 200 MHz 重布。
#   Stage D  : C 已布通但 write_bitstream 因时序中止 → 仿 M1 续跑
#              --from_step vpl.impl.write_bitstream 强制产 bitstream（时序负仍上真卡验证，
#              依据既有 hw-timing-negative-build-and-validate 策略）。
#
# 各阶段日志 /tmp/refdec_hw_<stage>.log；成功后在 build/hw/ 归档为
# krnl_vadd.DEC_<达成MHz>_<ts>.xclbin 并汇报达成时钟。
# 退出码：0 成功；90 HLS .xo 失败；1 全阶段失败。
# =====================================================================
set -u
REPO=/home/embedded-415/wjd/ZeroFlush/zero_flush/AcceleratorKernelRefDec
VITIS=/tools/Xilinx/Vitis/2022.2/settings64.sh
PLATFORM=/opt/xilinx/platforms/xilinx_u2_gen3x4_xdma_gc_2_202110_1/xilinx_u2_gen3x4_xdma_gc_2_202110_1.xpfm
cd "$REPO" || exit 99
source "$VITIS" >/dev/null 2>&1

achieved_mhz() {
  # 从 .info 的 Scalable Clocks 取首个 "Frequency: N MHz"
  awk '/Frequency:/{for(i=1;i<=NF;i++) if($i=="Frequency:"){print $(i+1); exit}}' \
      build/hw/krnl_vadd.xclbin.info 2>/dev/null | tr -d ' '
}
archive() {  # $1 = 阶段名
  local mhz stage=$1 ts
  ts=$(date +%m%d_%H%M)
  mhz=$(achieved_mhz); [ -n "$mhz" ] || mhz="clockNA"
  cp -f build/hw/krnl_vadd.xclbin "build/hw/krnl_vadd.DEC_${mhz}_${ts}.xclbin"
  cp -f build/hw/krnl_vadd.xclbin.info "build/hw/krnl_vadd.DEC_${mhz}_${ts}.xclbin.info"
  echo "[driver] === ${stage} 成功：xclbin 产出，达成 kernel 时钟 = ${mhz} MHz ==="
  echo "[driver] 归档: build/hw/krnl_vadd.DEC_${mhz}_${ts}.xclbin"
  date
}

echo "[driver] start $(date) free=$(df -BG /home/embedded-415/wjd | awk 'NR==2{print $4}')"

XO=build/hw/krnl_vadd.xo
XCL=build/hw/krnl_vadd.xclbin

echo "[driver] Stage A: hw HLS .xo (200MHz HLS schedule)"
rm -rf _x
make TARGET=hw "$XO" > /tmp/refdec_hw_A.log 2>&1
if [ ! -f "$XO" ]; then
  echo "[driver] Stage A FAIL: HLS -c 未产出 .xo（代码/HLS 合法性问题，降频无用）"
  grep -nE "ERROR|error:|FATAL" /tmp/refdec_hw_A.log | tail -20
  exit 90
fi
echo "[driver] Stage A OK: $XO ($(stat -c%s "$XO") B)"

echo "[driver] Stage B: link @300 MHz (默认)"
make TARGET=hw "$XCL" > /tmp/refdec_hw_B.log 2>&1
if [ -f "$XCL" ]; then archive "Stage B @300MHz"; exit 0; fi
echo "[driver] Stage B FAILED @300："
grep -nE "congestion|not routable|route_design ERROR|did not meet timing|write_bitstream ERROR" /tmp/refdec_hw_B.log | tail -4
grep -nE "ERROR: \[VPL (35-3|101-2|60-704)" /tmp/refdec_hw_B.log | tail -4

echo "[driver] Stage C: link @200 MHz (布线失败回退)"
rm -rf _x
make TARGET=hw KFREQ_MHZ=200 "$XCL" > /tmp/refdec_hw_C.log 2>&1
if [ -f build/hw/krnl_vadd.xclbin ]; then archive "Stage C @200MHz"; exit 0; fi
echo "[driver] Stage C FAILED @200："
grep -nE "congestion|not routable|route_design ERROR|did not meet timing|write_bitstream ERROR" /tmp/refdec_hw_C.log | tail -4

echo "[driver] Stage D: 若 200 已布通仅 write_bitstream 时序中止 → --from_step 续跑产 bitstream"
if [ -d _x/link ] && grep -qiE "Verifying routed nets|Finished .*tasks.*FPGA routing|route_design.*completed" /tmp/refdec_hw_C.log; then
  v++ -t hw --platform "$PLATFORM" --define KERNEL_NAME=krnl_vadd \
      --hls.clock 200000000:krnl_vadd \
      --from_step vpl.impl.write_bitstream \
      --input_files build/hw/krnl_vadd.xo \
      --kernel_frequency 200 --link --optimize 0 \
      --output build/hw/krnl_vadd.xclbin > /tmp/refdec_hw_D.log 2>&1
fi
if [ -f build/hw/krnl_vadd.xclbin ]; then archive "Stage D (resume @200MHz)"; exit 0; fi
echo "[driver] ALL STAGES FAILED（300 布线失败、200 亦未产出 bitstream）"
date
exit 1
