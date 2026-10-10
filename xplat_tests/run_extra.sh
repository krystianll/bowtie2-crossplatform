#!/usr/bin/env bash
# Extra launcher checks on Linux/macOS, run after xplat_tests/run_parity.sh
# WORK_DIR (re-uses its launch/ and td/):
#  1. a debug build: `bowtie2 --debug` (bowtie2-align-s-debug, with
#     assertions) must give the same alignments as the release binary for
#     plain, gzip, bzip2, zstd, lz4, stdin, paired, FASTA, interleaved, TAB6
#     and BAM input. Build it first (same flags as the release binaries):
#       make bowtie2-align-s-debug WITH_ZSTD=1 WITH_BZIP2=1
#  2. write errors: with a file-size limit (SIGXFSZ ignored, so writes fail as
#     on a full disk), every output the launcher writes itself (-S with
#     --no-unal, --un plain/-gz/-bz2/-lz4/-zst, --un-conc, --un-mates) must
#     end in an (ERR) message and a nonzero exit code.
#  3. .lz4 reads: the decompressed temporary files go to --temp-directory and
#     are removed afterwards, or kept with --keep.
# usage: xplat_tests/run_extra.sh WORK_DIR
set -u
H=$(cd "$(dirname "$0")/.." && pwd)
W=$(cd "${1:?usage: run_extra.sh WORK_DIR}" && pwd)
L=$W/launch
TD=$W/td
[ -x "$L/bowtie2" ] && [ -f "$TD/idx.1.bt2" ] || { echo "run xplat_tests/run_parity.sh $W first"; exit 2; }
[ -x "$H/bowtie2-align-s-debug" ] || { echo "missing $H/bowtie2-align-s-debug - build it first (make bowtie2-align-s-debug WITH_ZSTD=1 WITH_BZIP2=1)"; exit 2; }
X=$W/extra
rm -rf "$X"
mkdir -p "$X"
failed=0
cp "$H/bowtie2-align-s-debug" "$L/"

echo "-- debug build"
n=0
for c in "-U $TD/se.fq" "-U $TD/se.fq.gz" "-U $TD/se.fq.bz2" "-U $TD/se.fq.zst" "-U $TD/se.fq.lz4" "-U -" \
         "-1 $TD/m1.fq -2 $TD/m2.fq" "-f -U $TD/se.fa" "--interleaved $TD/il.fq" "--tab6 $TD/pe.tab6" \
         "-b $TD/pe.bam --align-paired-reads"; do
	n=$((n + 1))
	for v in rel dbg; do
		d=; [ $v = dbg ] && d=--debug
		# shellcheck disable=SC2086
		"$L/bowtie2" -x "$TD/idx" $d $c -S "$X/$n.$v.sam" < "$TD/se.fq" 2> "$X/$n.$v.err"
		echo $? > "$X/$n.$v.rc"
		grep -v '^@PG' "$X/$n.$v.sam" > "$X/$n.$v.body" 2>/dev/null
	done
	if cmp -s "$X/$n.rel.body" "$X/$n.dbg.body" && cmp -s "$X/$n.rel.rc" "$X/$n.dbg.rc" &&
	   [ "$(cat "$X/$n.dbg.rc")" = 0 ] && [ -s "$X/$n.dbg.body" ]; then
		echo "SAME  debug $c"
	else
		echo "DIFF  debug $c (exit $(cat "$X/$n.rel.rc") vs $(cat "$X/$n.dbg.rc"))"
		tail -3 "$X/$n.dbg.err"
		failed=$((failed + 1))
	fi
done
rm -f "$L/bowtie2-align-s-debug"

echo "-- write errors"
for c in "--no-unal -S out.sam" "--un un.fq -S /dev/null" "--un-gz un.fq.gz -S /dev/null" \
         "--un-bz2 un.fq.bz2 -S /dev/null" "--un-lz4 un.fq.lz4 -S /dev/null" "--un-zst un.fq.zst -S /dev/null" \
         "--un-conc uc.fq -S /dev/null" "--un-mates um.fq -S /dev/null"; do
	case $c in --un-conc*|--un-mates*) r="-1 $TD/m1.fq -2 $TD/m2.fq";; *) r="-U $TD/se.fq";; esac
	# shellcheck disable=SC2086
	out=$(cd "$X" && trap '' XFSZ && ulimit -f 1 && "$L/bowtie2" -x "$TD/idx" $r $c 2>&1)
	rc=$?
	msg=$(printf '%s\n' "$out" | grep '^(ERR): Could not write')
	if [ $rc -ne 0 ] && [ -n "$msg" ]; then
		echo "OK    $c: exit $rc, $msg"
	else
		echo "FAIL  $c: exit $rc, no write error reported"
		failed=$((failed + 1))
	fi
done

echo "-- lz4 temporary files"
for k in "" --keep; do
	T=$X/tmp$k
	mkdir -p "$T"
	# shellcheck disable=SC2086
	"$L/bowtie2" -x "$TD/idx" -U "$TD/se.fq.lz4" --temp-directory "$T" $k -S "$X/lz4.sam" 2> /dev/null
	rc=$?
	left=$(ls "$T")
	if [ $rc -eq 0 ] && { { [ -z "$k" ] && [ -z "$left" ]; } ||
	   { [ -n "$k" ] && [ -n "$left" ] && cmp -s "$T/$left" "$TD/se.fq"; }; }; then
		echo "OK    lz4 input ${k:-without --keep}: temporary files left: ${left:-none}"
	else
		echo "FAIL  lz4 input ${k:-without --keep}: exit $rc, temporary files left: ${left:-none}"
		failed=$((failed + 1))
	fi
done

echo
echo "$failed failure(s)"
[ $failed -eq 0 ]
