
/*
  graf.c Copyright (C) 2016 Jerry Rassier - adapted from zran.c, for
  which the copyright/license notice appears below.

  This software is distributed under the zlib license.

  === zlib copyright/license notice ===

  Copyright (C) 1995-2013 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu


  The data format used by the zlib library is described by RFCs (Request for
  Comments) 1950 to 1952 in the files http://tools.ietf.org/html/rfc1950
  (zlib format), rfc1951 (deflate format) and rfc1952 (gzip format).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include "zlib.h"

#define local static

#define WINSIZE 32768U      /* sliding window size */
#define CHUNK 16384         /* file input buffer size */

/* access point entry */
struct point {
    off_t out;          /* corresponding offset in uncompressed data */
    off_t in;           /* offset in input file of first full byte */
    int bits;           /* number of bits (1-7) from byte at in - 1, or 0 */
    unsigned char window[WINSIZE];  /* preceding 32K of uncompressed data */
};

/* access point list */
struct access {
    int have;           /* number of list entries filled in */
    int size;           /* number of list entries allocated */
    size_t usize;		/* uncompressed size of the gzip file */
    struct point *list; /* allocated list */
};

/* Deallocate an index built by build_index() */
local void free_index(struct access *index)
{
    if (index != NULL) {
        free(index->list);
        free(index);
    }
}

/* Add an entry to the access point list.  If out of memory, deallocate the
   existing list and return NULL. */
local struct access *addpoint(struct access *index, int bits, off_t in, off_t out, unsigned left, unsigned char *window)
{
    struct point *next;

    /* if list is empty, create it (start with eight points) */
    if (index == NULL)
    {
        index = malloc(sizeof(struct access));
        if (index == NULL) return NULL;

        index->list = malloc(sizeof(struct point) << 3);

        if (index->list == NULL)
        {
            free(index);
            return NULL;
        }
        index->size = 8;
        index->have = 0;
    }
    /* if list is full, make it bigger */
    else if (index->have == index->size)
    {
        index->size <<= 1;
        next = realloc(index->list, sizeof(struct point) * index->size);
        if (next == NULL)
        {
            free_index(index);
            return NULL;
        }
        index->list = next;
    }

    /* fill in entry and increment how many we have */
    next = index->list + index->have;
    next->bits = bits;
    next->in = in;
    next->out = out;
    if (left)
        memcpy(next->window, window + WINSIZE - left, left);
    if (left < WINSIZE)
        memcpy(next->window + left, window, WINSIZE - left);
    index->have++;

    /* return list, possibly reallocated */
    return index;
}

/* Make one entire pass through the compressed stream and build an index, with
   access points about every span bytes of uncompressed output -- span is
   chosen to balance the speed of random access against the memory requirements
   of the list, about 32K bytes per access point.  Note that data after the end
   of the first zlib or gzip stream in the file is ignored.  build_index()
   returns the number of access points on success (>= 1), Z_MEM_ERROR for out
   of memory, Z_DATA_ERROR for an error in the input file, or Z_ERRNO for a
   file read error.  On success, *built points to the resulting index. */
