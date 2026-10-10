/*
 * bowtie2-crossplatform: this file is a bowtie2-crossplatform addition (not in
 * upstream Bowtie 2).
 *
 * bowtie2_launcher.c - native versions of the `bowtie2`, `bowtie2-build` and
 * `bowtie2-inspect` wrapper scripts (upstream: Perl and Python).
 *
 * On Windows a script is not a runnable command (cmd, Explorer and other
 * programs only start .exe files), so the Makefile builds these launchers there
 * instead of upstream's .bat files. One source, compiled once per tool:
 *   -DLAUNCH_ALIGN    bowtie2          port of the Perl `bowtie2`
 *                                      (links zlib, libbz2, libzstd, liblz4)
 *   -DLAUNCH_BUILD    bowtie2-build    port of the Python `bowtie2-build` (links zlib)
 *   -DLAUNCH_INSPECT  bowtie2-inspect  port of the Python `bowtie2-inspect`
 * As with the scripts, the bowtie2-*-s/-l binaries must sit next to the
 * launcher. Each launcher runs the command the script would run and prints the
 * script's messages; the few deliberate differences are listed in README.md.
 * The code is portable so it can be compared with the scripts on Linux/macOS.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define EXE ".exe"
#define SEP '\\'
#define PATHSEP ';'
typedef struct _stat64 stat_t;
#define stat_fn _stat64
#else
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define EXE ""
#define SEP '/'
#define PATHSEP ':'
typedef struct stat stat_t;
#define stat_fn stat
#endif

#if !defined(LAUNCH_ALIGN) && !defined(LAUNCH_BUILD) && !defined(LAUNCH_INSPECT)
#error "define LAUNCH_ALIGN, LAUNCH_BUILD or LAUNCH_INSPECT"
#endif

/* ---- small helpers ------------------------------------------------------ */

static void oom(void) { fputs("bowtie2: out of memory\n", stderr); exit(1); }
static void *xrealloc(void *p, size_t n) { if (!(p = realloc(p, n ? n : 1))) oom(); return p; }
static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; return memcpy(xrealloc(NULL, n), s, n); }

static char *fmt(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int n = vsnprintf(NULL, 0, f, ap);
	va_end(ap);
	char *s = xrealloc(NULL, (size_t)n + 1);
	va_start(ap, f);
	vsnprintf(s, (size_t)n + 1, f, ap);
	va_end(ap);
	return s;
}

/* A growable, NULL-terminated string list (argument vectors). */
typedef struct { char **v; int n, cap; } list;

static void push(list *l, const char *s)
{
	if (l->n + 2 > l->cap) {
		l->cap = l->cap ? 2 * l->cap : 16;
		l->v = xrealloc(l->v, (size_t)l->cap * sizeof *l->v);
	}
	l->v[l->n++] = xstrdup(s);
	l->v[l->n] = NULL;
}

static char *join(char *const *v, int n, const char *sep)
{
	size_t len = 1;
	for (int i = 0; i < n; i++) len += strlen(v[i]) + strlen(sep);
	char *s = xrealloc(NULL, len);
	*s = 0;
	for (int i = 0; i < n; i++) {
		if (i) strcat(s, sep);
		strcat(s, v[i]);
	}
	return s;
}

#ifdef LAUNCH_ALIGN
static int is_file(const char *p) { stat_t st; return stat_fn(p, &st) == 0 && S_ISREG(st.st_mode); }
#else
static int exists(const char *p) { stat_t st; return stat_fn(p, &st) == 0; }
#endif

static char *last_sep(const char *p)
{
	char *s = strrchr(p, '/');
#ifdef _WIN32
	char *b = strrchr(p, '\\');
	if (!s || (b && b > s)) s = b;
#endif
	return s;
}

/* Directory holding this launcher, with a trailing separator. */
static char *self_dir(const char *argv0)
{
	char *path = NULL;
#ifdef _WIN32
	static char buf[32768];
	DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
	(void)argv0;
	if (n && n < sizeof buf) path = xstrdup(buf);
#else
	char buf[PATH_MAX];
	if (strchr(argv0, '/')) {
		if (realpath(argv0, buf)) path = xstrdup(buf);
	} else {                                 /* found through PATH */
		char *dirs = xstrdup(getenv("PATH") ? getenv("PATH") : "");
		for (char *d = strtok(dirs, ":"); d && !path; d = strtok(NULL, ":")) {
			char *cand = fmt("%s/%s", d, argv0);
			if (access(cand, X_OK) == 0 && realpath(cand, buf)) path = xstrdup(buf);
			free(cand);
		}
		free(dirs);
	}
#endif
	if (!path || !last_sep(path)) {
		fputs("bowtie2: cannot locate the launcher's own directory\n", stderr);
		exit(1);
	}
	last_sep(path)[1] = 0;
	return path;
}

/* LF-only stdout/stderr on Windows, like the patched binaries' _setmode(). */
static void binary_stdio(void)
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);
#endif
}

/* ---- running a binary ----------------------------------------------------
 * start() runs `prog` with `argv` (argv[0] is the name the child sees). The
 * child reads fd `in` and writes fd `out` (-1: ours); with `pipe_out`, its
 * stdout comes back as a binary stream instead. It shares our stderr.
 * finish() returns the exit code, or -1 with *sig set if the child was killed
 * by a signal (POSIX only). */

#ifdef _WIN32
typedef intptr_t child;

/* Quote one argument so the child's C runtime parses it back unchanged. */
static char *win_quote(const char *a)
{
	if (*a && !strpbrk(a, " \t\n\v\"")) return xstrdup(a);
	char *o = xrealloc(NULL, 2 * strlen(a) + 3), *p = o;
	*p++ = '"';
	for (const char *s = a;; s++) {
		size_t bs = 0;
		while (*s == '\\') { bs++; s++; }
		if (!*s) { memset(p, '\\', 2 * bs); p += 2 * bs; break; }
		if (*s == '"') { memset(p, '\\', 2 * bs + 1); p += 2 * bs + 1; }
		else { memset(p, '\\', bs); p += bs; }
		*p++ = *s;
	}
	*p++ = '"';
	*p = 0;
	return o;
}

