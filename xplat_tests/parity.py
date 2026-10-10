"""Compare the C launchers (bowtie2_launcher.c) with the upstream Perl/Python
wrapper scripts. xplat_tests/run_parity.sh sets up the directories; see
xplat_tests/README.md.

usage: python3 parity.py REF_DIR NEW_DIR DATA_DIR WORK_DIR [case-substring]
       python3 parity.py dump TOOL_DIR DATA_DIR WORK_DIR OUT.json [case-substring]
       python3 parity.py cmp A.json B.json      (e.g. Windows vs macOS launcher)

Each case runs the same command line through REF_DIR/<tool> and NEW_DIR/<tool>,
each in its own empty working directory, and compares exit code, stdout,
stderr and every file written there. Directory paths and clock times are
normalised; gzip/bzip2/zstd/lz4 outputs are compared by compression type plus
decompressed content, BAM as the text `samtools view -h` prints. Exit status
1 means a difference that is not in EXPECTED / EXPECTED_PG / EXPECTED_XPLAT.
"""
import bz2, difflib, gzip, json, os, re, shutil, subprocess, sys

MODE = sys.argv[1] if sys.argv[1] in ("dump", "cmp") else "compare"
if MODE == "compare":
    REF, NEW, TD, WORK = (os.path.abspath(p) for p in sys.argv[1:5])
    ONLY = sys.argv[5] if len(sys.argv) > 5 else ""
elif MODE == "dump":
    NEW, TD, WORK = (os.path.abspath(p) for p in sys.argv[2:5])
    OUT = sys.argv[5]
    ONLY = sys.argv[6] if len(sys.argv) > 6 else ""