local int build_index(FILE *in, off_t span, struct access **built)
{
    int ret;
    off_t totin, totout;        /* our own total counters to avoid 4GB limit */
    off_t last;                 /* totout value of last access point */
    struct access *index;       /* access points being generated */
    z_stream strm;
    unsigned char input[CHUNK];
    unsigned char window[WINSIZE];

	/* The original code used these memory blocks without zeroing them -
	 * this led to ~32kB of potentially sensitive 'junk' data being
	 * written to the index file for the first window.
	 */
	memset(&input[0], 0, CHUNK);
	memset(&window[0], 0, WINSIZE);

    /* initialize inflate */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, 47);      /* automatic zlib or gzip decoding */
    if (ret != Z_OK)
        return ret;

    /* inflate the input, maintain a sliding window, and build an index -- this
       also validates the integrity of the compressed data using the check
       information at the end of the gzip or zlib stream */
    totin = totout = last = 0;
    index = NULL;               /* will be allocated by first addpoint() */
    strm.avail_out = 0;
    do
    {
        /* get some compressed data from input file */
        strm.avail_in = fread(input, 1, CHUNK, in);
        if (ferror(in))
        {
            ret = Z_ERRNO;
            goto build_index_error;
        }
        if (strm.avail_in == 0)
        {
            ret = Z_DATA_ERROR;
            goto build_index_error;
        }
        strm.next_in = input;

        /* process all of that, or until end of stream */
        do
        {
            /* reset sliding window if necessary */
            if (strm.avail_out == 0)
            {
                strm.avail_out = WINSIZE;
                strm.next_out = window;
            }

            /* inflate until out of input, output, or at end of block --
               update the total input and output counters */
            totin += strm.avail_in;
            totout += strm.avail_out;

            ret = inflate(&strm, Z_BLOCK);      /* return at end of block */
            totin -= strm.avail_in;
            totout -= strm.avail_out;

            if (ret == Z_NEED_DICT)
                ret = Z_DATA_ERROR;
            if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
                goto build_index_error;
            if (ret == Z_STREAM_END)
            {
                break;
            }

            /* if at end of block, consider adding an index entry (note that if
               data_type indicates an end-of-block, then all of the
               uncompressed data from that block has been delivered, and none
               of the compressed data after that block has been consumed,
               except for up to seven bits) -- the totout == 0 provides an
               entry point after the zlib or gzip header, and assures that the
               index always has at least one access point; we avoid creating an
               access point after the last block by checking bit 6 of data_type
             */
            if ((strm.data_type & 128) && !(strm.data_type & 64) && (totout == 0 || totout - last > span))
            {
                index = addpoint(index, strm.data_type & 7, totin, totout, strm.avail_out, window);
                if (index == NULL)
                {
                    ret = Z_MEM_ERROR;
                    goto build_index_error;
                }
                last = totout;
            }
        }
        while (strm.avail_in != 0);
    }
    while (ret != Z_STREAM_END);

    /* clean up and return index (release unused entries in list) */
    (void)inflateEnd(&strm);
    index->list = realloc(index->list, sizeof(struct point) * index->have);
    index->size = index->have;
    index->usize = totout;
    *built = index;
    return index->size;

    /* return error */
  build_index_error:
    (void)inflateEnd(&strm);
    if (index != NULL)
        free_index(index);
    return ret;
}

/* Use the index to read len bytes from offset into buf, return bytes read or
   negative for error (Z_DATA_ERROR or Z_MEM_ERROR).  If data is requested past
   the end of the uncompressed data, then extract() will return a value less
   than len, indicating how much as actually read into buf.  This function
   should not return a data error unless the file was modified since the index
   was generated.  extract() may also return Z_ERRNO if there is an error on
   reading or seeking the input file. */