/* If the launcher is killed (TerminateProcess, e.g. a GUI's Cancel), the
 * binary must not keep running and writing output. The launcher joins a job
 * object that kills its members when its last handle closes; that handle is
 * held (not inherited) by the launcher alone, and the binary started later
 * is in the job from its first instruction. Ctrl-C is unaffected. If any step
 * fails, the binary simply runs outside a job, as before. */
static void kill_with_launcher(void)
{
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
	static int done;
	HANDLE job;
	if (done++ || !(job = CreateJobObjectA(NULL, NULL))) return;
	memset(&li, 0, sizeof li);
	li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) ||
	    !AssignProcessToJobObject(job, GetCurrentProcess()))
		CloseHandle(job);                     /* otherwise kept open until we exit */
}

/* MSVC docs' _pipe + _dup2 + _spawnv pattern: the child inherits fds 0 and 1. */
static int start(const char *prog, char **argv, int in, int out, FILE **pipe_out, child *c)
{
	list q = {0};
	kill_with_launcher();
	for (int i = 0; argv[i]; i++) {
		char *s = win_quote(argv[i]);
		push(&q, s);
		free(s);
	}
	int fds[2], saved0 = -1, saved1 = -1;
	fflush(stdout);
	if (pipe_out) {
		if (_pipe(fds, 1 << 16, _O_BINARY | _O_NOINHERIT)) return -1;
		out = fds[1];
	}
	if (out >= 0) { saved1 = _dup(1); _dup2(out, 1); }
	if (in >= 0) { saved0 = _dup(0); _dup2(in, 0); }
	*c = _spawnv(_P_NOWAIT, prog, (const char *const *)q.v);
	int err = errno;
	if (saved1 >= 0) { _dup2(saved1, 1); _close(saved1); }
	if (saved0 >= 0) { _dup2(saved0, 0); _close(saved0); }
	if (pipe_out) {
		_close(fds[1]);
		if (*c == -1) _close(fds[0]);
		else *pipe_out = _fdopen(fds[0], "rb");
	}
	errno = err;
	return *c == -1 ? -1 : 0;
}

static int finish(child *c, int *sig)          /* a crash gives a negative NTSTATUS */
{
	int st;
	*sig = 0;
	return _cwait(&st, *c, 0) == -1 ? 1 : st;
}

#ifdef LAUNCH_ALIGN
static int make_pipe(int fds[2]) { return _pipe(fds, 1 << 16, _O_BINARY | _O_NOINHERIT); }
#endif
#else
typedef pid_t child;

static int start(const char *prog, char **argv, int in, int out, FILE **pipe_out, child *c)
{
	int fds[2];
	if (pipe_out && pipe(fds)) return -1;
	fflush(stdout);
	fflush(stderr);
	if ((*c = fork()) < 0) return -1;
	if (*c == 0) {
		if (pipe_out) { dup2(fds[1], 1); close(fds[0]); close(fds[1]); }
		else if (out >= 0) dup2(out, 1);
		if (in >= 0) dup2(in, 0);
		execv(prog, argv);
		fprintf(stderr, "bowtie2: cannot run '%s': %s\n", prog, strerror(errno));
		_exit(127);
	}
	if (pipe_out) {
		close(fds[1]);
		fcntl(fds[0], F_SETFD, FD_CLOEXEC);  /* not inherited by later children */
		*pipe_out = fdopen(fds[0], "r");
	}
	return 0;
}

static int finish(child *c, int *sig)
{
	int st;
	*sig = 0;
	while (waitpid(*c, &st, 0) < 0)
		if (errno != EINTR) return 1;
	if (WIFSIGNALED(st)) { *sig = WTERMSIG(st); return -1; }
	return WEXITSTATUS(st);
}

#ifdef LAUNCH_ALIGN
static int make_pipe(int fds[2])
{
	if (pipe(fds)) return -1;
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	return 0;
}
#endif
#endif

/* ======================================================================== */
#ifdef LAUNCH_ALIGN
/* Port of the Perl `bowtie2` wrapper. */

#include <bzlib.h>
#include <lz4frame.h>
#include <zlib.h>
#include <zstd.h>

static int verbose, keep;
static list tmpfiles;                          /* decompressed .lz4 reads */

static int is_dir(const char *p) { stat_t st; return stat_fn(p, &st) == 0 && S_ISDIR(st.st_mode); }

static int ends_with(const char *s, const char *suf)
{
	size_t n = strlen(s), k = strlen(suf);
	return n >= k && !strcmp(s + n - k, suf);
}

/* Perl's Info(): "(INFO): ..." on stderr when --verbose is given. */
static void info(const char *f, ...)
{
	if (!verbose) return;
	va_list ap;
	fputs("(INFO): ", stderr);
	va_start(ap, f);
	vfprintf(stderr, f, ap);
	va_end(ap);
}

/* Perl's Fail(): "(ERR): ...", then die's message and exit status. */
static void fail(int code, const char *f, ...)
{
	va_list ap;
	fputs("(ERR): ", stderr);
	va_start(ap, f);
	vfprintf(stderr, f, ap);
	va_end(ap);
	fputs("Exiting now ...\n", stderr);
	exit(code);
}

static void remove_tmpfiles(void)
{
	if (!keep)
		for (int i = 0; i < tmpfiles.n; i++) remove(tmpfiles.v[i]);
}

/* ---- --un/--al/--un-conc/--al-conc/--un-mates outputs, and .lz4 reads ---- */

enum { PLAIN, GZ, BZ2, LZ4, ZST };
static const char *comp_ext[] = {"", "-gz", "-bz2", "-lz4", "-zst"};

typedef struct {
	FILE *f;
	gzFile gz;
	BZFILE *bz;
	ZSTD_CCtx *zs;
	LZ4F_cctx *lz;
	char *buf;
	size_t cap;
	const char *opt, *path;
} sink;

