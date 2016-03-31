#include "buffer.h"
#include <stdlib.h>
#include <string.h>

typedef unsigned char u_char;

struct evbuffer {
	u_char *buffer;
	u_char *orig_buffer;

	size_t misalign;
	size_t totallen;
	size_t off;

	//void (*cb)(struct evbuffer *, size_t, size_t, void *);
	//void *cbarg;
};

struct evbuffer *
evbuffer_new(void) 
{
	struct evbuffer * buffer;
	buffer = (struct evbuffer *)calloc(1, sizeof(struct evbuffer));
	return buffer;
}

void
evbuffer_free(struct evbuffer * buffer) 
{
	if (buffer->orig_buffer != NULL)
		free(buffer->orig_buffer);
	free(buffer);
}

/* Adds data to an event buffer */

static void
evbuffer_align(struct evbuffer *buf)
{
	memmove(buf->orig_buffer, buf->buffer, buf->off);
	buf->buffer = buf->orig_buffer;
	buf->misalign = 0;
}


/* Expands the available space in the event buffer to at least datlen */

int
evbuffer_expand(struct evbuffer *buf, size_t datlen)
{
	size_t need = buf->misalign + buf->off + datlen;

	/* If we can fit all the data, then we don't have to do anything */
	if (buf->totallen >= need)
		return (0);

	/*
	 * If the misalignment fulfills our data needs, we just force an
	 * alignment to happen.  Afterwards, we have enough space.
	 */
	if (buf->misalign >= datlen) {
		evbuffer_align(buf);
	} else {
		void *newbuf;
		size_t length = buf->totallen;

		if (length < 256)
			length = 256;
		while (length < need)
			length <<= 1;

		if (buf->orig_buffer != buf->buffer)
			evbuffer_align(buf);
		if ((newbuf = realloc(buf->buffer, length)) == NULL)
			return (-1);

		buf->orig_buffer = buf->buffer = (u_char *)newbuf;
		buf->totallen = length;
	}

	return (0);
}

int
evbuffer_add(struct evbuffer *buf, const void *data, size_t datlen)
{
	size_t need = buf->misalign + buf->off + datlen;
	size_t oldoff = buf->off;

	if (buf->totallen < need) {
		if (evbuffer_expand(buf, datlen) == -1)
			return (-1);
	}

	memcpy(buf->buffer + buf->off, data, datlen);
	buf->off += datlen;

// 	if (datlen && buf->cb != NULL)
// 		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

	return (0);
}

void
evbuffer_drain(struct evbuffer *buf, size_t len)
{
	size_t oldoff = buf->off;

	if (len >= buf->off) {
		buf->off = 0;
		buf->buffer = buf->orig_buffer;
		buf->misalign = 0;
		goto done;
	}

	buf->buffer += len;
	buf->misalign += len;

	buf->off -= len;

 done:
// 	/* Tell someone about changes in this buffer */
// 	if (buf->off != oldoff && buf->cb != NULL)
// 		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);
// 
	int i = 0;
	i++;
}


/* 
 * This is a destructive add.  The data from one buffer moves into
 * the other buffer.
 */

#define SWAP(x,y) do { \
	(x)->buffer = (y)->buffer; \
	(x)->orig_buffer = (y)->orig_buffer; \
	(x)->misalign = (y)->misalign; \
	(x)->totallen = (y)->totallen; \
	(x)->off = (y)->off; \
} while (0)

int
evbuffer_add_buffer(struct evbuffer *outbuf, struct evbuffer *inbuf)
{
	int res;

	/* Short cut for better performance */
	if (outbuf->off == 0) {
		struct evbuffer tmp;
		size_t oldoff = inbuf->off;

		/* Swap them directly */
		SWAP(&tmp, outbuf);
		SWAP(outbuf, inbuf);
		SWAP(inbuf, &tmp);

		///* 
		// * Optimization comes with a price; we need to notify the
		// * buffer if necessary of the changes. oldoff is the amount
		// * of data that we transfered from inbuf to outbuf
		// */
		//if (inbuf->off != oldoff && inbuf->cb != NULL)
		//	(*inbuf->cb)(inbuf, oldoff, inbuf->off, inbuf->cbarg);
		//if (oldoff && outbuf->cb != NULL)
		//	(*outbuf->cb)(outbuf, 0, oldoff, outbuf->cbarg);
		
		return (0);
	}

	res = evbuffer_add(outbuf, inbuf->buffer, inbuf->off);
	if (res == 0) {
		/* We drain the input buffer on success */
		evbuffer_drain(inbuf, inbuf->off);
	}

	return (res);
}

int
evbuffer_remove(struct evbuffer *buf, void *data, size_t datlen)
{
	size_t nread = datlen;
	if (nread >= buf->off)
		nread = buf->off;

	memcpy(data, buf->buffer, nread);
	evbuffer_drain(buf, nread);
	
	return (nread);
}

