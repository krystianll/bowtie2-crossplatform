// bowtie2-crossplatform: this file is a bowtie2-crossplatform addition (native
// bzip2 read input, built with WITH_BZIP2=1); see bzip2_reader.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <iostream>
#include <unistd.h>
#if defined(_WIN32) && defined(__GNUC__) && !defined(_WINDOWS_)
// bzlib.h includes <windows.h> on Windows but, with GCC/Clang, uses nothing
// from it; skip it so its macros cannot leak into bowtie2's code.
#define _WINDOWS_
#include <bzlib.h>
#undef _WINDOWS_
#else
#include <bzlib.h>
#endif

#include "bzip2_reader.h"

struct bz2Strm {
	FILE *fp;
	BZFILE *bz;          // NULL after the last stream (fp stays open)
	bool more;           // bz is a follow-on stream that has not produced data yet
	std::string name;    // for error messages
	int back;            // pushed-back character, or EOF
	size_t pos, len;     // read position and fill level of buf
	char buf[1 << 16];
};

// True if fd starts with a bzip2 header: "BZh", a block size '1'-'9', then the
// magic of the first block (or of the end of an empty stream). FASTQ, FASTA
// and the other read formats cannot start like this.
bool bz2IsFile(int fd) {
	static const unsigned char block[6] = {0x31, 0x41, 0x59, 0x26, 0x53, 0x59};
	static const unsigned char end[6] = {0x17, 0x72, 0x45, 0x38, 0x50, 0x90};
	if (fd == -1)
		return false;
	unsigned char b[10];
	ssize_t n = read(fd, b, sizeof(b));
	lseek(fd, 0, SEEK_SET);
	return n == (ssize_t)sizeof(b) && b[0] == 'B' && b[1] == 'Z' && b[2] == 'h' && b[3] >= '1' && b[3] <= '9'
	       && (memcmp(b + 4, block, 6) == 0 || memcmp(b + 4, end, 6) == 0);
}

static const char *bz2Message(int err) {
	return err == BZ_UNEXPECTED_EOF ? "truncated bzip2 input"
	     : err == BZ_IO_ERROR ? "bzip2 read error"
	     : err == BZ_MEM_ERROR ? "out of memory decompressing bzip2 input"
	     : "invalid bzip2 data";
}

static void bz2Fail(const bz2Strm *s, int err) {
	std::cerr << "Error: could not read \"" << s->name << "\": " << bz2Message(err) << std::endl;
	exit(1);
}

bz2Strm *bz2FdOpen(int fd, const char *name) {
	if (fd == -1)
		return NULL;
	bz2Strm *s = new bz2Strm();
	FILE *fp = (s != NULL) ? fdopen(fd, "rb") : NULL;
	int err = BZ_OK;
	BZFILE *bz = (fp != NULL) ? BZ2_bzReadOpen(&err, fp, 0, 0, NULL, 0) : NULL;
	if (bz == NULL || err != BZ_OK) {
		if (bz != NULL) BZ2_bzReadClose(&err, bz);
		if (fp != NULL) fclose(fp);
		delete s;
		return NULL;
	}
	s->name = name;
	s->fp = fp;
	s->bz = bz;
	s->back = EOF;
	return s;
}

// Refill buf, continuing through concatenated streams. Returns the number of
// bytes read, 0 at the end of the input; a decoding error is fatal.
static size_t bz2Fill(bz2Strm *s) {
	size_t got = 0;
	while (got < sizeof(s->buf) && s->bz != NULL) {
		int err = BZ_OK;
		int n = BZ2_bzRead(&err, s->bz, s->buf + got, (int)(sizeof(s->buf) - got));
		if (err == BZ_OK) { got += n; s->more = false; continue; }
		if (err == BZ_DATA_ERROR_MAGIC && s->more) {   // trailing bytes after the last stream
			BZ2_bzReadClose(&err, s->bz); s->bz = NULL;
			break;
		}
		if (err != BZ_STREAM_END)
			bz2Fail(s, err);
		got += n;
		void *unused = NULL;
		int nUnused = 0;
		char next[BZ_MAX_UNUSED];
		BZ2_bzReadGetUnused(&err, s->bz, &unused, &nUnused);
		if (nUnused > 0) memcpy(next, unused, nUnused);
		BZ2_bzReadClose(&err, s->bz); s->bz = NULL;
		int c = nUnused > 0 ? EOF : getc(s->fp);
		if (c != EOF) ungetc(c, s->fp);
		if (nUnused > 0 || c != EOF) {                 // another stream follows
			s->bz = BZ2_bzReadOpen(&err, s->fp, 0, 0, next, nUnused);
			if (err != BZ_OK)
				bz2Fail(s, err);
			s->more = true;
		}
	}
	return got;
}

int bz2Getc(bz2Strm *s) {
	if (s->back != EOF) {
		int c = s->back;
		s->back = EOF;
		return c;
	}
	if (s->pos == s->len) {
		s->len = bz2Fill(s);
		s->pos = 0;
		if (s->len == 0)
			return EOF;
	}
	return (unsigned char)s->buf[s->pos++];
}

int bz2Ungetc(int c, bz2Strm *s) {
	if (c == EOF || s->back != EOF)
		return EOF;
	s->back = c;
	return c;
}

int bz2Close(bz2Strm *s) {
	if (s == NULL)
		return -1;
	int err;
	if (s->bz != NULL) BZ2_bzReadClose(&err, s->bz);
	if (s->fp != NULL) fclose(s->fp);
	delete s;
	return 0;
}