X = ["-x", "{TD}/idx"]
SE = X + ["-U", "{TD}/se.fq"]
PE = X + ["-1", "{TD}/m1.fq", "-2", "{TD}/m2.fq"]
O = ["-S", "out.sam"]
NZ = "{CWD}/za\u017c\u00f3\u0142\u0107"
CASES = [
    # (name, tool, args, stdin file or None[, extra environment])
    ("align-noargs", "bowtie2", [], None),
    ("align-help", "bowtie2", ["-h"], None),
    ("align-version", "bowtie2", ["--version"], None),
    ("no-binaries", "bowtie2", X, None),
    ("se-plain", "bowtie2", SE + O, None),
    ("se-gz", "bowtie2", X + ["-U", "{TD}/se.fq.gz"] + O, None),
    ("se-bz2", "bowtie2", X + ["-U", "{TD}/se.fq.bz2"] + O, None),
    ("se-zst", "bowtie2", X + ["-U", "{TD}/se.fq.zst"] + O, None),
    ("se-lz4", "bowtie2", X + ["-U", "{TD}/se.fq.lz4"] + O, None),
    ("se-mixed-order", "bowtie2", X + ["-U", "{TD}/se.fq.lz4", "{TD}/se.fq.gz", "--al", "al.fq"] + O, None),
    ("se-two-wrapped", "bowtie2", X + ["-U", "{TD}/se.fq.lz4", "{TD}/se.fq.bz2", "{TD}/se.fq"] + O, None),
    ("se-stdout", "bowtie2", SE, None),
    ("se-stdin", "bowtie2", X + ["-U", "-"] + O, "se.fq"),
    ("se-commalist", "bowtie2", X + ["-U", "{TD}/se.fq,{TD}/se.fq.gz"] + O, None),
    ("se-multi-values", "bowtie2", X + ["-U", "{TD}/se.fq", "{TD}/se.fq.zst", "-", "--reads", "{TD}/se.fq.gz"] + O, "se.fq"),
    ("se-fasta", "bowtie2", X + ["-f", "-U", "{TD}/se.fa"] + O, None),
    ("se-seqs-c", "bowtie2", X + ["-c", "-U", "{SEQ1},{SEQ2}"] + O, None),
    ("se-local", "bowtie2", SE + ["--local", "--un", "un.fq"] + O, None),
    ("se-threads-reorder", "bowtie2", SE + ["-p", "3", "--reorder", "--no-unal"] + O, None),
    ("se-rg", "bowtie2", SE + ["--rg-id", "g 1", "--rg", "SM:a b", "--no-unal"] + O, None),
    ("pe-plain", "bowtie2", PE + O, None),
    ("pe-attached", "bowtie2", X + ["-1{TD}/m1.fq", "-2{TD}/m2.fq"] + O, None),
    ("pe-long-eq", "bowtie2", X + ["--1", "{TD}/m1.fq", "--2={TD}/m2.fq"] + O, None),
    ("pe-lz4", "bowtie2", X + ["-1", "{TD}/m1.fq.lz4", "-2", "{TD}/m2.fq.lz4"] + O, None),
    ("pe-mixed-order", "bowtie2", X + ["-1", "{TD}/m1.fq.lz4", "{TD}/m1.fq", "-2", "{TD}/m2.fq", "{TD}/m2.fq"] + O, None),
    ("pe-only-m1", "bowtie2", X + ["-1", "{TD}/m1.fq"] + O, None),
    ("interleaved", "bowtie2", X + ["--interleaved", "{TD}/il.fq", "--un-conc", "uc.fq"] + O, None),
    ("tab5", "bowtie2", X + ["--tab5", "{TD}/pe.tab5", "--un", "un.fq", "--al-conc", "ac.fq"] + O, None),
    ("tab6", "bowtie2", X + ["--tab6", "{TD}/pe.tab6", "--al", "al.fq", "--un-conc", "uc.fq"] + O, None),
    ("bam-in-se", "bowtie2", X + ["-b", "{TD}/se.bam", "--un", "un.fq"] + O, None),
    ("bam-in-pe", "bowtie2", X + ["-b", "{TD}/pe.bam", "--align-paired-reads", "--un-conc", "uc.fq"] + O, None),
    ("se-un-al", "bowtie2", SE + ["--un", "un.fq", "--al", "al.fq"] + O, None),
    ("se-un-eq", "bowtie2", SE + ["--un=un.fq"] + O, None),
    ("se-un-gz-al-bz2", "bowtie2", SE + ["--un-gz", "un.fq.gz", "--al-bz2", "al.fq.bz2"] + O, None),
    ("se-un-lz4-al-zst", "bowtie2", SE + ["--un-lz4", "un.fq.lz4", "--al-zst", "al.fq.zst"] + O, None),
    ("se-un-stdout", "bowtie2", SE + ["--un", "un.fq"], None),
    ("se-un-dir", "bowtie2", SE + ["--un", "{CWD}", "--al-gz", "{CWD}/"] + O, None),
    ("se-un-missing-dir", "bowtie2", SE + ["--un", "nodir/un.fq"] + O, None),
    ("se-no-unal", "bowtie2", SE + ["--no-unal"] + O, None),
    ("se-no-unal-un", "bowtie2", SE + ["--no-unal", "--un", "un.fq"], None),
    ("se-no-unal-S-last", "bowtie2", SE + ["--no-unal", "-S"], None),
    ("se-no-unal-missing-dir", "bowtie2", SE + ["--no-unal", "-S", "nodir/out.sam"], None),
    ("se-S-attached", "bowtie2", SE + ["--no-unal", "-Sout.sam"], None),
    ("pe-conc-ext", "bowtie2", PE + ["--un-conc", "uc.fq", "--al-conc", "ac.fq"] + O, None),
    ("pe-conc-pct-gz", "bowtie2", PE + ["--un-conc-gz", "uc_%.fq.gz", "--al-conc-bz2", "ac_%.fq.bz2"] + O, None),
    ("pe-conc-lz4-zst", "bowtie2", PE + ["--un-conc-lz4", "uc", "--al-conc-zst", "ac.fq.zst"] + O, None),
    ("pe-conc-dir", "bowtie2", PE + ["--al-conc", "{CWD}", "--un-conc", "sub/"] + O, None),
    ("pe-conc-missing-dir", "bowtie2", PE + ["--un-conc", "nodir/uc.fq"] + O, None),
    ("pe-un-mates", "bowtie2", PE + ["--un-mates", "um.fq", "--un-conc", "uc.fq", "--al", "al.fq"] + O, None),
    ("pe-un-mates-gz-dir", "bowtie2", PE + ["--un-mates-gz", "{CWD}"] + O, None),
    ("pe-un-mates-zst-no-unal", "bowtie2", PE + ["--un-mates-zst", "um_%.zst", "--no-unal"] + O, None),
    ("bam-out", "bowtie2", SE + ["--bam", "-S", "out.bam"], None),
    ("bam-out-stdout", "bowtie2", PE + ["--bam"], None),
    ("bam-out-un-no-unal", "bowtie2", SE + ["--bam", "--no-unal", "--un-gz", "un.fq.gz", "-S", "out.bam"], None),
    ("bam-no-samtools", "bowtie2", SE + ["--bam", "-S", "out.bam"], None, {"PATH": "{NOSAM}"}),
    ("large-auto", "bowtie2", ["-x", "{TD}/lidx", "-U", "{TD}/se.fq"] + O, None),
    ("large-forced", "bowtie2", ["-x", "{TD}/lidx", "--large-index", "-U", "{TD}/se.fq"] + O, None),
    ("large-forced-missing", "bowtie2", SE + ["--large-index"] + O, None),
    ("missing-read", "bowtie2", X + ["-U", "nosuch.fq"] + O, None),
    ("missing-index", "bowtie2", ["-x", "nosuchidx", "-U", "{TD}/se.fq"] + O, None),
    ("index-prefix-glob", "bowtie2", ["-x", "{TD}/id", "-U", "{TD}/se.fq"] + O, None),
    ("index-env", "bowtie2", ["-x", "lidx", "-U", "{TD}/se.fq", "--verbose"] + O, None, {"BOWTIE2_INDEXES": "{TD}"}),
    ("index-env-missing", "bowtie2", ["-x", "nosuch", "-U", "{TD}/se.fq"] + O, None, {"BOWTIE2_INDEXES": "{TD}"}),
    ("temp-directory-keep", "bowtie2", SE + ["--temp-directory", "{CWD}", "--keep", "--no-named-pipes"] + O, None),
    ("log-file", "bowtie2", SE + ["--log-file", "log.txt", "--un", "un.fq"] + O, None),
    ("log-file-missing-dir", "bowtie2", SE + ["--log-file", "nodir/log.txt"] + O, None),
    ("verbose-noreads", "bowtie2", X + ["--verbose"], None),
    ("verbose-noindex", "bowtie2", ["--verbose", "-U", "{TD}/se.fq"], None),
    ("verbose-reads", "bowtie2", SE + ["--verbose", "--rg-id", "g"] + O, None),
    ("verbose-passthrough", "bowtie2", SE + ["--verbose", "--un", "un.fq"] + O, None),
    ("debug-missing", "bowtie2", SE + ["--debug"] + O, None),
    ("sanitized-missing", "bowtie2", SE + ["--sanitized", "--no-unal"] + O, None),
    ("ref-string", "bowtie2", ["--ref-string", "ACGT", "-U", "{TD}/se.fq"] + O, None),
    # Paths with spaces and non-ASCII characters (setup copies the reads there).
    ("space-path", "bowtie2", X + ["-U", "{CWD}/a b/r 1.fq", "--un", "{CWD}/a b/un 1.fq", "-S", "{CWD}/a b/o 1.sam"], None),
    ("space-path-pe", "bowtie2", X + ["-1", "{CWD}/a b/r 1.fq", "-2", "{CWD}/a b/r 1.fq", "--al-conc", "{CWD}/a b/%.fq", "-S", "{CWD}/a b/o 1.sam"], None),
    ("nonascii-path", "bowtie2", X + ["-U", NZ + "/r\u0119ads.fq", "--un-gz", NZ + "/\u0105.fq", "-S", NZ + "/o.sam"], None),
    # GetOptions details: `--` ends the wrapper options; -U=x drops "=x" and takes the
    # next token; --un= with an empty value and +name.
    ("dd-first", "bowtie2", ["--"] + SE + O, None),
    ("dd-split", "bowtie2", ["-U", "{TD}/se.fq", "--no-unal", "--"] + X + ["--un", "un.fq"] + O, None),
    ("bundled-eq", "bowtie2", X + ["-U=ignored", "{TD}/se.fq"] + O, None),
    ("long-empty-value", "bowtie2", SE + ["--un="] + O, None),
    ("plus-prefix", "bowtie2", X + ["+U", "{TD}/se.fq", "+no-unal"] + O, None),
    ("flag-with-value", "bowtie2", SE + ["--no-unal=1"] + O, None),
    ("build-noargs", "bowtie2-build", [], None),
    ("build", "bowtie2-build", ["-q", "{TD}/ref.fa", "bidx"], None),
    ("build-verbose", "bowtie2-build", ["-q", "--verbose", "{TD}/ref.fa", "bidx"], None),
    ("build-large", "bowtie2-build", ["-q", "--large-index", "{TD}/ref.fa", "bidx"], None),
    ("build-multi-fasta", "bowtie2-build", ["-q", "--verb", "--threads", "2", "{TD}/ref.fa", "{TD}/ref.fa.gz", "bidx"], None),
    ("build-gz", "bowtie2-build", ["--verbose", "-q", "{TD}/ref.fa.gz", "bidx"], None),
    ("build-zst", "bowtie2-build", ["-q", "{TD}/se.fq.zst", "bidx"], None),
    ("build-zst-verbose", "bowtie2-build", ["-q", "--verbose", "{TD}/se.fq.zst", "bidx"], None),
    ("build-debug-missing", "bowtie2-build", ["--debug", "-q", "{TD}/ref.fa", "bidx"], None),
    ("build-mutex", "bowtie2-build", ["--sanitized", "--deb", "{TD}/ref.fa", "bidx"], None),
    ("build-explicit-arg", "bowtie2-build", ["--large=1", "{TD}/ref.fa", "bidx"], None),
    ("build-dd", "bowtie2-build", ["-q", "--", "--verbose", "{TD}/ref.fa", "bidx"], None),
    ("inspect-noargs", "bowtie2-inspect", [], None),
    ("inspect-summary", "bowtie2-inspect", ["-s", "{TD}/idx"], None),
    ("inspect-names-large", "bowtie2-inspect", ["-n", "{TD}/lidx"], None),
    ("inspect-forced-large", "bowtie2-inspect", ["--large-index", "--verbose", "-n", "{TD}/lidx"], None),
    ("inspect-verbose", "bowtie2-inspect", ["--verbose", "-n", "{TD}/idx"], None),
    ("inspect-mutex", "bowtie2-inspect", ["--debug", "--sanitized", "{TD}/idx"], None),
    ("inspect-debug-missing", "bowtie2-inspect", ["--debug", "-n", "{TD}/idx"], None),
]

