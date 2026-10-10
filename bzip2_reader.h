#ifndef _BZIP2_READER_H_
#define _BZIP2_READER_H_

/*
 * bowtie2-crossplatform: streaming bzip2 read input (built with WITH_BZIP2=1),
 * shaped like zstd_decompress.h so that CFilePatternSource can use it the same
 * way. Reads concatenated bzip2 streams in order and ignores bytes after the
 * last stream, as `bzip2 -dc` does. A truncated or corrupt stream is a fatal
 * error naming the file, not a silent early end of the reads.
 */

struct bz2Strm;

bool bz2IsFile(int fd);                          // starts with a bzip2 header? (rewinds fd)
bz2Strm *bz2FdOpen(int fd, const char *name);    // takes ownership of fd; NULL on failure
int bz2Getc(bz2Strm *s);                         // next byte, or EOF at the end
int bz2Ungetc(int c, bz2Strm *s);
int bz2Close(bz2Strm *s);

#endif
