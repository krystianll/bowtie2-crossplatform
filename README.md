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
adds a native build on Windows itself, for x64 **and ARM64**, LF-only output, and
compressed input without external tools. Native launchers that replace the
Perl/Python wrapper scripts on Windows are in progress.

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

### Verify (any platform)

```bash
./bowtie2-build-s example/reference/lambda_virus.fa lambda
./bowtie2-align-s -x lambda -U example/reads/reads_1.fq -S out.sam   # also .gz/.bz2/.zst
```

---

## Patches (vs upstream `58e34bf`)

Every edit to an upstream file carries a `bowtie2-crossplatform:` comment, so

```bash
grep -rn "bowtie2-crossplatform:" --include='*.cpp' --include='*.h' --include=Makefile .
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