SEQ1 = SEQ2 = None


def load_seqs():
    global SEQ1, SEQ2
    lines = open(os.path.join(TD, "se.fq")).read().split("\n")
    SEQ1, SEQ2 = lines[1], lines[5]


def norm(text, tooldir, cwd):
    """Replace the run's directories by tags. Each path may appear as given or
    resolved (the launcher resolves symlinks, e.g. macOS /tmp -> /private/tmp),
    with either separator; longer spellings go first. Clock times (verbose
    output) become <TIME>."""
    forms = []
    for path, tag in ((cwd, "<CWD>"), (tooldir, "<BIN>"), (TD, "<TD>")):
        for p in {path, os.path.realpath(path)}:
            forms += [(f, tag) for f in {p, p.replace("\\", "/"), p.replace("/", "\\")}]
    for form, tag in sorted(forms, key=lambda ft: -len(ft[0])):
        text = text.replace(form, tag)
    return re.sub(r"\b\d\d:\d\d:\d\d\b", "<TIME>", text)


def lz4_decompress(raw):
    """lz4 frames (as written by the lz4 program / liblz4), in pure Python."""
    out, i = bytearray(), 0
    while i < len(raw):
        magic = int.from_bytes(raw[i:i + 4], "little")
        i += 4
        if magic & 0xFFFFFFF0 == 0x184D2A50:         # skippable frame
            i += 4 + int.from_bytes(raw[i:i + 4], "little")
            continue
        if magic != 0x184D2204:
            raise ValueError("not an lz4 frame")
        flg = raw[i]
        i += 3 + (8 if flg & 8 else 0) + (4 if flg & 1 else 0)
        while True:
            size = int.from_bytes(raw[i:i + 4], "little")
            i += 4
            if size == 0:
                break
            blk, i = raw[i:i + (size & 0x7FFFFFFF)], i + (size & 0x7FFFFFFF) + (4 if flg & 0x10 else 0)
            if size & 0x80000000:
                out += blk
                continue
            j = 0
            while j < len(blk):
                tok = blk[j]
                j += 1
                n = tok >> 4
                if n == 15:
                    while True:
                        n += blk[j]
                        j += 1
                        if blk[j - 1] != 255:
                            break
                out += blk[j:j + n]
                j += n
                if j >= len(blk):
                    break
                off, j, m = blk[j] | blk[j + 1] << 8, j + 2, tok & 15
                if m == 15:
                    while True:
                        m += blk[j]
                        j += 1
                        if blk[j - 1] != 255:
                            break
                for _ in range(m + 4):
                    out.append(out[-off])
        i += 4 if flg & 4 else 0
    return bytes(out)


