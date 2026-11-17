#!/bin/sh
#
# Compare two build_mixer binaries on three deterministic XM tracks.
# Usage: test/run_bench_mixer.sh BASELINE_BINARY OPTIMIZED_BINARY [OUTPUT_DIR]
#

set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	echo "Usage: $0 BASELINE_BINARY OPTIMIZED_BINARY [OUTPUT_DIR]" >&2
	exit 1
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASELINE=$1
OPTIMIZED=$2
OUTPUT=${3:-/tmp/openmpt-mixer-bench}

mkdir -p "$OUTPUT"
python3 "$ROOT/test/gen_bench_mixer_modules.py" "$OUTPUT"

for tier in low mid high; do
	module="$OUTPUT/bench_$tier.xm"
	base_pcm="$OUTPUT/$tier.base.pcm"
	opt_pcm="$OUTPUT/$tier.opt.pcm"
	echo "== $tier =="
	"$BASELINE" "$module" 10 15 "$base_pcm" 8
	"$OPTIMIZED" "$module" 10 15 "$opt_pcm" 8
	cmp "$base_pcm" "$opt_pcm"
	sha256sum "$base_pcm" "$opt_pcm"
done