static int sink_open(sink *s, const char *path, int comp)
{
	memset(s, 0, sizeof *s);
	s->path = path;
	if (comp == GZ) return (s->gz = gzopen(path, "wb")) != NULL;
	if (!(s->f = fopen(path, "wb"))) return 0;
	int err = BZ_OK;
	if (comp == BZ2) s->bz = BZ2_bzWriteOpen(&err, s->f, 9, 0, 0);
	if (comp == ZST) s->zs = ZSTD_createCCtx();
	if (comp == ZST) s->buf = xrealloc(NULL, s->cap = ZSTD_CStreamOutSize());
	if (comp == LZ4) {                       /* frame format, as the lz4 program writes */
		LZ4F_preferences_t pr;
		memset(&pr, 0, sizeof pr);
		pr.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
		s->buf = xrealloc(NULL, s->cap = LZ4F_compressBound(1 << 16, &pr) + LZ4F_HEADER_SIZE_MAX);
		size_t n = 0;
		if (LZ4F_isError(LZ4F_createCompressionContext(&s->lz, LZ4F_VERSION)) ||
		    LZ4F_isError(n = LZ4F_compressBegin(s->lz, s->buf, s->cap, &pr)) || fwrite(s->buf, 1, n, s->f) != n)
			err = -1;
	}
	if (err != BZ_OK || (comp == ZST && !s->zs)) { fclose(s->f); return 0; }
	return 1;
}

/* A failed write or close (e.g. a full disk) ends the run, as a truncated
 * output must not look like a success. */
static void write_failed(const char *what, const char *path, int err)
{
	fail(err ? err : 255, "Could not write %s '%s': %s\n", what, path, err ? strerror(err) : "write error");
}

static void sink_failed(sink *s)
{
	char *what = fmt("--%s output file", s->opt);
	write_failed(what, s->path, errno);
}

/* zstd: compress (or with `end`, finish) and write what comes out. */
static void zs_write(sink *s, const char *p, size_t n, int end)
{
	ZSTD_inBuffer in = {p, n, 0};
	for (;;) {
		ZSTD_outBuffer out = {s->buf, s->cap, 0};
		size_t left = ZSTD_compressStream2(s->zs, &out, &in, end ? ZSTD_e_end : ZSTD_e_continue);
		if (ZSTD_isError(left) || fwrite(s->buf, 1, out.pos, s->f) != out.pos) sink_failed(s);
		if (end ? left == 0 : in.pos == in.size) break;
	}
}

static void sink_write(sink *s, const char *p, size_t n)
{
	int err;
	errno = 0;
	if (s->gz) { if (n && gzwrite(s->gz, p, (unsigned)n) <= 0) sink_failed(s); }
	else if (s->bz) { BZ2_bzWrite(&err, s->bz, (void *)p, (int)n); if (err != BZ_OK) sink_failed(s); }
	else if (s->zs) zs_write(s, p, n, 0);
	else if (s->lz) {
		for (size_t k; n; p += k, n -= k) {
			k = n < (1 << 16) ? n : (1 << 16);
			size_t m = LZ4F_compressUpdate(s->lz, s->buf, s->cap, p, k, NULL);
			if (LZ4F_isError(m) || fwrite(s->buf, 1, m, s->f) != m) sink_failed(s);
		}
	} else if (fwrite(p, 1, n, s->f) != n) sink_failed(s);
}

static void sink_close(sink *s)
{
	int err = BZ_OK, bad = 0;
	errno = 0;
	if (s->gz) bad = gzclose(s->gz) != Z_OK;
	if (s->bz) { BZ2_bzWriteClose(&err, s->bz, 0, NULL, NULL); bad = err != BZ_OK; }
	if (s->zs) { zs_write(s, "", 0, 1); ZSTD_freeCCtx(s->zs); }
	if (s->lz) {
		size_t m = LZ4F_compressEnd(s->lz, s->buf, s->cap, NULL);
		bad = LZ4F_isError(m) || fwrite(s->buf, 1, m, s->f) != m;
		LZ4F_freeCompressionContext(s->lz);
	}
	if (s->f) bad |= fclose(s->f) != 0;
	if (bad) sink_failed(s);
}

/* Decompress the .lz4 read file `src` into `dst` (concatenated frames, as lz4 -dc). */
static void lz4_unpack(const char *src, const char *dst)
{
	FILE *in = fopen(src, "rb"), *out;
	if (!in) fail(errno ? errno : 255, "Could not open lz4ed read file: %s \n", src);
	if (!(out = fopen(dst, "wb"))) fail(errno ? errno : 255, "Can't open '%s' for writing.\n", dst);
	LZ4F_dctx *d;
	static char ib[1 << 16], ob[1 << 16];
	size_t hint = 1, got = 0;                 /* hint: 0 at a frame boundary */
	int bad = LZ4F_isError(LZ4F_createDecompressionContext(&d, LZ4F_VERSION));
	while (!bad && (got = fread(ib, 1, sizeof ib, in)) > 0) {
		for (size_t pos = 0; pos < got && !bad;) {
			size_t isz = got - pos, osz = sizeof ob;
			hint = LZ4F_decompress(d, ob, &osz, ib + pos, &isz, NULL);
			bad = LZ4F_isError(hint) || fwrite(ob, 1, osz, out) != osz;
			pos += isz;
		}
	}
	while (!bad && hint) {                    /* flush what the last call kept back */
		size_t isz = 0, osz = sizeof ob;
		hint = LZ4F_decompress(d, ob, &osz, NULL, &isz, NULL);
		if (LZ4F_isError(hint) || !osz) break;
		bad = fwrite(ob, 1, osz, out) != osz;
	}
	LZ4F_freeDecompressionContext(d);
	fclose(in);
	if (fclose(out) || bad || hint) fail(1, "Could not decompress lz4ed read file: %s \n", src);
}

/* --un etc.: the read files given and how they are written. */
enum { AL, UN, AL_CONC, UN_CONC, UN_MATES, NREAD };
static const char *read_opt[NREAD] = {"al", "un", "al-conc", "un-conc", "un-mates"};

/* Line reader for the binary's output: binary-safe, as the passthrough line
 * of a BAM read is the raw record. */
typedef struct { FILE *f; char b[1 << 16]; size_t pos, len; } reader;

