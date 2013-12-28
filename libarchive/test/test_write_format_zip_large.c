/*-
 * Copyright (c) 2003-2007,2013 Tim Kientzle
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "test.h"
__FBSDID("$FreeBSD$");

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * This is a somewhat tricky test that verifies the ability to
 * write and read very large entries to zip archives.
 *
 * See test_tar_large.c for more information about the machinery
 * being used here.
 */

static size_t nullsize;
static void *nulldata;

struct fileblock {
	struct fileblock *next;
	int	size;
	void *buff;
	int64_t gap_size; /* Size of following gap */
};

struct fileblocks {
	int64_t filesize;
	int64_t fileposition;
	int64_t gap_remaining;
	void *buff;
	struct fileblock *first;
	struct fileblock *current;
	struct fileblock *last;
};

/* The following size definitions simplify things below. */
#define KB ((int64_t)1024)
#define MB ((int64_t)1024 * KB)
#define GB ((int64_t)1024 * MB)
#define TB ((int64_t)1024 * GB)

static int64_t	memory_read_skip(struct archive *, void *, int64_t request);
static ssize_t	memory_read(struct archive *, void *, const void **buff);
static ssize_t	memory_write(struct archive *, void *, const void *, size_t);

static ssize_t
memory_write(struct archive *a, void *_private, const void *buff, size_t size)
{
	struct fileblocks *private = _private;
	struct fileblock *block;

	(void)a;

	if ((const char *)nulldata <= (const char *)buff
	    && (const char *)buff < (const char *)nulldata + nullsize) {
		/* We don't need to store a block of gap data. */
		private->last->gap_size += (int64_t)size;
	} else {
		/* Yes, we're assuming the very first write is metadata. */
		/* It's header or metadata, copy and save it. */
		block = (struct fileblock *)malloc(sizeof(*block));
		memset(block, 0, sizeof(*block));
		block->size = size;
		block->buff = malloc(size);
		memcpy(block->buff, buff, size);
		if (private->last == NULL) {
			private->first = private->last = block;
		} else {
			private->last->next = block;
			private->last = block;
		}
		block->next = NULL;
	}
	private->filesize += size;
	return ((long)size);
}

static ssize_t
memory_read(struct archive *a, void *_private, const void **buff)
{
	struct fileblocks *private = _private;
	ssize_t size;

	(void)a;

	while (private->current != NULL && private->buff == NULL && private->gap_remaining == 0) {
		private->current = private->current->next;
		if (private->current != NULL) {
			private->buff = private->current->buff;
			private->gap_remaining = private->current->gap_size;
		}
	}

	if (private->current == NULL)
		return (ARCHIVE_EOF);

	/* If there's real data, return that. */
	if (private->buff != NULL) {
		*buff = private->buff;
		size = (private->current->buff + private->current->size)
		    - private->buff;
		private->buff = NULL;
		private->fileposition += size;
		return (size);
	}

	/* Big gap: too big to return all at once, so just return some. */
	if (private->gap_remaining > (int64_t)nullsize) {
		private->gap_remaining -= nullsize;
		*buff = nulldata;
		private->fileposition += nullsize;
		return (nullsize);
	}

	/* Small gap: finish the gap and prep for next block. */
	if (private->gap_remaining > 0) {
		size = (ssize_t)private->gap_remaining;
		*buff = nulldata;
		private->gap_remaining = 0;
		private->fileposition += size;

		private->current = private->current->next;
		if (private->current != NULL) {
			private->buff = private->current->buff;
			private->gap_remaining = private->current->gap_size;
		}

		return (size);
	}
	fprintf(stderr, "\n\n\nInternal failure\n\n\n");
	exit(1);
}

static int
memory_read_open(struct archive *a, void *_private)
{
	struct fileblocks *private = _private;

	(void)a; /* UNUSED */

	private->current = private->first;
	private->fileposition = 0;
	if (private->current != NULL) {
		private->buff = private->current->buff;
		private->gap_remaining = private->current->gap_size;
	}
	return (ARCHIVE_OK);
}

static int64_t
memory_read_seek(struct archive *a, void *_private, int64_t offset, int whence)
{
	struct fileblocks *private = _private;

	(void)a;
	if (whence == SEEK_END) {
		offset = private->filesize + offset;
		whence = SEEK_SET;
	} else if (whence == SEEK_CUR) {
		offset = private->fileposition + offset;
		whence = SEEK_SET;
	}

	if (offset < 0) {
		fprintf(stderr, "\n\n\nInternal failure: negative seek\n\n\n");
		exit(1);
	}

	/* We've converted the request into a SEEK_SET. */
	private->fileposition = offset;

	/* Walk the block list to find the new position. */
	offset = 0;
	private->current = private->first;
	while (private->current != NULL) {
		if (offset + private->current->size > private->fileposition) {
			/* Position is in this block. */
			private->buff = private->current->buff
			    + private->fileposition - offset;
			private->gap_remaining = private->current->gap_size;
			return private->fileposition;
		}
		offset += private->current->size;
		if (offset + private->current->gap_size > private->fileposition) {
			/* Position is in this gap. */
			private->buff = NULL;
			private->gap_remaining = private->current->gap_size
			    - (private->fileposition - offset);
			return private->fileposition;
		}
		offset += private->current->gap_size;
		/* Skip to next block. */
		private->current = private->current->next;
	}
	if (private->fileposition == private->filesize) {
		return private->fileposition;
	}
	fprintf(stderr, "\n\n\nInternal failure: over-sized seek\n\n\n");
	exit(1);
}

