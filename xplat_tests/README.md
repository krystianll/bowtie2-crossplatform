# Launcher parity tests

Checks that the native launchers in `bowtie2_launcher.c` (`bowtie2`,
`bowtie2-build`, `bowtie2-inspect`) behave like the upstream wrapper scripts
(Perl `bowtie2`, Python `bowtie2-build` / `bowtie2-inspect`). Each test case
runs one command line through both and compares the exit code, stdout, stderr
and every file written. Paths are replaced by tags (`<BIN>`, `<TD>`, `<CWD>`) and
clock times by `<TIME>`; gzip/bzip2/zstd/lz4 outputs are compared by compression
type plus decompressed content, and BAM output as the text `samtools view -h`
prints.

- `gen_td.py`: writes deterministic synthetic test data (two random
  chromosomes; single-end reads of lengths 100 and 75, some with a name
  comment, plus random reads, also as `.gz`, `.bz2`, `.zst`, `.lz4`; paired
  reads where both, one or no mate aligns, also as `.lz4`; FASTA, interleaved,
  TAB5, TAB6 and unaligned BAM versions).
- `parity.py`: the comparison harness. The case list (`CASES`) covers plain,
  gzip, bzip2, zstd and lz4 input, mixed and multi-value read lists, stdin,
  comma lists, `-c`, FASTA, `--interleaved`, `--tab5`, `--tab6`, `-b` BAM
  input (unpaired and `--align-paired-reads`), attached and `--1=` forms, local
  mode, `--un`/`--al`/`--un-conc`/`--al-conc`/`--un-mates` (plain, `-gz`,
  `-bz2`, `-lz4`, `-zst`; `%`, extension, no-extension and directory targets),
  `--no-unal`, `--bam` (to a file and to stdout), small/large indexes and
  `BOWTIE2_INDEXES`, the error paths (missing binaries, index, read file,
  output directory, samtools), `--verbose`, `--debug`/`--sanitized`,
  `--log-file`, `--temp-directory`/`--keep`, `--` and other option-parsing
  details of the script, paths with spaces and non-ASCII characters, index
  building (reference size from `.gz`, the `.zst` warning, argparse errors and
  abbreviations) and inspection.
- `run_parity.sh`: sets everything up and runs the comparison on Linux/macOS.
- `run_extra.sh`: checks the scripts cannot be compared on, run after
  `run_parity.sh` (re-uses its directory): a debug build (`bowtie2 --debug`
  with `bowtie2-align-s-debug`, which has assertions on) must align every input
  format and compression exactly like the release binary; a failing write (a
  file-size limit, as on a full disk) of each output the launcher writes itself
  must give an `(ERR)` message and a nonzero exit code; and the temporary files
  for `.lz4` reads must be removed, or kept with `--keep`.

## Linux / macOS: launchers vs the scripts

Needs perl, python3, samtools, gzip, bzip2, zstd, lz4, a C compiler and the
zlib, libbz2, libzstd and liblz4 development files. From the tree root (on
macOS with Homebrew; on Linux leave out `CPPFLAGS`/`LDFLAGS`):

```bash
export CPPFLAGS="-I$(brew --prefix)/include" LDFLAGS="-L$(brew --prefix)/lib"
make bowtie2-align-s bowtie2-align-l bowtie2-build-s bowtie2-build-l \
     bowtie2-inspect-s bowtie2-inspect-l WITH_ZSTD=1 WITH_BZIP2=1
xplat_tests/run_parity.sh "$WORK"          # optional 2nd argument: case-name filter
make bowtie2-align-s-debug WITH_ZSTD=1 WITH_BZIP2=1   # then the extra checks:
xplat_tests/run_extra.sh "$WORK"
```

`WORK` is any scratch directory outside the tree (`run_parity.sh` makes a
temporary one when it is left out; `make launchers-test LAUNCHERS_TEST_DIR=...`
runs the same). The script copies the binaries next to the upstream scripts
(`ref/`) and next to freshly compiled launchers (`launch/`), generates the data
and indexes (`td/`), and runs `parity.py`.

Each case prints `SAME`, `EXPECTED-DIFF` (a known, deliberate difference listed
in `parity.py` with its reason) or `DIFF` with a diff. The exit status is 1 if
any case differs unexpectedly. The expected differences are the ones described
in the main `README.md`. For the cases in `EXPECTED_PG` (`.bz2`/`.lz4` input)
the comparison is repeated without the `@PG` line, and everything else must be
identical.

## Windows: launcher vs the macOS/Linux launcher

The scripts do not run on Windows, so compare the Windows launcher's results
with those of the launcher on macOS/Linux (which the step above checks against
the scripts):

1. On macOS/Linux, after `run_parity.sh` (re-uses its `launch/` and `td/`):

   ```bash
   python3 xplat_tests/parity.py dump "$WORK/launch" "$WORK/td" "$WORK/dump" mac.json
   ```

2. On Windows, build the launchers and binaries (`mingw32-make
   bowtie2.exe bowtie2-build.exe bowtie2-inspect.exe`, and `bowtie2-align-s`
   etc. one target per call, see the main `README.md`), put them in one folder
   (with `samtools.exe` if available, for the `--bam` cases), copy `td/` and
   rebuild its indexes there (`bowtie2-build-s -q ref.fa idx`,
   `bowtie2-build-l -q ref.fa lidx`), then:

   ```bash
   python xplat_tests/parity.py dump BIN_DIR TD_DIR WORK_DIR win.json
   ```

   For the non-ASCII path case, embed a UTF-8 code-page manifest
   (`<activeCodePage>UTF-8</activeCodePage>`) in each `.exe`.

3. Compare: `python3 xplat_tests/parity.py cmp mac.json win.json`. Known
   differences (`EXPECTED_XPLAT`) come from how the binaries are built (no
   `--mm` in the Windows usage text, `--version` build details), from Windows
   refusing to start a missing binary, and from samtools on the Windows machine.
