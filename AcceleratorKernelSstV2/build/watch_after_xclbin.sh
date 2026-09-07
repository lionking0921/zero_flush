#!/usr/bin/env bash
# Wait for `make TARGET=hw xclbin` (PID given as $1) to finish with EXIT=0,
# then run `make TARGET=hw run` (hw validation on the real card).
# All output appended to build/hw_after_xclbin_auto_run.log
set -u
BUILD_PID="$1"
cd "$(dirname "$0")/.." || exit 2

stamp() { date '+%F %T %Z'; }

echo "== auto watcher (re)started: $(stamp) =="
echo "watching PID=${BUILD_PID}, poll for EXIT= in build/hw_synth_j5_300_postloop.log"

while :; do
    # EXIT= marker written by the wrapper that launched the make
    if grep -q '^EXIT=' build/hw_synth_j5_300_postloop.log 2>/dev/null; then
        break
    fi
    if ! kill -0 "$BUILD_PID" 2>/dev/null; then
        # process gone without EXIT marker (e.g. was killed by someone)
        # but maybe it finished and the marker hasn't flushed yet; double-check log
        sleep 5
        if ! grep -q '^EXIT=' build/hw_synth_j5_300_postloop.log 2>/dev/null; then
            echo "$(stamp) build PID ${BUILD_PID} no longer alive and no EXIT= marker -> abort watch"
            exit 3
        fi
        break
    fi
    sleep 60
done

rc="missing"
while IFS= read -r line; do
    case "$line" in
        EXIT=*) rc="${line#EXIT=}" ;;
    esac
done < build/hw_synth_j5_300_postloop.log

XCLBIN="build/hw/krnl_vadd.xclbin"
echo "$(stamp) observed build rc=${rc}"
if [ "$rc" = "0" ] && [ -s "$XCLBIN" ]; then
    sz=$(stat -c %s "$XCLBIN")
    echo "$(stamp) xclbin ready (${sz} bytes): $XCLBIN"
    echo "$(stamp) starting next step: make TARGET=hw run"
    make TARGET=hw run
    run_rc=$?
    echo "$(stamp) NEXT_STEP_EXIT=${run_rc}"
    exit "$run_rc"
else
    echo "$(stamp) SKIP next step: rc=${rc}, xclbin exists=$([ -s "$XCLBIN" ] && echo yes || echo no)"
    exit 1
fi