def zstd_decompress(raw):
    try:
        from compression import zstd
        return zstd.decompress(raw)
    except ImportError:
        return subprocess.run(["zstd", "-dc"], input=raw, capture_output=True, check=True).stdout


def content(path=None, raw=None):
    """(kind, decoded bytes) of a file or of captured output."""
    raw = open(path, "rb").read() if raw is None else raw
    kind = "plain"
    if raw[:2] == b"\x1f\x8b":
        kind, raw = "gzip", gzip.decompress(raw)
    elif raw[:3] == b"BZh":
        kind, raw = "bzip2", bz2.decompress(raw)
    elif raw[:4] == b"\x28\xb5\x2f\xfd":
        kind, raw = "zstd", zstd_decompress(raw)
    elif raw[:4] == b"\x04\x22\x4d\x18":
        kind, raw = "lz4", lz4_decompress(raw)
    if raw[:4] == b"BAM\1":
        kind += "+bam"
        # samtools on PATH, else next to the launchers (as in an installed tool folder)
        sam = shutil.which("samtools") or (shutil.which("samtools", path=NEW) if "NEW" in globals() else None)
        if sam:
            raw = subprocess.run(["samtools", "view", "-h", "-"], executable=sam,   # @PG CL: "samtools"
                                 input=raw, capture_output=True, check=True).stdout
    return kind, raw


