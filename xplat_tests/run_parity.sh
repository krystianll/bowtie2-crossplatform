#!/usr/bin/env bash
# Compare the C launchers (bowtie2_launcher.c) with the upstream Perl/Python
# wrapper scripts on Linux/macOS. Build the six binaries first (see README.md):
#   make bowtie2-align-s bowtie2-align-l bowtie2-build-s bowtie2-build-l \
#        bowtie2-inspect-s bowtie2-inspect-l WITH_ZSTD=1 WITH_BZIP2=1
# usage: xplat_tests/run_parity.sh [WORK_DIR] [case-substring]
# Needs perl, python3, samtools, gzip, bzip2, zstd, lz4, a C compiler (CC,
# default cc) and the zlib, libbz2, libzstd and liblz4 headers/libraries
# (CPPFLAGS/LDFLAGS are passed on, e.g. -I/-L for Homebrew).
set -eu
H=$(cd "$(dirname "$0")/.." && pwd)
W=${1:-$(mktemp -d)}
ONLY=${2:-}
BINS="bowtie2-align-s bowtie2-align-l bowtie2-build-s bowtie2-build-l bowtie2-inspect-s bowtie2-inspect-l"
for b in $BINS; do
	[ -x "$H/$b" ] || { echo "missing $H/$b - build it first (make $BINS WITH_ZSTD=1 WITH_BZIP2=1)"; exit 2; }
done
mkdir -p "$W"
W=$(cd "$W" && pwd)
rm -rf "$W/ref" "$W/launch" "$W/td" "$W/work"
mkdir -p "$W/ref" "$W/launch"

# Reference: the upstream scripts next to the binaries.
for b in $BINS; do cp "$H/$b" "$W/ref/"; cp "$H/$b" "$W/launch/"; done
cp "$H/bowtie2" "$H/bowtie2-build" "$H/bowtie2-inspect" "$W/ref/"
chmod +x "$W/ref/"*

for t in ALIGN:bowtie2 BUILD:bowtie2-build INSPECT:bowtie2-inspect; do
	# shellcheck disable=SC2086
	"${CC:-cc}" -O2 -Wall -D_DEFAULT_SOURCE ${CPPFLAGS:-} "-DLAUNCH_${t%%:*}" -o "$W/launch/${t#*:}" \
		"$H/bowtie2_launcher.c" ${LDFLAGS:-} -lz -lbz2 -lzstd -llz4
done

python3 "$H/xplat_tests/gen_td.py" "$W/td"
"$W/launch/bowtie2-build-s" -q "$W/td/ref.fa" "$W/td/idx"
"$W/launch/bowtie2-build-l" -q "$W/td/ref.fa" "$W/td/lidx"
python3 "$H/xplat_tests/parity.py" "$W/ref" "$W/launch" "$W/td" "$W/work" "$ONLY"
