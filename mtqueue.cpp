#include "mtqueue.h"
#include "atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define DEFAULT_QUEUE_SIZE 64
#define MAX_GLOBAL_MQ 0x10000
#define MQ_OVERLOAD 1024

#define mt_malloc malloc
#define mt_free free

struct message_queue {
	spinlock_t lock;
	uint32_t handle;
	int cap;
	int head;
	int tail;
	int release;
	int overload;
	int overload_threshold;
	struct request_message *queue;
};

struct message_queue * mt_queue_create(uint32_t handle) {
	struct message_queue * q = (struct message_queue *)mt_malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	SPIN_INIT(q);
	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;
	q->queue = (struct request_message *)mt_malloc(sizeof(struct request_message *)*q->cap);
	return q;
}

void mt_queue_delete(struct message_queue * mq) {
	if (mq->queue)
		mt_free(mq->queue);
	mt_free(mq);
}


static void expend_queue(struct message_queue * mq) {
	struct request_message * new_queue = (struct request_message *)mt_malloc(sizeof(*new_queue)*mq->cap*2);
	int i;
	for (i = 0; i < mq->cap; i++) {
		new_queue[i] = mq->queue[i];
	}
	mq->head = 0;
	mq->tail = mq->cap;
	mq->cap *= 2;

	mt_free(mq->queue);
	mq->queue = new_queue;
}

void mq_queue_push(struct message_queue * mq, struct request_message * sm) {
	SPIN_LOCK(mq);

	mq->queue[mq->tail] = *sm;
	if (++mq->tail >= mq->cap) {
		mq->tail = 0;
	}
	
	if (mq->head == mq->tail) {
		expend_queue(mq);
	}

	SPIN_UNLOCK(mq);
}

int mq_queue_pop(struct message_queue * mq, struct request_message * message) {
	int ret = 1;
	SPIN_LOCK(mq);

	if (mq->head != mq->tail) {
		*message	= mq->queue[mq->head++];
		ret			= 0;
		int head	= mq->head;
		int tail	= mq->tail;
		int cap		= mq->cap;

		if (head >= cap) {
			mq->head = head = 0;
		}
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}

		while (length > mq->overload_threshold) {
			mq->overload = length;
			mq->overload_threshold *= 2;
		}
	} else {
		mq->overload_threshold = MQ_OVERLOAD;
	}

	SPIN_UNLOCK(mq);

	return ret;
}

int mq_queue_length(struct message_queue * mq) {
	int head, tail, cap;

	SPIN_LOCK(mq);

	head = mq->head;
	tail = mq->tail;
	cap = mq->cap;

	SPIN_UNLOCK(mq);

	if (head <= tail) {
		return tail - head;
	}

	return tail + cap - head;
}

int mq_queue_overload(struct message_queue * mq) {
	if (mq->overload) {
		int overload = mq->overload;
		mq->overload = 0;
		return overload;
	}
	return 0;
}