INPUT_COPIES = ("r 1.fq", "r\u0119ads.fq")


def run(tooldir, case, cwd):
    name, tool, args, stdin = case[:4]
    env = dict(os.environ)
    shutil.rmtree(cwd, ignore_errors=True)
    os.makedirs(cwd)
    nosam = os.path.join(os.path.dirname(cwd), "nosam")  # PATH with perl but no samtools
    os.makedirs(nosam, exist_ok=True)
    perl = shutil.which("perl")
    if perl and not os.path.exists(os.path.join(nosam, "perl")):
        try:
            os.symlink(perl, os.path.join(nosam, "perl"))
        except OSError:
            pass
    for k, v in (case[4] if len(case) > 4 else {}).items():
        env[k] = v.replace("{TD}", TD).replace("{NOSAM}", nosam)
    for a in args:                          # reads under {CWD}/... are copies of se.fq
        if a.startswith("{CWD}/") and a.endswith(INPUT_COPIES):
            dst = a.replace("{CWD}", cwd)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy(os.path.join(TD, "se.fq"), dst)
    if "sub/" in args:
        os.makedirs(os.path.join(cwd, "sub"))
    exe = os.path.join(tooldir, tool)
    ext = ".exe" if os.name == "nt" else ""
    if name == "no-binaries":               # the wrapper alone, without bowtie2-align-*
        os.makedirs(os.path.join(cwd, "lone"))
        exe = os.path.join(cwd, "lone", tool)
        shutil.copy(os.path.join(tooldir, tool + ext), exe + ext)
        for f in os.listdir(tooldir):       # a DLL-linked Windows build needs its DLLs
            if f.lower().endswith(".dll"):
                shutil.copy(os.path.join(tooldir, f), os.path.join(cwd, "lone", f))
    if name == "bam-no-samtools":           # the tools without samtools beside them
        os.makedirs(os.path.join(cwd, "lone"))
        for f in os.listdir(tooldir):
            if not f.lower().startswith("samtools") and os.path.isfile(os.path.join(tooldir, f)):
                shutil.copy(os.path.join(tooldir, f), os.path.join(cwd, "lone", f))
        exe = os.path.join(cwd, "lone", tool)
    argv = [exe] + [a.replace("{TD}", TD).replace("{CWD}", cwd).replace("{SEQ1}", SEQ1).replace("{SEQ2}", SEQ2)
                    for a in args]
    inp = open(os.path.join(TD, stdin), "rb") if stdin else subprocess.DEVNULL
    p = subprocess.run(argv, cwd=cwd, stdin=inp, capture_output=True, env=env)
    files = {}
    for root, _, fs in os.walk(cwd):
        for f in fs:
            full = os.path.join(root, f)
            rel = os.path.relpath(full, cwd).replace(os.sep, "/")
            if f in INPUT_COPIES or rel.startswith("lone/"):
                continue
            kind, data = content(full)
            files[rel] = (kind, norm(data.decode("latin-1"), tooldir, cwd))
    kind, out = content(raw=p.stdout)
    return {
        "exit": p.returncode,
        "stdout": (kind + ":" if kind != "plain" else "") + norm(out.decode("latin-1"), tooldir, cwd),
        "stderr": norm(p.stderr.decode("latin-1"), tooldir, cwd),
        "files": files,
    }


