#ifndef __inet_buffer_h__
#define __inet_buffer_h__


struct evbuffer;

struct evbuffer * evbuffer_new();
void evbuffer_free(struct evbuffer *);

int evbuffer_remove(struct evbuffer *buf, void *data, size_t datlen);
int evbuffer_expand(struct evbuffer *buf, size_t datlen);
int evbuffer_add(struct evbuffer *buf, const void *data, size_t datlen);
void evbuffer_drain(struct evbuffer *buf, size_t len);
int evbuffer_add_buffer(struct evbuffer *outbuf, struct evbuffer *inbuf);

#endif