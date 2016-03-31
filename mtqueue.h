#ifndef __mt_queue_h__
#define __mt_queue_h__
#include <stdint.h>

struct request_message {
	int type;
	uint32_t source;
	int session;
	void * data;
	size_t sz;
};

struct message_queue;

struct message_queue * mt_queue_create(uint32_t handle);
void mt_queue_delete(struct message_queue * mq);

void mq_queue_push(struct message_queue * mq, struct request_message * message);
int mq_queue_pop(struct message_queue * mq, struct request_message * message);

int mq_queue_length(struct message_queue * mq);
int mq_queue_overload(struct message_queue * mq);

#endif