def diff(a, b, label):
    d = list(difflib.unified_diff(a.splitlines(), b.splitlines(), "A (reference)", "B (launcher)", lineterm="", n=1))
    return [f"    {label}:"] + ["      " + x for x in d[:16]] + (["      ..."] if len(d) > 16 else [])


# Text the aligner binary itself prints differently per platform (not the launcher):
# the Windows build has no --mm (memory-mapping) usage line, the C library's getopt
# words its errors differently, and upstream reports a failed fstat() only on POSIX.
XPLAT_TEXT = [
    (r"(?m)^  --mm  +use memory-mapped I/O.*\n", ""),
    (r"(?m)^(?:\S*/)?(bowtie2-[\w-]+): option `--?([\w-]+)' is ambiguous.*$", r"\1: ambiguous option \2"),
    (r"(?m)^(?:\S*/)?(bowtie2-[\w-]+): ambiguous option -- ([\w-]+)$", r"\1: ambiguous option \2"),
    (r"(?m)^(?:\S*/)?(bowtie2-[\w-]+): option `--?([\w-]+)' doesn't allow an argument$", r"\1: no argument allowed \2"),
    (r"(?m)^(?:\S*/)?(bowtie2-[\w-]+): option doesn't take an argument -- ([\w-]+)$", r"\1: no argument allowed \2"),
    (r"(?m)^stat: Bad file descriptor\n", ""),
]


def xnorm(text):
    """Cross-platform: separators, the .exe suffix, the launcher's temporary
    files for .lz4 reads (temporary directory and process id), and XPLAT_TEXT."""
    text = text.replace("\\", "/").replace(".exe", "")
    for pattern, repl in XPLAT_TEXT:
        text = re.sub(pattern, repl, text)
    out, last = [], 0
    for m in re.finditer(r"bowtie2_\d+_(\d+\.\w+)", text):
        start = m.start()                   # back to the start of the path
        while start > last and text[start - 1] not in ' \t\n,"':
            start -= 1
        out.append(text[last:start] + "<TMP>/bowtie2_<PID>_" + m.group(1))
        last = m.end()
    return "".join(out) + text[last:]


def no_pg(d):
    """The result without SAM @PG lines (their command line names the read files)."""
    strip = lambda t: "\n".join(l for l in t.split("\n") if not l.startswith("@PG\tID:bowtie2"))
    return {"exit": d["exit"], "stdout": strip(d["stdout"]), "stderr": d["stderr"],
            "files": {k: (v[0], strip(v[1])) for k, v in d["files"].items()}}


def compare(r, n):
    problems = []
    if r["exit"] != n["exit"]:
        problems.append(f"    exit: {r['exit']} vs {n['exit']}")
    for k in ("stdout", "stderr"):
        if r[k] != n[k]:
            problems += diff(r[k], n[k], k)
    for f in sorted(set(r["files"]) | set(n["files"])):
        if f not in r["files"] or f not in n["files"]:
            problems.append(f"    file {f}: only in {'B' if f in n['files'] else 'A'}")
        elif r["files"][f] != n["files"][f]:
            (rk, rc), (nk, nc) = r["files"][f], n["files"][f]
            if rk != nk:
                problems.append(f"    file {f}: {rk} vs {nk}")
            if rc != nc:
                problems += diff(rc, nc, f"file {f}")
    return problems


