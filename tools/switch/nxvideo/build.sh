#!/usr/bin/env bash
# culdcept-nx (2026-11-20): builds tools/switch/nxvideo/out/nxvideo.o, the NVDEC video decoder of nxvideo.h, for the
# Switch NRO (app/CMakeLists.txt links it when it exists):
#   1. nxvideo.c compiled against devkitPro's switch-ffmpeg 7.1 headers (portlibs: built with --enable-gpl and averne's
#      nvtegra backend), with the port's Switch flags;
#   2. a partial link of it with devkitPro's libavcodec.a and libavutil.a: only the members it reaches (the HEVC, H.264
#      and WMV3 decoders by name, their nvtegra hwaccels, libavutil);
#   3. every defined symbol made local but nxv_*: FFmpeg 7.1's names cannot meet the port's own FFmpeg 4.4 (the XMA
#      audio, the software WMV3 path). What stays undefined (newlib, libnx's nv / mmu / fence services) the NRO's link
#      resolves, from the same libnx the port links.
# usage (Git Bash): tools/switch/nxvideo/build.sh
set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${OUT:-$HERE/out}
DKP=${DEVKITPRO_POSIX:-/c/devkitPro}
BIN=$DKP/devkitA64/bin
PL=$DKP/portlibs/switch
mkdir -p "$OUT"

FLAGS="-O2 -ffunction-sections -fdata-sections -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft
       -ftls-model=local-exec -fPIE -D__SWITCH__ -DNX -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter"
"$BIN/aarch64-none-elf-gcc" $FLAGS -I"$PL/include" -I"$DKP/libnx/include" -c "$HERE/nxvideo.c" -o "$OUT/shim.o"

"$BIN/aarch64-none-elf-ld" -r -o "$OUT/nxvideo_full.o" "$OUT/shim.o" \
  --start-group "$PL/lib/libavcodec.a" "$PL/lib/libavutil.a" --end-group

API="nxv_open nxv_decode nxv_flush nxv_close nxv_is_hw nxv_version nxv_stats nxv_options"
KEEP=()
for s in $API; do KEEP+=(--keep-global-symbol="$s"); done
"$BIN/aarch64-none-elf-objcopy" "${KEEP[@]}" "$OUT/nxvideo_full.o" "$OUT/nxvideo.o"
rm -f "$OUT/nxvideo_full.o"

# Checks: the API is there, nothing else defined is global, and what it needs from outside.
DEFINED=$("$BIN/aarch64-none-elf-nm" -g --defined-only "$OUT/nxvideo.o" | awk '{print $3}' | sort)
echo "global symbols: $(echo $DEFINED)"
for s in $DEFINED; do case " $API " in *" $s "*) ;; *) echo "unexpected global symbol: $s" >&2; exit 1;; esac; done
echo "undefined (from newlib / libnx at the NRO's link): $("$BIN/aarch64-none-elf-nm" -u "$OUT/nxvideo.o" | wc -l)"
"$BIN/aarch64-none-elf-nm" -u "$OUT/nxvideo.o" | awk '{print $2}' | grep -E '^(nv|mmu)' | tr '\n' ' '; echo
ls -la "$OUT/nxvideo.o"