local int extract(FILE *in, struct access *index, off_t offset, unsigned char *buf, int len)
{
    int ret, skip;
    z_stream strm;
    struct point *here;
    unsigned char input[CHUNK];
    unsigned char discard[WINSIZE];

    /* proceed only if something reasonable to do */
    if (len <= 0)
    {
        fprintf(stderr, "graf: Nonsenical number of bytes (%d) requested. Don't ask for more than %d (max int size on this system) or fewer than 1 (since that's just silly).\n", len, INT_MAX);
        return 0;
	}

	if((offset + len) > index->usize)
	{
		fprintf(stderr, "graf: offset %zu is beyond end of file\n", offset);
		return 0;
	}

    /* find where in stream to start */
    here = index->list;
    ret = index->have;
    while (--ret && here[1].out <= offset)
        here++;

    /* initialize file and inflate state to start there */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, -15);         /* raw inflate */
    if (ret != Z_OK)
        return ret;
    ret = fseeko(in, here->in - (here->bits ? 1 : 0), SEEK_SET);
    if (ret == -1)
        goto extract_ret;
    if (here->bits)
    {
        ret = getc(in);
        if (ret == -1)
        {
            ret = ferror(in) ? Z_ERRNO : Z_DATA_ERROR;
            goto extract_ret;
        }
        (void)inflatePrime(&strm, here->bits, ret >> (8 - here->bits));
    }
    (void)inflateSetDictionary(&strm, here->window, WINSIZE);

    /* skip uncompressed bytes until offset reached, then satisfy request */
    offset -= here->out;
    strm.avail_in = 0;
    skip = 1;                               /* while skipping to offset */
    do
    {
        /* define where to put uncompressed data, and how much */
        if (offset == 0 && skip)
        {          /* at offset now */
            strm.avail_out = len;
            strm.next_out = buf;
            skip = 0;                       /* only do this once */
        }
        if (offset > WINSIZE)
        {             /* skip WINSIZE bytes */
            strm.avail_out = WINSIZE;
            strm.next_out = discard;
            offset -= WINSIZE;
        }
        else if (offset != 0)
        {             /* last skip */
            strm.avail_out = (unsigned)offset;
            strm.next_out = discard;
            offset = 0;
        }

        /* uncompress until avail_out filled, or end of stream */
        do
        {
            if (strm.avail_in == 0)
            {
                strm.avail_in = fread(input, 1, CHUNK, in);
                if (ferror(in))
                {
                    ret = Z_ERRNO;
                    goto extract_ret;
                }
                if (strm.avail_in == 0)
                {
                    ret = Z_DATA_ERROR;
                    goto extract_ret;
                }
                strm.next_in = input;
            }
            ret = inflate(&strm, Z_NO_FLUSH);       /* normal inflate */
            if (ret == Z_NEED_DICT)
                ret = Z_DATA_ERROR;
            if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
                goto extract_ret;
            if (ret == Z_STREAM_END)
                break;
        }
        while (strm.avail_out != 0);

        /* if reach end of stream, then don't keep trying to get more */
        if (ret == Z_STREAM_END)
            break;

        /* do until offset reached and requested data read, or stream ends */
    }
    while (skip);

    /* compute number of uncompressed bytes read after offset */
    ret = skip ? 0 : len - strm.avail_out;

    /* clean up and return bytes read or error */
  extract_ret:
    (void)inflateEnd(&strm);
    return ret;
}

void write_index(struct access *index, char *filename)
{
	FILE *fh = fopen(filename, "wb");
	// Write the uncompressed size, followed by all access points.
	fwrite(&index->usize, sizeof(size_t), 1, fh);
	fwrite(index->list, sizeof(struct point), index->have, fh);
	fclose(fh);
}

off_t fsize(const char *filename)
{
	struct stat st;
	if(stat(filename, &st) == 0)
	{
		return st.st_size;
	}
	else
	{
		return -1;
	}
}

int load_index(char* filename, struct access** idx)
{
	off_t size = 0;
	FILE *fh = fopen(filename, "rb");

	if( (!fh) || (fsize(filename) <= 0) )
	{
		fprintf(stderr, "graf: invalid or inaccessible index file %s", filename);
		return 1;
	}

	size = fsize(filename);

	struct point* list;
	list = calloc(1, (size - sizeof(size_t)));

	struct access* dest = *idx;

	fread(&dest->usize, sizeof(size_t), 1, fh);
	fread(list, (size - sizeof(size_t)), 1, fh);

	dest->have = ((size - sizeof(size_t)) / sizeof(struct point));
	dest->size = dest->have;
	dest->list = list;

	fclose(fh);
	return 0;
}

