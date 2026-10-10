"""Deterministic test data for the bowtie2 launcher parity tests.

Writes into the directory given as argv[1]:
  ref.fa            two random chromosomes; also as ref.fa.gz
  se.fq             single-end: aligning reads (lengths 100 and 75, some with a
                    name comment) + random reads; also as .gz, .bz2, .zst, .lz4
  m1.fq / m2.fq     paired: concordant pairs (--fr), pairs with one random mate,
                    random pairs; also as .lz4
  se.fa             FASTA reads (for -f)
  il.fq             the pairs, interleaved (--interleaved)
  pe.tab5 / pe.tab6 the pairs plus some unpaired reads (--tab5 / --tab6)
  se.bam / pe.bam   unaligned BAM, unpaired / paired (-b, --align-paired-reads)

zstd and lz4 files are made with the zstd/lz4 programs when they are on PATH,
otherwise with Python's compression.zstd and stored (uncompressed) lz4 blocks.
"""
import bz2, gzip, os, random, shutil, struct, subprocess, sys, zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rnd = random.Random(7)
comp = str.maketrans("ACGT", "TGCA")


def p(name):
    return os.path.join(out, name)


def rc(s):
    return s.translate(comp)[::-1]


def write(name, text):
    with open(p(name), "w", newline="\n") as f:
        f.write(text)


chroms = {name: "".join(rnd.choice("ACGT") for _ in range(6000)) for name in ("chrA", "chrB")}
write("ref.fa", "".join(f">{n}\n" + "".join(s[i:i + 60] + "\n" for i in range(0, len(s), 60))
                        for n, s in chroms.items()))


def sample(length):
    seq = chroms[rnd.choice(sorted(chroms))]
    q = rnd.randint(0, len(seq) - length)
    s = seq[q:q + length]
    return s if rnd.random() < 0.5 else rc(s)


def rand_seq(length):
    return "".join(rnd.choice("ACGT") for _ in range(length))


def qual(n):
    return "".join("I5?+"[(i * 7) % 4] for i in range(n))


se = [(f"a{i}" + (" c=x\ty" if i % 10 == 0 else ""), sample(100 if i % 3 else 75)) for i in range(120)]
se += [(f"r{i}", rand_seq(100)) for i in range(40)]
write("se.fq", "".join(f"@{n}\n{s}\n+\n{qual(len(s))}\n" for n, s in se))
write("se.fa", "".join(f">{n}\n{s}\n" for n, s in se))

pairs = []
for i in range(90):
    if i < 60:
        seq = chroms[sorted(chroms)[i % 2]]
        q = rnd.randint(0, len(seq) - 300)
        frag = seq[q:q + 300]
        a, b = frag[:100], rc(frag[-100:])
    elif i < 75:                            # mate 1 aligns, mate 2 does not
        a, b = sample(100), rand_seq(100)
    else:
        a, b = rand_seq(100), rand_seq(100)
    pairs.append((f"p{i}", a, b))
write("m1.fq", "".join(f"@{n}/1\n{a}\n+\n{qual(100)}\n" for n, a, _ in pairs))
write("m2.fq", "".join(f"@{n}/2\n{b}\n+\n{qual(100)}\n" for n, _, b in pairs))
write("il.fq", "".join(f"@{n}/1\n{a}\n+\n{qual(100)}\n@{n}/2\n{b}\n+\n{qual(100)}\n" for n, a, b in pairs))
unp = [(f"u{i}", sample(80) if i % 2 else rand_seq(80)) for i in range(10)]
write("pe.tab5", "".join(f"{n}\t{a}\t{qual(100)}\t{b}\t{qual(100)}\n" for n, a, b in pairs) +
      "".join(f"{n}\t{s}\t{qual(80)}\n" for n, s in unp))
write("pe.tab6", "".join(f"{n}/1\t{a}\t{qual(100)}\t{n}/2\t{b}\t{qual(100)}\n" for n, a, b in pairs) +
      "".join(f"{n}\t{s}\t{qual(80)}\n" for n, s in unp))

# Compressed copies of the reference and the read files.
with gzip.open(p("ref.fa.gz"), "wb") as f:
    f.write(open(p("ref.fa"), "rb").read())
