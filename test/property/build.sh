#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "${0%/*}/../.." && pwd)"
OUT="$ROOT/test/property/bin"
mkdir -p "$OUT"

SAN_FLAGS=(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
COMMON_FLAGS=(-std=c++17 -pthread -I"$ROOT")

if [[ ! -f "$ROOT/bin/asan/libopenmpt.a" ]]; then
	mkdir -p "$ROOT/bin/asan"
	make -C "$ROOT" -j"$(nproc)" \
		CONFIG=gcc \
		FLAVOUR_DIR=asan/ \
		CHECKED_ADDRESS=1 \
		CHECKED_UNDEFINED=1 \
		SHARED_LIB=0 \
		STATIC_LIB=1 \
		TEST=0 \
		EXAMPLES=0 \
		OPENMPT123=0 \
		NO_MPG123=1 \
		NO_VORBIS=1 \
		NO_VORBISFILE=1 \
		NO_OGG=1 \
		NO_FLAC=1 \
		NO_SNDFILE=1 \
		NO_PORTAUDIO=1 \
		NO_PORTAUDIOCPP=1 \
		NO_PULSEAUDIO=1 \
		NO_SDL2=1 \
		bin/asan/libopenmpt.a
fi

g++ "${COMMON_FLAGS[@]}" "${SAN_FLAGS[@]}" \
	"$ROOT/test/property/module_property_runner.cpp" \
	"$ROOT/bin/asan/libopenmpt.a" \
	-lz -ldl -lm \
	-o "$OUT/module_property_runner_asan"

REFERENCE_LIB="${REFERENCE_LIB:-/usr/lib/x86_64-linux-gnu/libopenmpt.so.0}"
if [[ ! -e "$REFERENCE_LIB" ]]; then
	echo "reference libopenmpt shared library not found: $REFERENCE_LIB" >&2
	exit 1
fi

g++ "${COMMON_FLAGS[@]}" -O2 \
	"$ROOT/test/property/module_property_runner.cpp" \
	"$REFERENCE_LIB" \
	-Wl,-rpath,/usr/lib/x86_64-linux-gnu \
	-o "$OUT/module_property_runner_ref"

echo "built:"
echo "  $OUT/module_property_runner_asan"
echo "  $OUT/module_property_runner_ref"