/* Next line without its '\n' (*n bytes, NUL-terminated); 0 at the end. */
static int get_line(reader *r, char **buf, size_t *cap, size_t *n)
{
	int got = 0;
	if (!*buf) *buf = xrealloc(NULL, *cap = 1 << 16);
	for (*n = 0;;) {
		if (r->pos == r->len && !(r->pos = 0, r->len = fread(r->b, 1, sizeof r->b, r->f))) break;
		char *s = r->b + r->pos, *nl = memchr(s, '\n', r->len - r->pos);
		size_t k = nl ? (size_t)(nl - s) : r->len - r->pos;
		if (*n + k + 1 > *cap) *buf = xrealloc(*buf, *cap = 2 * (*n + k + 1));
		memcpy(*buf + *n, s, k);
		*n += k;
		r->pos += k + (nl != NULL);
		got = 1;
		if (nl) break;
	}
	(*buf)[*n] = 0;
	return got;
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* --passthrough read line: the read with \n, \r and % escaped as %XX. */
static size_t unescape(const char *s, size_t n, char *o)
{
	char *p = o;
	for (size_t i = 0; i < n;) {
		int hi = i + 2 < n ? hexval(s[i + 1]) : -1, lo = hi < 0 ? -1 : hexval(s[i + 2]);
		if (s[i] == '%' && lo >= 0) {
			*p++ = (char)(hi * 16 + lo);
			i += 3;
		} else *p++ = s[i++];
	}
	return (size_t)(p - o);
}

/* File::Spec->catpath(): `file` inside `dir`. */
static char *in_dir(const char *dir, const char *file)
{
	size_t n = strlen(dir);
	if (!n) return xstrdup(file);
	int sep = dir[n - 1] == '/' || dir[n - 1] == SEP;
	return fmt("%s%s%s", dir, sep ? "" : (char[]){SEP, 0}, file);
}

/* glob("<base>*.bt2{,l}") finds something: a file in base's directory whose
 * name starts with base's last component and ends in .bt2 or .bt2l. */
static int index_files(const char *base)
{
	const char *sep = last_sep(base), *pre = sep ? sep + 1 : base;
	char *dir = sep ? fmt("%.*s", (int)(sep - base + 1), base) : xstrdup("");
	size_t np = strlen(pre);
	int found = 0;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	char *pat = fmt("%s*", dir);
	HANDLE h = FindFirstFileA(pat, &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const char *e = fd.cFileName;
#else
	DIR *d = opendir(*dir ? dir : ".");
	struct dirent *de;
	if (d) {
		while (!found && (de = readdir(d))) {
			const char *e = de->d_name;
#endif
			size_t ne = strlen(e);
			if ((np || e[0] != '.') && !strncmp(e, pre, np) &&
			    ((ne >= np + 4 && ends_with(e, ".bt2")) || (ne >= np + 5 && ends_with(e, ".bt2l"))))
				found = 1;
#ifdef _WIN32
		} while (!found && FindNextFileA(h, &fd));
		FindClose(h);
	}
	free(pat);
#else
		}
		closedir(d);
	}
#endif
	free(dir);
	return found;
}

/* The wrapper options of the script's GetOptions() call (configured with
 * pass_through, no_ignore_case, no_auto_abbrev, bundling_values). */
enum { FLAG, STR, MULTI };
enum { W_1, W_2, W_U, W_TAB5, W_TAB6, W_IL, W_B, NLIST, W_TEMP = NLIST, W_BAM, W_PIPES, W_REF, W_KEEP,
       W_VERBOSE, W_DEBUG, W_SAN, W_LARGE, W_NOUNAL, W_LOG, W_READS };
static const struct { const char *name; int id, type; } wopt[] = {
	{"1", W_1, MULTI}, {"2", W_2, MULTI}, {"reads", W_U, MULTI}, {"U", W_U, MULTI},
	{"tab5", W_TAB5, MULTI}, {"tab6", W_TAB6, MULTI}, {"interleaved", W_IL, MULTI}, {"b", W_B, MULTI},
	{"temp-directory", W_TEMP, STR}, {"bam", W_BAM, FLAG}, {"no-named-pipes", W_PIPES, FLAG},
	{"ref-string", W_REF, STR}, {"reference-string", W_REF, STR}, {"keep", W_KEEP, FLAG},
	{"verbose", W_VERBOSE, FLAG}, {"debug", W_DEBUG, FLAG}, {"sanitized", W_SAN, FLAG},
	{"large-index", W_LARGE, FLAG}, {"no-unal", W_NOUNAL, FLAG}, {"log-file", W_LOG, STR}};
#define NWOPT (int)(sizeof wopt / sizeof *wopt)

/* Option `name` (n chars): its wopt index, NWOPT + 5*k + compression for
 * --al/--un/... (k = AL...), or -1. */
static int wopt_find(const char *name, size_t n)
{
	for (int i = 0; i < NWOPT; i++)
		if (strlen(wopt[i].name) == n && !strncmp(wopt[i].name, name, n)) return i;
	for (int k = 0; k < NREAD; k++)
		for (int c = 0; c < 5; c++) {
			char *o = fmt("%s%s", read_opt[k], comp_ext[c]);
			int hit = strlen(o) == n && !strncmp(o, name, n);
			free(o);
			if (hit) return NWOPT + 5 * k + c;
		}
	return -1;
}

/* A further value for a multi-value option: not an option or `--` ("-" is). */
static int more_value(const char *s)
{
	return !strcmp(s, "-") || !((s[0] == '-' || s[0] == '+') && s[1]);
}

/* Perl's maybe_wrap_input(): read files ending in .bz2 or .lz4 move to the
 * end of the list (for pairs: both mates, if either does). The script feeds
 * them through a named pipe; here .bz2 files go to the binary as they are
 * (it reads them natively) and .lz4 files are decompressed to temporary
 * files (`unpack` collects source/target pairs). */
static int wrapped(const char *f) { return ends_with(f, ".bz2") || ends_with(f, ".lz4"); }

static void wrap_input(list *m1, list *m2, const char *ext, const char *tmp, list *unpack)
{
	list k1 = {0}, k2 = {0}, w1 = {0}, w2 = {0};
	for (int i = 0; i < m1->n; i++) {
		const char *b = m2 && i < m2->n ? m2->v[i] : "";
		int keep_ = !wrapped(m1->v[i]) && (!m2 || (strcmp(m1->v[i], "") && strcmp(m1->v[i], "0") && !wrapped(b)));
		push(keep_ ? &k1 : &w1, m1->v[i]);
		if (m2) push(keep_ ? &k2 : &w2, b);
	}
	list *ks[2] = {&k1, &k2}, *ws[2] = {&w1, &w2};
	for (int m = 0; m < (m2 ? 2 : 1); m++)
		for (int i = 0; i < ws[m]->n; i++) {
			const char *f = ws[m]->v[i];
			if (ends_with(f, ".lz4")) {
#ifdef _WIN32
				int pid = (int)GetCurrentProcessId();
#else
				int pid = (int)getpid();
#endif
				char *t = fmt("%s%cbowtie2_%d_%d.%s", tmp, SEP, pid, unpack->n / 2 + 1, ext);
				push(unpack, f);
				push(unpack, t);
				push(ks[m], t);
			} else push(ks[m], f);
		}
	*m1 = k1;
	if (m2) *m2 = k2;
}