raw = open(p("se.fq"), "rb").read()
with gzip.open(p("se.fq.gz"), "wb") as f:
    f.write(raw)
with bz2.open(p("se.fq.bz2"), "wb") as f:
    f.write(raw)


def xxh32(data, seed=0):
    P1, P2, P3, P4, P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393
    M = 0xFFFFFFFF
    rotl = lambda x, r: ((x << r) | (x >> (32 - r))) & M
    n, i = len(data), 0
    if n >= 16:
        v = [(seed + P1 + P2) & M, (seed + P2) & M, seed, (seed - P1) & M]
        while i + 16 <= n:
            for k in range(4):
                v[k] = rotl((v[k] + struct.unpack_from("<I", data, i + 4 * k)[0] * P2) & M, 13) * P1 & M
            i += 16
        h = (rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18)) & M
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 4 <= n:
        h = rotl((h + struct.unpack_from("<I", data, i)[0] * P3) & M, 17) * P4 & M
        i += 4
    while i < n:
        h = rotl((h + data[i] * P5) & M, 11) * P1 & M
        i += 1
    h = (h ^ (h >> 15)) * P2 & M
    h = (h ^ (h >> 13)) * P3 & M
    return h ^ (h >> 16)


def lz4_stored(data):
    """An lz4 frame of uncompressed blocks (valid input for lz4 -d and liblz4)."""
    desc = bytes([0x60, 0x70])               # version 1, independent blocks; 4 MB blocks
    o = b"\x04\x22\x4d\x18" + desc + bytes([(xxh32(desc) >> 8) & 0xFF])
    for i in range(0, len(data), 4 << 20):
        blk = data[i:i + (4 << 20)]
        o += struct.pack("<I", len(blk) | 0x80000000) + blk
    return o + b"\0\0\0\0"


def compress(src, tool):
    dst = src + (".zst" if tool == "zstd" else ".lz4")
    if shutil.which(tool):
        with open(p(src), "rb") as i, open(p(dst), "wb") as o:
            subprocess.run([tool, "-q", "-c"], stdin=i, stdout=o, check=True)
        return
    data = open(p(src), "rb").read()
    if tool == "zstd":
        from compression import zstd
        data = zstd.compress(data)
    else:
        data = lz4_stored(data)
    open(p(dst), "wb").write(data)


compress("se.fq", "zstd")
for f in ("se.fq", "m1.fq", "m2.fq"):
    compress(f, "lz4")


# Unaligned BAM (BGZF blocks + EOF marker), records in file order.
def bam(name, records):
    body = b"BAM\1" + struct.pack("<i", 0) + struct.pack("<i", 0)
    nt = {c: i for i, c in enumerate("=ACMGRSVTWYHKDBN")}
    for rn, flag, seq, ql in records:
        s = bytes((nt[seq[i]] << 4) | (nt[seq[i + 1]] if i + 1 < len(seq) else 0) for i in range(0, len(seq), 2))
        rec = struct.pack("<iiBBHHHIiii", -1, -1, len(rn) + 1, 0, 4680, 0, flag, len(seq), -1, -1, 0)
        rec += rn.encode() + b"\0" + s + bytes(ord(c) - 33 for c in ql)
        body += struct.pack("<i", len(rec)) + rec
    o = b""
    for i in range(0, len(body), 60000):
        c = zlib.compressobj(6, zlib.DEFLATED, -15)
        d = c.compress(body[i:i + 60000]) + c.flush()
        blk = body[i:i + 60000]
        o += (b"\x1f\x8b\x08\x04\0\0\0\0\0\xff\x06\0BC\x02\0" + struct.pack("<H", len(d) + 25) + d +
              struct.pack("<II", zlib.crc32(blk), len(blk)))
    o += bytes.fromhex("1f8b08040000000000ff0600424302001b0003000000000000000000")
    open(p(name), "wb").write(o)


bam("se.bam", [(n.split()[0], 4, s, qual(len(s))) for n, s in se])
bam("pe.bam", [r for n, a, b in pairs for r in ((n, 77, a, qual(100)), (n, 141, b, qual(100)))])
print("test data in", out)
