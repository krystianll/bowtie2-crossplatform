# Bowtie 2 — patched, cross-platform build (work in progress)

A minimal patched copy of [**Bowtie 2**](https://github.com/BenLangmead/bowtie2)
(upstream `master` at `58e34bf`, version 2.5.5) that builds and runs natively on
**Linux, macOS (Intel + Apple Silicon), and Windows (x64 + ARM64)**, and reads
**gzip-, bzip2- and zstd-compressed reads directly** inside the aligner.

For Bowtie 2's documentation, options, and canonical source, see the upstream
repository: <https://github.com/BenLangmead/bowtie2> and the
[manual](https://bowtie-bio.sourceforge.net/bowtie2/manual.shtml). **This README
covers only how to build this tree and what was changed** — everything else is
unchanged from upstream.

Upstream publishes an x64 Windows build (`bowtie2-<version>-mingw-x86_64.zip`,
cross-compiled from Linux) whose `.bat` wrappers need Perl and Python. This tree
adds a native build on Windows itself, for x64 **and ARM64**, LF-only output,
compressed input without external tools, and native `bowtie2`, `bowtie2-build`
and `bowtie2-inspect` launchers that replace the Perl/Python wrapper scripts on
Windows.

---

## Building from source

Run every command **from this directory** (the one containing `Makefile`). The
SIMD code on ARM needs the `third_party/simde` submodule:

```bash
git submodule update --init third_party/simde
```

Two optional inputs are switched on at build time, as upstream does for zstd:

| Make variable | Adds | Links |
|---|---|---|
| `WITH_ZSTD=1` | `.zst` read input (upstream feature) | `-lzstd` |
| `WITH_BZIP2=1` | `.bz2` read input (this fork, `bzip2_reader.cpp`) | `-lbz2` |

gzip input is always available (zlib). Without these flags the build is
upstream's, apart from the Windows fixes below.

### Linux

```bash
sudo apt install build-essential zlib1g-dev libbz2-dev libzstd-dev   # Debian/Ubuntu
make -j8 WITH_ZSTD=1 WITH_BZIP2=1
```

### macOS (Apple clang, Intel or Apple Silicon)

```bash
brew install zstd          # bzip2 and zlib come with the SDK
CPPFLAGS="-I$(brew --prefix)/include" LDFLAGS="-L$(brew --prefix)/lib" \
  make -j8 WITH_ZSTD=1 WITH_BZIP2=1
```

Pass `CPPFLAGS`/`LDFLAGS` in the environment as shown, not on the `make` command
line: a command-line value replaces the Makefile's own `-Ithird_party`.

### Windows x64 (MSYS2 **UCRT64** shell) and ARM64 (MSYS2 **CLANGARM64** shell)

The build is native MinGW in the matching MSYS2 shell — not MSVC, not the
Cygwin-style "MSYS2 MSYS" shell. The Makefile detects a native Windows build
from `$(OS)` and takes the target architecture from the compiler.

```bash
# UCRT64
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make \
  mingw-w64-ucrt-x86_64-zlib mingw-w64-ucrt-x86_64-bzip2 mingw-w64-ucrt-x86_64-zstd
CC=gcc CXX=g++
# CLANGARM64
pacman -S --needed mingw-w64-clang-aarch64-clang mingw-w64-clang-aarch64-make \
  mingw-w64-clang-aarch64-zlib mingw-w64-clang-aarch64-bzip2 mingw-w64-clang-aarch64-zstd
CC=clang CXX=clang++

for t in bowtie2-align-s bowtie2-align-l bowtie2-build-s bowtie2-build-l \
         bowtie2-inspect-s bowtie2-inspect-l; do
  mingw32-make -j4 $t CC=$CC CXX=$CXX WITH_ZSTD=1 WITH_BZIP2=1
done
```

The executables are linked statically by default (upstream's Windows setting).
`WINDOWS_STATIC=0` links the toolchain and compression libraries as DLLs instead,
for an installer that ships them once for several tools. `BUILD_HOST=<name>`
replaces the build machine's name in `--version` output for distributed builds.

On Windows also build the launchers (they replace upstream's `.bat` files, which
need Perl and Python; the align launcher links zlib, libbz2, libzstd and liblz4):

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-lz4      # CLANGARM64: mingw-w64-clang-aarch64-lz4
mingw32-make bowtie2.exe bowtie2-build.exe bowtie2-inspect.exe CC=$CC CXX=$CXX
```

### Verify (any platform)

```bash
./bowtie2-build-s example/reference/lambda_virus.fa lambda
./bowtie2-align-s -x lambda -U example/reads/reads_1.fq -S out.sam   # also .gz/.bz2/.zst
```

### The `bowtie2`, `bowtie2-build` and `bowtie2-inspect` launchers

Upstream runs the aligner and index tools through wrapper scripts: `bowtie2` is
**Perl**, `bowtie2-build` and `bowtie2-inspect` are **Python**. On Windows a
script is not a runnable command (cmd, Explorer and other programs only start
`.exe` files), so a native Windows build makes launchers instead:
`bowtie2.exe`, `bowtie2-build.exe` and `bowtie2-inspect.exe`, all from
[`bowtie2_launcher.c`](bowtie2_launcher.c). They need no Perl, Python, `gzip`,
`lz4` or other helper programs, and are used exactly like the scripts:

```bat
bowtie2 -x <index> -1 r1.fq.gz -2 r2.fq.gz -S out.sam
bowtie2 -x <index> -U reads.fq.lz4 --un-gz unaligned.fq.gz --al-conc-zst al_%.fq.zst -S out.sam
bowtie2 -x <index> -b reads.bam --align-paired-reads --bam -S out.bam
bowtie2-build ref.fa <index>
bowtie2-inspect -s <index>
```

Like the scripts, each launcher picks the small- or large-index binary (`-s`/`-l`,
by the index files present or `--large-index`; for `bowtie2-build` by the
reference size), runs the `-debug` or `-sanitized` variant with `--debug` /
`--sanitized`, adds `--wrapper basic-0`, and runs the binary from its own
directory. `bowtie2` also does what the Perl wrapper does on top: it takes the
same wrapper options in the same way (`-1`/`-2`/`-U`/`--tab5`/`--tab6`/
`--interleaved`/`-b` with one or more files, `--`, `--temp-directory`,
`--keep`, `--verbose`, `--log-file`, the index check with `BOWTIE2_INDEXES`),
writes `--un` / `--al` / `--un-conc` / `--al-conc` / `--un-mates` (plain or
`-gz`, `-bz2`, `-lz4`, `-zst`; file, directory or `%` targets) and handles
`--no-unal` through the binary's `--passthrough` mode, pipes the SAM through
`samtools view -b -` for `--bam` (`samtools` next to the launcher, else on
`PATH`), and prints the same `(INFO)` / `(ERR)` messages with the same exit
codes. Gzip, bzip2 and zstd reads go to the binary as they are (it reads them
natively); `.lz4` reads are decompressed with liblz4 into temporary files in the
temporary directory, deleted afterwards unless `--keep` is given.

The launchers were compared with the scripts on 101 command lines
([`xplat_tests/`](xplat_tests/README.md); `make launchers-test` or
`xplat_tests/run_parity.sh` reruns it), covering those options, every input
format and compression, local mode, BAM in and out, small and large indexes,
paths with spaces and non-ASCII characters, the error paths, index building and
inspection. Output files, stdout, stderr and exit codes are identical, except
for these deliberate differences:

- `.bz2` and `.lz4` read files: the Perl wrapper feeds them to the binary
  through a named pipe (which Windows does not have); the launcher passes `.bz2`
  files as they are and `.lz4` files as decompressed temporary files. Only the
  command line recorded in the SAM `@PG` header differs. As in the script, these
  files are read after the other files of the same option. `--no-named-pipes` is
  accepted and changes nothing.
- Several `.bz2`/`.lz4` files for one option (e.g. `-U a.fq.lz4 b.fq.bz2`): the
  launcher reads them all. The Perl wrapper only feeds the first one and drops
  the others without a message.
- A missing or damaged `.lz4` read file stops the launcher with
  `(ERR): Could not open lz4ed read file` / `Could not decompress lz4ed read
  file`. The Perl wrapper passes on whatever `lz4` could decompress.
- Arguments reach the binary unchanged. The Perl wrapper runs the binary through
  a shell command line and quotes only the values of some options (`-x`, `-S`,
  `-U`, `-1`, `-2`, `-b`, `--rg`, ...), so other values with spaces or shell
  characters, and compressed `--un-gz`-style targets with spaces, break there.
- `-S<file>` and `--output=<file>` are honoured together with `--un*`, `--al*`,
  `--no-unal` and `--bam`; the Perl wrapper passes them on to the binary, which
  then writes the raw passthrough output into that file.
- When a binary cannot be started (e.g. `--debug` without a `-debug` build), the
  error comes from the launcher rather than from `sh` (or a Python traceback for
  `bowtie2-inspect`).
- `--ref-string` / `--reference-string` (index from a sequence on the command
  line) is not supported: the launcher stops with an `(ERR)` message. The Perl
  wrapper builds a temporary index with `bowtie2-build` from `PATH`.
- A failed write or close of an output the launcher writes itself (`-S` with
  `--un*`/`--al*`/`--no-unal`, and those read files) stops it with
  `(ERR): Could not write ...` and a nonzero exit code (the system error number,
  as for its other errors). The Perl wrapper ignores such errors, so a full disk
  gives truncated files and exit code 0.
- With `--verbose` together with `--un*`/`--al*`/`--no-unal`/`--bam`, the
  binary's verbose lines end up in the SAM stream (upstream behaviour, kept); the
  Perl wrapper also prints a Perl warning for each of them, the launcher does not.
- The Perl wrapper first runs `bowtie2-align-s --wrapper basic-0 --arg-desc` and
  ignores the result; the launcher skips that extra run.
- On Windows, if the aligner crashes, the launcher reports the Windows exception
  code (`(ERR): bowtie2-align died with exception 0xC0000005 (access
  violation)`) and exits with it. If the launcher itself is killed (e.g. by a
  GUI's Cancel, `TerminateProcess`), the binary it started is killed too (job
  object). On Linux/macOS signals are reported as by the script.

On Linux and macOS the upstream scripts are used as before. The launchers build
there too, for testing (outside the source tree, so the scripts are not
overwritten):

```bash
OUT=../launchers; mkdir -p $OUT   # any directory outside the tree
P=$(brew --prefix)                # macOS; on Linux leave out the -I/-L flags
cc -O2 -I$P/include -DLAUNCH_ALIGN   -o $OUT/bowtie2         bowtie2_launcher.c -L$P/lib -lzstd -llz4 -lbz2 -lz
cc -O2              -DLAUNCH_BUILD   -o $OUT/bowtie2-build   bowtie2_launcher.c -lz
cc -O2              -DLAUNCH_INSPECT -o $OUT/bowtie2-inspect bowtie2_launcher.c
```

(then copy the `bowtie2-*-s`/`-l` binaries next to them).
**Non-ASCII paths** on Windows (outside the ANSI code page) need a UTF-8
application manifest (`<activeCodePage>UTF-8</activeCodePage>`) embedded in each
`.exe`, as the binaries use the narrow-character file APIs.

---

## Patches (vs upstream `58e34bf`)

Every edit to an upstream file carries a `bowtie2-crossplatform:` comment, so

```bash
grep -rn "bowtie2-crossplatform:" --include='*.cpp' --include='*.c' --include='*.h' --include=Makefile .
```

lists them. Windows-only changes are `#ifdef _WIN32`-guarded; bzip2 support is
`#ifdef WITH_BZIP2`-guarded and off unless requested.

| File | Change | Reason |
|------|--------|--------|
| `bowtie_main.cpp`, `bowtie_build_main.cpp`, `bt2_inspect.cpp` | `#ifdef _WIN32`: at the top of `main()`, `_fmode = _O_BINARY` and `_setmode(stdout/stderr, _O_BINARY)` (+ `<fcntl.h>`/`<io.h>`) | Windows text mode turns every `\n` written into `\r\n` (SAM, FASTA, logs) and turns CRLF into LF inside `read()`. The latter broke CRLF FASTQ input: `is_zstd_file` got a short read and returned without rewinding, so the plain reader started mid-record ("does not look like a FASTQ file"). `_fmode` makes every file opened afterwards binary, so no call site needed changing. |
| `pat.h`, `pat.cpp` | `#ifdef WITH_BZIP2`: a `BZIP2` compression type next to `GZIP`/`ZSTD` — detection in `CFilePatternSource::open()`, and branches in the destructor, `open()`'s close switch, `getc_wrapper` and `ungetc_wrapper` | Native `.bz2` input. Upstream reads bzip2 only through the Perl wrapper (`bzip2 -dc` into a named pipe), which does not exist on Windows. |
| `bzip2_reader.h` / `bzip2_reader.cpp` *(new)* | Streaming libbz2 reader with the shape of upstream's `zstd_decompress.h`. Detects bzip2 by the file's first bytes, reads concatenated streams in order, ignores bytes after the last stream (as `bzip2 -dc`), and stops with an error naming the file on a truncated or corrupt stream | Keeps all libbz2 code out of the shared headers. |
| `Makefile` | On `$(OS)==Windows_NT`: set `WINDOWS`/`MINGW`, take `ARCH` from `$(CXX) -dumpmachine`, and keep the cross-compiler names (`x86_64-w64-mingw32-g++-posix`, …) for cross builds only | Upstream detects Windows only when `$(CXX) --version` contains "mingw", which MSYS2's GCC and Clang do not print; `uname -m` reports `x86_64` even in the CLANGARM64 shell. Without this the build pulls `<sys/mman.h>` and passes `-msse2` to the ARM compiler. |
| `Makefile` | `WINDOWS_STATIC ?= 1` around the Windows `-static` flags | Lets an installer build link the runtime and compression libraries as DLLs. Default unchanged. |
| `Makefile` | `BUILD_HOST ?=` the host name, used for `-DBUILD_HOST` | Distributed builds can keep the build machine's name out of `--version`. Default output unchanged. |
| `Makefile` | `WITH_BZIP2=1` block next to `WITH_ZSTD` (`-DWITH_BZIP2`, `-lbz2`, `bzip2_reader.cpp`) | Opt-in like upstream's zstd support. |
| `Makefile` | On a native Windows build, `bowtie2.exe` / `bowtie2-build.exe` / `bowtie2-inspect.exe` (rules compiling `bowtie2_launcher.c`) in the binary list and package instead of the `.bat` files; `bowtie2-bin-pkg` adds no second `.exe` to them; `launchers-test` runs `xplat_tests/run_parity.sh` | The `.bat` files need Perl and Python. Cross builds and Linux/macOS keep upstream's files and default targets. |
| `bowtie2_launcher.c` *(new)* | Native ports of the `bowtie2` (Perl), `bowtie2-build` and `bowtie2-inspect` (Python) wrappers, one source compiled with `-DLAUNCH_ALIGN` / `-DLAUNCH_BUILD` / `-DLAUNCH_INSPECT`; compressed outputs with zlib, libbz2, libzstd and liblz4 | A script is not a runnable command on Windows. See [the launchers](#the-bowtie2-bowtie2-build-and-bowtie2-inspect-launchers). |
| `xplat_tests/` *(new)* | Parity harness: launchers vs scripts | Proves the launchers behave like the scripts. |

### Upstream limitations (not changed here)

These behave the same in upstream Bowtie 2 on every platform and are left as they
are, to keep this fork minimal:

- **A truncated `.gz` or `.zst` file can pass silently.** With zlib 1.3.2, zlib
  reports a damaged gzip stream only together with the read that returns the last
  good bytes and clears the error on the next call; Bowtie 2 reads gzip one
  character at a time and never sees it. Upstream's zstd reader treats an
  unfinished frame as the end of the file. If the damage falls inside a record,
  the read parser still fails; if it falls at a record boundary, the reads after
  it are dropped and the run succeeds. Check archives with `gzip -t` / `zstd -t`
  first. (Truncated `.bz2` input is always an error in this fork.)
- **Bad input aborts instead of exiting cleanly.** A malformed read file stops
  Bowtie 2 with an uncaught exception ("terminate called after throwing an
  instance of 'int'"). The exit code is non-zero, so pipelines still see the
  failure.
- **No memory-mapped or shared-memory index on Windows** (`--mm`), as upstream.

---

## Related work

[win-ngs](https://github.com/win-ngs) publishes community Windows builds of
several NGS tools, including
[bowtie2-windows-build](https://github.com/win-ngs/bowtie2-windows-build)
(x64, MSYS2 UCRT64, PowerShell launchers). Its README's notes on building with
MSYS2 (the `MINGW=1 WINDOWS=1` workaround) and on the zstd/plain-FASTQ failure
helped locate the issues fixed here; this fork shares no code with it. It differs
in building natively for ARM64, using one `_fmode` switch instead of per-call
binary modes, and adding bzip2 input.

## Redistribution & licensing

Bowtie 2 is **GPL-3.0** (see `LICENSE`). Redistributing a binary requires its
license and source (or a written offer), plus the notices of everything linked
in — zlib, bzip2, zstd, and on Windows the GCC/libstdc++ (or LLVM libc++) and
winpthread runtimes. A `WINDOWS_STATIC=0` build keeps those runtimes as
replaceable DLLs; ship each DLL's license with it.