static int64_t
memory_read_skip(struct archive *a, void *_private, int64_t skip)
{
	struct fileblocks *private = _private;
	int64_t old_position = private->fileposition;
	int64_t new_position = memory_read_seek(a, _private, skip, SEEK_CUR);
	return (new_position - old_position);
}

DEFINE_TEST(test_write_format_zip_large)
{
	/* The sizes of the entries we're going to generate. */
	static int64_t tests[] = {
		/* Test for 32-bit signed overflow. */
		2 * GB - 1, 2 * GB, 2 * GB + 1,
		/* Test for 32-bit unsigned overflow. */
		4 * GB - 1, 4 * GB, 4 * GB + 1,
		/* And beyond ... because we can. */
		16 * GB - 1, 16 * GB, 16 * GB + 1,
		64 * GB - 1, 64 * GB, 64 * GB + 1,
		256 * GB - 1, 256 * GB, 256 * GB + 1,
		1 * TB,
		0
	};
	int i;
	char namebuff[64];
	struct fileblocks fileblocks;
	struct archive_entry *ae;
	struct archive *a;
	int64_t  filesize;
	size_t writesize;

	nullsize = (size_t)(1 * MB);
	nulldata = malloc(nullsize);
	memset(nulldata, 0xAA, nullsize);
	memset(&fileblocks, 0, sizeof(fileblocks));

	/*
	 * Open an archive for writing.
	 */
	a = archive_write_new();
	archive_write_set_format_zip(a);
	archive_write_set_options(a, "zip:compression=store");
	archive_write_set_options(a, "zip:fakecrc32");
	archive_write_set_bytes_per_block(a, 0); /* No buffering. */
	archive_write_open(a, &fileblocks, NULL, memory_write, NULL);

	/*
	 * Write a series of large files to it.
	 */
	for (i = 0; tests[i] != 0; i++) {
		assert((ae = archive_entry_new()) != NULL);
		sprintf(namebuff, "file_%d", i);
		archive_entry_copy_pathname(ae, namebuff);
		archive_entry_set_mode(ae, S_IFREG | 0755);
		filesize = tests[i];

		archive_entry_set_size(ae, filesize);

		assertA(0 == archive_write_header(a, ae));
		archive_entry_free(ae);

		/*
		 * Write the actual data to the archive.
		 */
		while (filesize > 0) {
			writesize = nullsize;
			if ((int64_t)writesize > filesize)
				writesize = (size_t)filesize;
			assertA((int)writesize
			    == archive_write_data(a, nulldata, writesize));
			filesize -= writesize;
		}
	}

	assert((ae = archive_entry_new()) != NULL);
	archive_entry_copy_pathname(ae, "lastfile");
	archive_entry_set_mode(ae, S_IFREG | 0755);
	assertA(0 == archive_write_header(a, ae));
	archive_entry_free(ae);


	/* Close out the archive. */
	assertEqualIntA(a, ARCHIVE_OK, archive_write_close(a));
	assertEqualInt(ARCHIVE_OK, archive_write_free(a));

	/*
	 * Open the same archive for reading.
	 */
	a = archive_read_new();
	assertEqualIntA(a, ARCHIVE_OK,
	    archive_read_support_format_zip_seekable(a));
	assertEqualIntA(a, ARCHIVE_OK,
	    archive_read_set_options(a, "zip:ignorecrc32"));
	assertEqualIntA(a, ARCHIVE_OK,
	    archive_read_set_open_callback(a, memory_read_open));
	assertEqualIntA(a, ARCHIVE_OK,
	    archive_read_set_read_callback(a, memory_read));
	assertEqualIntA(a, ARCHIVE_OK,
	    archive_read_set_skip_callback(a, memory_read_skip));
	assertEqualIntA(a, ARCHIVE_OK,
	    archive_read_set_seek_callback(a, memory_read_seek));
	assertEqualIntA(a, ARCHIVE_OK,
	    archive_read_set_callback_data(a, &fileblocks));
	assertEqualIntA(a, ARCHIVE_OK, archive_read_open1(a));

	/*
	 * Read entries back.
	 */
	for (i = 0; tests[i] > 0; i++) {
		assertEqualIntA(a, ARCHIVE_OK,
		    archive_read_next_header(a, &ae));
		sprintf(namebuff, "file_%d", i);
		assertEqualString(namebuff, archive_entry_pathname(ae));
		assertEqualInt(tests[i], archive_entry_size(ae));
	}
	assertEqualIntA(a, 0, archive_read_next_header(a, &ae));
	assertEqualString("lastfile", archive_entry_pathname(ae));

	assertEqualIntA(a, ARCHIVE_EOF, archive_read_next_header(a, &ae));

	/* Close out the archive. */
	assertEqualIntA(a, ARCHIVE_OK, archive_read_close(a));
	assertEqualInt(ARCHIVE_OK, archive_read_free(a));

	free(fileblocks.buff);
	free(nulldata);
}