# Known, deliberate differences (see README.md "The launchers"): case -> reason.
# EXPECTED_PG: only the bowtie2 @PG line may differ (checked with it removed).
PG = "@PG command line: the script feeds .bz2/.lz4 reads through a named pipe"
EXPECTED_PG = {c: PG for c in ("se-bz2", "se-lz4", "se-mixed-order", "pe-lz4", "pe-mixed-order")}
EXPECTED = {
    "debug-missing": "the 'cannot run' message comes from sh under Perl",
    "sanitized-missing": "the 'cannot run' message comes from sh under Perl",
    "inspect-debug-missing": "Python prints a traceback for the missing binary",
    "se-S-attached": "-S<file> is honoured; the script passes it to the binary, which then writes the passthrough lines into it",
    "se-two-wrapped": "the script feeds only the first of several .bz2/.lz4 files of an option (the others are dropped)",
    "verbose-passthrough": "the binary's --verbose lines reach the SAM stream; Perl warns about them",
    "ref-string": "--ref-string is not supported (the script builds a temporary index)",
}
# Windows vs macOS launcher: differences in the binaries themselves.
EXPECTED_XPLAT = {
    "align-noargs": "usage: Windows build has no --mm (memory-mapping off)",
    "align-help": "usage: Windows build has no --mm (memory-mapping off)",
    "align-version": "--version prints build host, date and type sizes",
    "debug-missing": "Windows cannot start the missing binary at all (exit 1)",
    "sanitized-missing": "Windows cannot start the missing binary at all (exit 1)",
    "inspect-debug-missing": "Windows cannot start the missing binary at all (exit 1)",
    "bam-out": "differs only if samtools is missing or another version on Windows",
    "bam-out-stdout": "differs only if samtools is missing or another version on Windows",
    "bam-out-un-no-unal": "differs only if samtools is missing or another version on Windows",
}


def report(name, problems, expected, extra=""):
    """Print one case; return 1 if it differs unexpectedly."""
    status = "SAME" if not problems else ("EXPECTED-DIFF" if name in expected else "DIFF")
    print(f"{status}  {name}{extra}" + (f"  ({expected[name]})" if status == "EXPECTED-DIFF" else ""))
    if status == "DIFF":
        for line in problems:
            print(line)
    return status == "DIFF"


def main():
    if MODE == "cmp":
        a, b = (json.load(open(p, encoding="utf-8")) for p in sys.argv[2:4])
        fix = lambda d: {"exit": d["exit"], "stdout": xnorm(d["stdout"]), "stderr": xnorm(d["stderr"]),
                         "files": {k: (v[0], xnorm(v[1])) for k, v in d["files"].items()}}
        failed = sum(report(name, compare(fix(a[name]), fix(b[name])), EXPECTED_XPLAT)
                     for name in a if name in b)
    else:
        load_seqs()
        if MODE == "dump":
            out = {}
            for case in CASES:
                if ONLY in case[0]:
                    out[case[0]] = run(NEW, case, os.path.join(WORK, case[0]))
                    print("ran", case[0], "exit", out[case[0]]["exit"])
            json.dump(out, open(OUT, "w", encoding="utf-8"), indent=1)
            return
        failed = n = 0
        for case in CASES:
            if ONLY in case[0]:
                n += 1
                r = run(REF, case, os.path.join(WORK, "ref", case[0]))
                c = run(NEW, case, os.path.join(WORK, "new", case[0]))
                problems = compare(r, c)
                if problems and case[0] in EXPECTED_PG:
                    rest = compare(no_pg(r), no_pg(c))
                    failed += report(case[0], rest or problems, {} if rest else EXPECTED_PG)
                else:
                    failed += report(case[0], problems, EXPECTED)
        print(f"\n{n} case(s), ", end="")
    print(f"{failed} unexpected difference(s)")
    sys.exit(1 if failed else 0)


main()