/* samtools for --bam: next to the launcher, else on PATH. */
static char *find_samtools(const char *dir)
{
	char *p = fmt("%ssamtools" EXE, dir);
	if (is_file(p)) return p;
	char *dirs = xstrdup(getenv("PATH") ? getenv("PATH") : ""), *d = dirs;
	for (char *e; d; d = e ? e + 1 : NULL) {
		if ((e = strchr(d, PATHSEP))) *e = 0;
		p = fmt("%s%c" "samtools" EXE, *d ? d : ".", SEP);
#ifdef _WIN32
		if (is_file(p)) return p;
#else
		if (is_file(p) && access(p, X_OK) == 0) return p;
#endif
		free(p);
	}
	free(dirs);
	return NULL;
}

int main(int argc, char **argv)
{
	binary_stdio();
	char *dir = self_dir(argv[0]);
	char *prog_s = fmt("%sbowtie2-align-s" EXE, dir), *prog_l = fmt("%sbowtie2-align-l" EXE, dir);
#ifdef _WIN32
	if (!is_file(prog_s))
#else
	if (access(prog_s, X_OK))
#endif
		fail(2, "Expected bowtie2 to be in same directory with bowtie2-align:\n%s\n", dir);

	/* The script's GetOptions(): a wrapper option is -x (one letter, value
	 * attached or next), --name, +name or --name=value; multi-value options
	 * take further values up to the next option. Everything else, and all
	 * from `--` on, stays for the binary in its order. */
	list args = {0}, in[NLIST] = {{0}};
	const char *temp_dir = NULL, *log_fn = NULL, *ref_str = NULL;
	int bam_out = 0, debug = 0, sanitized = 0, large = 0, no_unal = 0, nread = 0;
	char *read_fn[NREAD] = {0};
	int read_comp[NREAD] = {0};
	for (int i = 1; i < argc; i++) {
		const char *t = argv[i];
		if (!strcmp(t, "--")) {
			for (; i < argc; i++) push(&args, argv[i]);
			break;
		}
		size_t pre = t[0] == '-' ? 1 + (t[1] == '-') : t[0] == '+';
		const char *name = t + pre, *eq = *name ? strchr(name + 1, '=') : NULL, *optarg = eq ? eq + 1 : NULL;
		size_t n = eq ? (size_t)(eq - name) : strlen(name);
		if (t[0] == '-' && pre == 1) {       /* bundling_values: -U<value>; any =value is lost */
			optarg = n > 1 ? fmt("%.*s", (int)(n - 1), name + 1) : NULL;
			n = n ? 1 : 0;
		}
		int o = pre && strcmp(t, "-") ? wopt_find(name, n) : -1;
		int type = o < 0 ? -1 : o >= NWOPT ? STR : wopt[o].type;
		if (type < 0 || (type == FLAG && optarg) || (type != FLAG && (optarg ? !*optarg : i + 1 >= argc))) {
			push(&args, t);
			continue;
		}
		const char *val = type == FLAG ? NULL : optarg ? optarg : argv[++i];
		if (o >= NWOPT) {
			int k = (o - NWOPT) / 5;
			nread += !read_fn[k];
			read_fn[k] = xstrdup(val);
			read_comp[k] = (o - NWOPT) % 5;
			continue;
		}
		switch (wopt[o].id) {
		case W_TEMP: temp_dir = val; break;
		case W_BAM: bam_out = 1; break;
		case W_REF: ref_str = val; break;
		case W_KEEP: keep = 1; break;
		case W_VERBOSE: verbose = 1; break;
		case W_DEBUG: debug = 1; break;
		case W_SAN: sanitized = 1; break;
		case W_LARGE: large = 1; break;
		case W_NOUNAL: no_unal = 1; break;
		case W_LOG: log_fn = val; break;
		case W_PIPES: break;                 /* .lz4 reads always go through temporary files */
		default:
			push(&in[wopt[o].id], val);
			while (i + 1 < argc && more_value(argv[i + 1])) push(&in[wopt[o].id], argv[++i]);
		}
	}
	atexit(remove_tmpfiles);

	if (verbose) push(&args, "--verbose");
	/* Read redirection, --no-unal or --bam: the binary emits each read after
	 * its SAM record (--passthrough) and the wrapper writes SAM to -S itself. */
	int passthru = nread || no_unal || bam_out;
	const char *cap_out = NULL;
	if (passthru) {
		list kept = {0};
		push(&args, "--passthrough");
		cap_out = "-";
		for (int i = 0; i < args.n; i++) {
			const char *a = args.v[i];
			if (!strcmp(a, "-S") || !strcmp(a, "--output")) {
				if (i + 1 >= args.n) fail(255, "-S/--output takes an argument.\n");
				cap_out = args.v[++i];
			} else if (!strncmp(a, "-S", 2) && a[2]) cap_out = a + 2;
			else if (!strncmp(a, "--output=", 9)) cap_out = a + 9;
			else push(&kept, a);
		}
		args = kept;
	}

	if (log_fn) {                            /* stderr (also the binary's) goes to the log file */
		fflush(stderr);
#ifdef _WIN32
		int fd = _open(log_fn, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
		if (fd < 0 || _dup2(fd, 2)) fail(errno ? errno : 255, "Cannot redirect to log file %s.\n", log_fn);
		_close(fd);
#else
		int fd = open(log_fn, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0 || dup2(fd, 2) < 0) fail(errno ? errno : 255, "Cannot redirect to log file %s.\n", log_fn);
		close(fd);
#endif
	}

	/* Temporary files: --temp-directory, else File::Spec->tmpdir(). */
	char *tmp;
#ifdef _WIN32
	static char tbuf[MAX_PATH + 1];
	DWORD tn = GetTempPathA(sizeof tbuf, tbuf);
	if (tn > 1 && (tbuf[tn - 1] == '\\' || tbuf[tn - 1] == '/')) tbuf[tn - 1] = 0;
	tmp = xstrdup(temp_dir ? temp_dir : tn ? tbuf : ".");
#else
	const char *te = getenv("TMPDIR");
	tmp = xstrdup(temp_dir ? temp_dir : te && is_dir(te) && access(te, W_OK) == 0 ? te : "/tmp");
	for (size_t n = strlen(tmp); !temp_dir && n > 1 && tmp[n - 1] == '/'; n--) tmp[n - 1] = 0;
#endif

	list unpack = {0};
	static const char *lopt[NLIST] = {"-1", "-2", "-U", "--tab5", "--tab6", "--interleaved", "-b"};
	static const char *lext[NLIST] = {"mate", "", "unps", "tab5", "tab6", "int", "bam"};
	for (int k = 0; k < NLIST; k++) {
		if (k == W_1 || k == W_2) {          /* mates only with -2, as in the script */
			if (k == W_2 || !in[W_2].n) continue;
			wrap_input(&in[W_1], &in[W_2], "mate", tmp, &unpack);
			for (int m = W_1; m <= W_2; m++) {
				char *j = join(in[m].v, in[m].n, ",");
				push(&args, lopt[m]);
				push(&args, j);
				free(j);
			}
		} else if (in[k].n) {
			wrap_input(&in[k], NULL, lext[k], tmp, &unpack);
			char *j = join(in[k].v, in[k].n, ",");
			push(&args, lopt[k]);
			push(&args, j);
			free(j);
		}
	}
	if (ref_str) fail(1, "--ref-string / --reference-string is not supported by this launcher; build an index with bowtie2-build\n");

	char *b = join(args.v, args.n, " ");
	info("After arg handling:\n");
	info("  Binary args:\n[ %s ]\n", b);

	/* Index name (-x), for choosing the small or large binary. */
	char *idx = NULL;
	for (int i = 0; i < args.n && !idx; i++)
		if (!strcmp(args.v[i], "-x")) idx = xstrdup(i + 1 < args.n ? args.v[i + 1] : "");
	if (idx) {
		const char *env = getenv("BOWTIE2_INDEXES");
		if (!index_files(idx)) {
			char *alt = env ? in_dir(env, idx) : NULL;
			if (!alt || !index_files(alt)) fail(255, "\"%s\" does not exist or is not a Bowtie 2 index\n", idx);
			idx = alt;
		}
	} else {                                 /* the script then uses Info()'s return value */
		info("Cannot find any index option (--reference-string, --ref-string or -x) in the given command line.\n");
		idx = verbose ? "1" : "0";
	}

	const char *prog = prog_s;
	char *large_idx = fmt("%s.1.bt2l", idx), *small_idx = fmt("%s.1.bt2", idx);
	if (large) {
		info("Using a large index enforced by user.\n");
		prog = prog_l;
		if (!is_file(large_idx)) fail(2, "Cannot find the large index %s\n", large_idx);
		info("Using large index (%s).\n", large_idx);
	} else if (is_file(large_idx) && !is_file(small_idx)) {
		info("Cannot find a small index but a large one seems to be present.\n");
		info("Switching to using the large index (%s).\n", large_idx);
		prog = prog_l;
	} else {
		info("Using the small index (%s).\n", small_idx);
	}
	if (debug || sanitized)
		prog = fmt("%.*s%s" EXE, (int)(strlen(prog) - strlen(EXE)), prog, debug ? "-debug" : "-sanitized");

	/* The script's command line, with its quoting of some option values. */
	if (verbose) {
		static const char *quoted[] = {"-S", "-U", "-1", "-2", "-x", "-b", "--interleaved", "--rg", "--rg-id",
		                               "--tab5", "--tab6", "--sam-acc", "--bam", NULL};
		list q = {0};
		for (int i = 0, quote = 0; i < args.n; i++) {
			char *a = quote ? fmt("\"%s\"", args.v[i]) : xstrdup(args.v[i]);
			push(&q, a);
			free(a);
			int was = quote;
			quote = 0;
			for (int k = 0; !was && quoted[k]; k++) quote |= !strcmp(args.v[i], quoted[k]);
		}
		info("\"%s\" --wrapper basic-0 %s\n", prog, join(q.v, q.n, " "));
	}

	for (int i = 0; i < unpack.n; i += 2) {
		push(&tmpfiles, unpack.v[i + 1]);
		lz4_unpack(unpack.v[i], unpack.v[i + 1]);
	}

	list cmd = {0};
	push(&cmd, prog);
	push(&cmd, "--wrapper");
	push(&cmd, "basic-0");
	for (int i = 0; i < args.n; i++) push(&cmd, args.v[i]);

	child c, sc;
	FILE *bt = NULL;
	if (start(prog, cmd.v, -1, -1, cap_out ? &bt : NULL, &c)) {
		fprintf(stderr, "(ERR): Failed to execute bowtie2-align: %s\n", strerror(errno));
		return 1;
	}
	if (cap_out) {
		FILE *sam = stdout;
		if (bam_out) {                       /* SAM through `samtools view -b -` */
			char *st = find_samtools(dir);
			if (!st) fail(2, "samtools is needed when outputting BAM");
			int fds[2], out = -1;
			FILE *of = NULL;
			if (strcmp(cap_out, "-") && !(of = fopen(cap_out, "wb")))
				fail(errno ? errno : 255, "Error while writing BAM to file '%s'.\n", cap_out);
			if (of) out = fileno(of);
			char *sargv[] = {"samtools", "view", "-b", "-", NULL};
			if (make_pipe(fds) || start(st, sargv, fds[0], out, NULL, &sc))
				fail(errno ? errno : 255, "Error while writing BAM to %s.\n",
				     of ? fmt("file '%s'", cap_out) : "STDOUT");
			if (of) fclose(of);
#ifdef _WIN32
			_close(fds[0]);
			sam = _fdopen(fds[1], "wb");
#else
			close(fds[0]);
			sam = fdopen(fds[1], "w");
#endif
		} else if (strcmp(cap_out, "-") && !(sam = fopen(cap_out, "wb")))
			fail(errno ? errno : 255, "Could not open output file '%s' for writing.\n", cap_out);

		/* Read files, opened in the script's order; -conc and -mates make a pair. */
		sink out[NREAD][2];
		int open_[NREAD] = {0}, nsink = 0;
		for (int k = 0; k < NREAD; k++) {
			if (!read_fn[k]) continue;
			const char *t = read_fn[k], *sep = last_sep(t);
			char *pdir = sep ? fmt("%.*s", (int)(sep - t + 1), t) : xstrdup("");
			const char *base = sep ? sep + 1 : t;
			if (is_dir(t)) { pdir = xstrdup(t); base = ""; }
			if (k == UN || k == AL) {
				char *fn = *base ? xstrdup(t) : in_dir(pdir, fmt("%s-seqs", read_opt[k]));
				if (!sink_open(&out[k][0], fn, read_comp[k]))
					fail(errno ? errno : 255, "Could not open --%s output file '%s'\n", read_opt[k], t);
				out[k][0].opt = read_opt[k];
			} else {
				char *fn = *base ? xstrdup(base) : fmt("%s-mate", read_opt[k]);
				char *fns[2];
				for (int m = 0; m < 2; m++) {
					char *pct = strchr(fn, '%'), *d = strrchr(fn, '.'), *mf;
					if (pct) {                 /* every '%' becomes the mate number */
						mf = xstrdup(fn);
						for (char *p = mf; *p; p++) if (*p == '%') *p = (char)('1' + m);
					} else if (d) mf = fmt("%.*s.%d%s", (int)(d - fn), fn, m + 1, d);
					else mf = fmt("%s.%d", fn, m + 1);
					fns[m] = in_dir(pdir, mf);
				}
				for (int m = 0; m < 2; m++) {
					if (!sink_open(&out[k][m], fns[m], read_comp[k]))
						fail(errno ? errno : 255, "Could not open --%s mate-%d output file '%s'\n", read_opt[k], m + 1, fns[m]);
					out[k][m].opt = read_opt[k];
				}
			}
			open_[k] = 1;
			nsink++;
		}

		static reader r;
		char *line = NULL, *rd = NULL, *buf = NULL;
		size_t cap = 0, rcap = 0, len, rlen;
		r.f = bt;
		while (get_line(&r, &line, &cap, &len)) {
			int filt = 0;
			if (line[0] != '@') {
				const char *tab = strchr(line, '\t');
				long fl = tab ? strtol(tab + 1, NULL, 10) : 0;
				int unal = (fl & 4) != 0, secondary = (fl & 256) != 0;
				filt = no_unal && unal;
				get_line(&r, &rd, &rcap, &rlen);    /* the read, after each record */
				if (nsink && !secondary) {
					buf = xrealloc(buf, rlen + 1);
					size_t n = unescape(rd, rlen, buf);
					int mate1 = (fl & 64) != 0, mate2 = (fl & 128) != 0, m = mate1 ? 0 : 1;
					if (!mate1 && !mate2) {
						int k = unal ? UN : AL;
						if (open_[k]) sink_write(&out[k][0], buf, n);
					} else {
						int k = (fl & 2) ? AL_CONC : UN_CONC;
						if (open_[k]) sink_write(&out[k][m], buf, n);
						if (unal && open_[UN_MATES]) sink_write(&out[UN_MATES][m], buf, n);
					}
				}
			}
			line[len] = '\n';
			if (!filt && fwrite(line, 1, len + 1, sam) != len + 1)
				write_failed("output file", cap_out, errno);
		}
		for (int k = 0; k < NREAD; k++)
			if (open_[k]) {
				sink_close(&out[k][0]);
				if (k != UN && k != AL) sink_close(&out[k][1]);
			}
		if (sam != stdout ? fclose(sam) != 0 : fflush(stdout) != 0) write_failed("output file", cap_out, errno);
		if (bam_out) { int sig; finish(&sc, &sig); }
		fclose(bt);
	}

	int sig, ret = finish(&c, &sig);
#ifdef _WIN32
	if (ret < 0) {                           /* crashed: report the NTSTATUS, exit with it */
		static const struct { unsigned n; const char *s; } names[] = {
			{0xC0000005u, "access violation"}, {0xC00000FDu, "stack overflow"},
			{0xC0000017u, "out of memory"}, {0xC000001Du, "illegal instruction"},
			{0xC0000094u, "integer division by zero"}, {0xC0000409u, "stack buffer overrun / fast fail"},
			{0xC0000374u, "heap corruption"}, {0xC0000006u, "in-page error"},
			{0xC000013Au, "Ctrl-C"}, {0x80000003u, "breakpoint"},
			{0xC0000135u, "DLL not found"}, {0xC0000139u, "entry point not found"},
			{0xC000007Bu, "invalid image format"}, {0, NULL}};
		const char *nm = "unknown";
		for (int i = 0; names[i].s; i++) if (names[i].n == (unsigned)ret) nm = names[i].s;
		fprintf(stderr, "(ERR): bowtie2-align died with exception 0x%08X (%s)\n", (unsigned)ret, nm);
		return ret;
	}
#else
	if (ret < 0) {
		static const struct { int n; const char *s; } names[] = {
			{SIGHUP, "HUP"}, {SIGINT, "INT"}, {SIGQUIT, "QUIT"}, {SIGILL, "ILL"}, {SIGTRAP, "TRAP"},
			{SIGABRT, "ABRT"}, {SIGFPE, "FPE"}, {SIGKILL, "KILL"}, {SIGBUS, "BUS"}, {SIGSEGV, "SEGV"},
			{SIGPIPE, "PIPE"}, {SIGALRM, "ALRM"}, {SIGTERM, "TERM"}, {SIGUSR1, "USR1"},
			{SIGUSR2, "USR2"}, {SIGXCPU, "XCPU"}, {SIGXFSZ, "XFSZ"}, {0, NULL}};
		const char *nm = "(unknown)";
		for (int i = 0; names[i].s; i++) if (names[i].n == sig) nm = names[i].s;
		fprintf(stderr, "(ERR): bowtie2-align died with signal %d (%s) \n", sig, nm);
		return 1;
	}
#endif
	if (ret) fprintf(stderr, "(ERR): bowtie2-align exited with value %d\n", ret);
	return ret;
}

/* ======================================================================== */
#else
/* Ports of the Python `bowtie2-build` and `bowtie2-inspect` wrappers. */

#ifdef LAUNCH_BUILD
#include <zlib.h>
#define TOOL "bowtie2-build"
#define USAGE "usage: bowtie2-build [--large-index] [--verbose] [--debug | --sanitized]\n"
#else
#define TOOL "bowtie2-inspect"
#define USAGE "usage: bowtie2-inspect [--debug | --sanitized] [--verbose] [--large-index]\n"
#endif

static void usage_error(const char *f, const char *a, const char *b)
{
	fputs(USAGE TOOL ": error: ", stderr);
	fprintf(stderr, f, a, b);
	fputc('\n', stderr);
	exit(2);
}

#ifdef LAUNCH_BUILD
static long long gunzip_size(const char *fn)   /* uncompressed size of a .gz file */
{
	static char buf[1 << 16];
	long long n = 0;
	int k;
	gzFile g = gzopen(fn, "rb");
	while (g && (k = gzread(g, buf, sizeof buf)) > 0) n += k;
	if (g) gzclose(g);
	return n;
}

static int sais_enabled(const char *prog)      /* `<build-s> --version` mentions USE_SAIS */
{
	char *argv[] = {(char *)prog, "--version", NULL}, line[4096];
	FILE *out = NULL;
	child c;
	int sig, found = 0;
	if (start(prog, argv, -1, -1, &out, &c)) return 0;
	while (fgets(line, sizeof line, out)) found |= strstr(line, "USE_SAIS") != NULL;
	fclose(out);
	finish(&c, &sig);
	return found;
}
#endif

int main(int argc, char **argv)
{
	static const char *flag[] = {"--large-index", "--verbose", "--debug", "--sanitized"};
	int set[4] = {0}, dd = 0;
	char *pending = "";                      /* a message Python's stderr holds back */
	list a = {0};
	binary_stdio();
	/* argparse's parse_known_args(): the four options, also as unique
	 * abbreviations; everything else (and all from `--` on) stays. */
	for (int i = 1; i < argc; i++) {
		const char *t = argv[i], *eq = strchr(t, '=');
		size_t n = eq ? (size_t)(eq - t) : strlen(t);
		int o = -1, hits = 0;
		dd |= !strcmp(t, "--");
		for (int k = 0; k < 4 && !dd && n > 2 && !strncmp(t, "--", 2); k++)
			if (!strncmp(flag[k], t, n)) {
				if (strlen(flag[k]) == n) { o = k; hits = 1; break; }
				o = k;
				hits++;
			}
		if (hits != 1) { push(&a, t); continue; }
		if (eq) usage_error("argument %s: ignored explicit argument '%s'", flag[o], eq + 1);
		if (o >= 2 && set[5 - o]) usage_error("argument %s: not allowed with argument %s", flag[o], flag[5 - o]);
		set[o] = 1;
	}
	int large = set[0], verbose = set[1];
	const char *sfx = set[2] ? "-debug" : set[3] ? "-sanitized" : "";
	char *dir = self_dir(argv[0]);
	char *spec = fmt("%s" TOOL "-s%s", dir, sfx), *spec_l = fmt("%s" TOOL "-l%s", dir, sfx);
#ifdef LAUNCH_BUILD
	/* Reference files: the trailing arguments before the index name (up to an
	 * option or a number), joined with commas. Large index above ~4 GiB
	 * (~2 GiB with libsais). */
	list fastas = {0};
	int has_c = 0;
	for (int i = 0; i < a.n; i++) has_c |= !strcmp(a.v[i], "-c");
	if (!has_c && a.n >= 2) {
		int i = a.n - 2;
		for (; i >= 0; i--) {
			const char *s = a.v[i];
			int digits = *s != 0;
			for (const char *p = s; *p; p++) digits &= *p >= '0' && *p <= '9';
			if (s[0] == '-' || digits) break;
		}
		for (int k = i + 1; k < a.n - 1; k++) push(&fastas, a.v[k]);
		if (fastas.n) {
			char *last = a.v[a.n - 1], *j = join(fastas.v, fastas.n, ",");
			a.n = i + 1;
			push(&a, j);
			push(&a, last);
		}
	}
	if (large) spec = spec_l;
	else if (fastas.n) {
		long long total = 0, small_max = 4LL * 1024 * 1024 * 1024 - 200;
		for (int k = 0; k < fastas.n; k++) {
			const char *f = fastas.v[k];
			stat_t st;
			size_t n = strlen(f);
			if (stat_fn(f, &st)) continue;
			if (n >= 3 && !strcmp(f + n - 3, ".gz")) total += gunzip_size(f);
			else if (n >= 4 && !strcmp(f + n - 4, ".zst"))
				pending = fmt("%s%s" TOOL EXE " cannot determine the uncompressed size of ZSTD compressed files."
				        "                        Please make sure that the uncompressed file size is suitable for"
				        "                        building the current index", pending, dir);
			else total += (long long)st.st_size;
		}
		if (total > 2LL * 1024 * 1024 * 1024 - 200 && total <= small_max) {
			char *s = fmt("%s" TOOL "-s", dir);
			if (sais_enabled(s)) small_max = 2LL * 1024 * 1024 * 1024 - 200;
		}
		if (total > small_max) spec = spec_l;
	}
	spec = fmt("%s" EXE, spec);
	if (!exists(spec)) {
		const char *base = last_sep(spec) + 1;
		fprintf(stderr, "%s%s does not exist, try running `[g]make %s'\n", pending, base, base);
		return 1;
	}
#else
	/* Large binary when only the large index exists. */
	if (large) spec = spec_l;
	else if (a.n >= 1) {
		char *l = fmt("%s.1.bt2l", a.v[a.n - 1]), *s = fmt("%s.1.bt2", a.v[a.n - 1]);
		if (exists(l) && !exists(s)) spec = spec_l;
	}
	spec = fmt("%s" EXE, spec);
#endif
	list cmd = {0};
	push(&cmd, spec);
	push(&cmd, "--wrapper");
	push(&cmd, "basic-0");
	for (int i = 0; i < a.n; i++) push(&cmd, a.v[i]);
	/* Python's stderr holds back text without a newline until the next line
	 * (a logged one is flushed at once) or until it exits. */
	if (verbose) fprintf(stderr, "%sINFO: Command: %s\n", pending, join(cmd.v, cmd.n, " "));
	if (verbose) pending = "";

	child c;
	if (start(spec, cmd.v, -1, -1, NULL, &c)) {
		fprintf(stderr, "%s" TOOL ": cannot run '%s': %s\n", pending, spec, strerror(errno));
		return 1;
	}
	int sig, ret = finish(&c, &sig);
	fputs(pending, stderr);
	return ret < 0 ? 256 - sig : ret;        /* as Python's sys.exit(-signal) */
}
#endif