void usage(char* progname)
{
        fprintf(stderr, "\nThis is GRAF, a Gzip Random Access Facilitator based on the zran.c example by Mark Adler.\n");
        fprintf(stderr, " Example usage:\n");
        fprintf(stderr, " 1. Generate an index. You only have to do this once for a given .gz file:\n\n");
        fprintf(stderr, "    %s myfile.gz myfile.gz.idx build 1048576\n\n",progname);
        fprintf(stderr, "    - This does a full read of myfile.gz to create an index file with 1MB spans, named myfile.gz.idx\n");
        fprintf(stderr, "    - Larger span lengths decrease index filesize, but cause longer random access times and require more memory to work with.\n");
        fprintf(stderr, "    - Each index entry is approximately 32KB in size, so a span length less than that would just be silly.\n\n");

        fprintf(stderr, " 2. Use the index file:\n\n");
        fprintf(stderr, "    %s myfile.gz myfile.gz.idx read 9000 42\n\n",progname);
        fprintf(stderr, "    - This uses the index file you generated in step 1 to read the 42 bytes that begin at uncompressed offset 9000.\n");
        fprintf(stderr, "    - This and all subsequent reads will be much faster than having to decompress the whole file up to your desired offset.\n");
        fprintf(stderr, "    - Attempted reads which extend beyond the end of the file and/or begin at non-existent offsets may result in errors...or segfaults.\n\n");

        fprintf(stderr, " Quick usage:\n\n");
        fprintf(stderr, "    %s (gzfile) (idxfile) build (spanlength)\n",progname);
        fprintf(stderr, "    %s (gzfile) (idxfile) read (offset) (bytes to read)\n\n",progname);
}

int main(int argc, char **argv)
{
    FILE *in;
    struct access *index = NULL;
	int len = 0;

	// build = 5 args
	// read = 6 args

	if (argc < 5 || argc > 6) {	usage(argv[0]);	return 1; }

	if ( strcmp(argv[3], "build") == 0)
	{
		if (argc != 5) { usage(argv[0]); return 1; }

		char *gzfile = argv[1];
		char *idxfile = argv[2];

		size_t spanlength;
		sscanf(argv[4], "%zu", &spanlength);

		fprintf(stderr,"Building index %s from gzip file %s with span length %zu\n", idxfile, gzfile, spanlength);

		in = fopen(gzfile, "rb");

		len = build_index(in, spanlength, &index);
		if (len < 0) {
			fclose(in);
			switch (len) {
			case Z_MEM_ERROR:
				fprintf(stderr, "zran: out of memory\n");
				break;
			case Z_DATA_ERROR:
				fprintf(stderr, "zran: compressed data error in %s\n", argv[1]);
				break;
			case Z_ERRNO:
				fprintf(stderr, "zran: read error on %s\n", argv[1]);
				break;
			default:
				fprintf(stderr, "zran: error %d while building index\n", len);
			}
			return 1;
		}
		write_index(index, idxfile);
		fprintf(stderr,"Wrote index %s with %d access points - index size is %zu bytes\n", idxfile, len, fsize(idxfile));
		fclose(in);
		return 0;
	}
	else if (strcmp(argv[3], "read") == 0)
	{
		if (argc != 6) { usage(argv[0]); return 1; }
		char *gzfile = argv[1];
		char *idxfile = argv[2];
		struct access *index = malloc(sizeof(*index));

		off_t readoffset;
		sscanf(argv[4], "%zu", &readoffset);

		size_t readlen;
		sscanf(argv[5], "%zu", &readlen);

		in = fopen(gzfile, "rb");

		unsigned char *buf = malloc(readlen + 1);

		int load_index_res = load_index(idxfile, &index);

		if(load_index_res != 0)
		{
			fprintf(stderr, "Unable to load index file %s", idxfile);
			free(index);
			return 0;
		}

		int bytes_extracted = extract(in, index, readoffset, buf, readlen);
		if(bytes_extracted > 0)
		{
			fwrite(buf, 1, bytes_extracted, stdout);
			printf("\n");
		}

		free_index(index);
		free(buf);
		fclose(in);

		return 0;
	}
	else
	{
		fprintf(stderr, "Invalid operation: %s\n",argv[3]);
		return 2;
	}

	return 3;
}
