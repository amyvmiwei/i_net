#include "inet_base.h"
#include "socket_poll.h"
#include "mtqueue.h"

#include <stdio.h> // for fprintf;
#include <stdlib.h> //for malloc
#include <string.h> //for memset
#include <assert.h>

#define MAX_SOCKET_P 16
#define MAX_SOCKET (1<<MAX_SOCKET_P)
#define MAX_EVENT 64
#define MIN_READ_BUFFER 64
#define MAX_INFO 128

#define SOCKET_TYPE_INVALID 0
#define SOCKET_TYPE_RESERVE 1		//?
#define SOCKET_TYPE_PLISTEN 2		//?
#define SOCKET_TYPE_LISTEN 3
#define SOCKET_TYPE_CONNECTING 4
#define SOCKET_TYPE_CONNECTED 5
#define SOCKET_TYPE_HALFCLOSE 6
#define SOCKET_TYPE_PACCEPT 7		//
#define SOCKET_TYPE_BIND 8			//?

#define HASH_ID(id) (((unsigned int)id) % MAX_SOCKET)

#define MALLOC malloc
#define FREE free

union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

struct write_buffer {
	struct write_buffer * next;
	void * buffer;
	char * ptr;
	int sz;
};

struct wb_list {
	struct write_buffer * head;
	struct write_buffer * tail;
};


struct socket {
	uintptr_t		opaque;		//logic data
	struct wb_list	high;		//high send buffer;
	struct wb_list	low;		//low send buffer;
	int64_t			wb_size;	//write buffer size;
	int				fd;			// socket handle.
	int				id;			//index for MAX_SOCKET;
	uint16_t		type;		// socket_type_xxx
	union {
		int size;
		//uint8_t udp_address[UDP_ADDRESS_SIZE];
	} p;
};

struct socket_server {
	poll_fd	event_fd;
	int		alloc_id;
	int		event_n;
	int		event_index;
	struct event ev[MAX_EVENT];
	struct socket slot[MAX_SOCKET];
	char	buffer[MAX_INFO];

	struct message_queue *queue;
};

struct send_object {
	void * buffer;
	int sz;
	void (*free_func)(void *);
};

struct request_connect {
	int			port;
	char		host[1];
};
struct request_listen {
	int			port;
	char		host[1];
};

static inline bool
send_object_init(struct socket_server *ss, struct send_object *so, void *object, int sz) {
	//if (sz < 0) {
	//	so->buffer = ss->soi.buffer(object);
	//	so->sz = ss->soi.size(object);
	//	so->free_func = ss->soi.free;
	//	return true;
	//} else {
		so->buffer = object;
		so->sz = sz;
		so->free_func = FREE;
		return false;
	//}
}

static inline void 
clear_wb_list(struct wb_list * list) {
	list->head = NULL;
	list->tail = NULL;
}

struct socket_server * socket_server_create() {
	struct socket_server * ss;
	struct message_queue * mq;
	int i;

	mq = mt_queue_create(0);
	if (mq == NULL)
		return NULL;

	ss = (struct socket_server *) MALLOC(sizeof(*ss));
	if (ss == NULL)
		return NULL;

	poll_fd efd = sp_create();
	if (sp_invalid(efd)) {
		return NULL;
	}

	ss->event_fd = efd;
	for (i = 0; i < MAX_SOCKET; ++i) {
		struct socket* s = &ss->slot[i];
		s->type = SOCKET_TYPE_INVALID;
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
	}
	ss->alloc_id	= 0;
	ss->event_n		= 0;
	ss->event_index = 0;
	ss->queue		= mq;
	memset(ss->buffer, 0, MAX_INFO);

	return ss;
}

static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {
	//if (wb->userobject) {
	//	ss->soi.free(wb->buffer);
	//} else {
		FREE(wb->buffer);
	//}
	FREE(wb);
}

static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {
	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		write_buffer_free(ss, tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}

static void 
force_close(struct socket_server * ss, struct socket * s, struct socket_message * result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	if (s->type == SOCKET_TYPE_INVALID) {
		return;
	}

	assert(s->type != SOCKET_TYPE_RESERVE);
	free_wb_list(ss, &s->high);
	free_wb_list(ss, &s->low);

	if (s->type != SOCKET_TYPE_PACCEPT && s->type != SOCKET_TYPE_PLISTEN) {
		sp_del(ss->event_fd, s->fd);
	}
	if (s->type != SOCKET_TYPE_BIND) {
		if (close(s->fd) < 0) {
			perror("close socket:");
		}
	}
	s->type = SOCKET_TYPE_INVALID;
}

static inline void
check_wb_list(struct wb_list *s) {
	assert(s->head == NULL);
	assert(s->tail == NULL);
}

void socket_server_release(struct socket_server * ss) {
	int i;
	struct socket_message dummy;
	for (i = 0; i < MAX_SOCKET; ++i) {
		struct socket * s = &ss->slot[i];
		if (s->type != SOCKET_TYPE_RESERVE) {
			force_close(ss, s, &dummy);
		}
	}

	sp_release(ss->event_fd);
	FREE(ss);
}


// return -1 (ignore) when error
static int
forward_message_tcp(struct socket_server *ss, struct socket *s, struct socket_message * result) {
	int sz = s->p.size;
	char * buffer = (char*)MALLOC(sz);
	int n = (int)read(s->fd, buffer, sz);
	if (n<0) {
		FREE(buffer);
		switch(errno) {
		case EINTR:
			break;
		case EAGAIN:
			fprintf(stderr, "socket-server: EAGAIN capture.\n");
			break;
		default:
			// close when error
			force_close(ss, s, result);
			result->data = strerror(errno);
			return INET_SOCKET_ERROR;
		}
		return -1;
	}
	if (n==0) {
		FREE(buffer);
		force_close(ss, s, result);
		return INET_SOCKET_CLOSE;
	}

	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		// discard recv data
		FREE(buffer);
		return -1;
	}

	if (n == sz) {
		s->p.size *= 2;
	} else if (sz > MIN_READ_BUFFER && n*2 < sz) {
		s->p.size /= 2;
	}

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = buffer;
	return INET_SOCKET_DATA;
}

static inline int
send_buffer_empty(struct socket *s) {
	return (s->high.head == NULL && s->low.head == NULL);
}

static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	int error;
	socklen_t len = sizeof(error);  
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  
	if (code < 0 || error) {  
		force_close(ss,s, result);
		if (code >= 0)
			result->data = strerror(error);
		else
			result->data = strerror(errno);
		return INET_SOCKET_ERROR;
	} else {
		s->type = SOCKET_TYPE_CONNECTED;
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		if (send_buffer_empty(s)) {
			sp_write(ss->event_fd, s->fd, s, false);
		}
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		if (getpeername(s->fd, &u.s, &slen) == 0) {
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result->data = ss->buffer;
				return INET_SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return INET_SOCKET_OPEN;
	}
}

static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		//int id = ATOM_INC(&(ss->alloc_id));
		int id = ss->alloc_id++;
		if (id < 0) {
			//id = ATOM_AND(&(ss->alloc_id), 0x7fffffff);
			id = (ss->alloc_id & 0x7fffffff);
		}
		struct socket *s = &ss->slot[HASH_ID(id)];
		if (s->type == SOCKET_TYPE_INVALID) {
			//if (ATOM_CAS(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {
			if (s->type == SOCKET_TYPE_INVALID)
			{
				s->type = SOCKET_TYPE_RESERVE;
				s->id = id;
				s->fd = -1;
				return id;
			} else {
				// retry
				--i;
			}
		}
	}
	return -1;
}

static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool add) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	assert(s->type == SOCKET_TYPE_RESERVE);

	if (add) {
		if (sp_add(ss->event_fd, fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	s->id = id;
	s->fd = fd;
//	s->protocol = protocol;
	s->p.size = MIN_READ_BUFFER;
	s->opaque = opaque;
	s->wb_size = 0;
	check_wb_list(&s->high);
	check_wb_list(&s->low);
	return s;
}


// return 0 when failed, or -1 when file limit
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);
	int client_fd = accept(s->fd, &u.s, &len);
	if (client_fd < 0) {

#if defined(__linux__)
		if (errno == EMFILE || errno == ENFILE) {
#else 
		if (0) {
#endif
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = strerror(errno);
			return -1;
		} else {
			return 0;
		}
	}
	int id = reserve_id(ss);
	if (id < 0) {
		close(client_fd);
		return 0;
	}
	//socket_keepalive(client_fd);
	sp_nonblocking(client_fd);
	struct socket *ns = new_fd(ss, id, client_fd, 0/*PROTOCOL_TCP*/, s->opaque, false);
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}
	ns->type = SOCKET_TYPE_PACCEPT;
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = id;
	result->data = NULL;

	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
	char tmp[INET6_ADDRSTRLEN];
	if (inet_ntop(u.s.sa_family, sin_addr, tmp, sizeof(tmp))) {
		snprintf(ss->buffer, sizeof(ss->buffer), "%s:%d", tmp, sin_port);
		result->data = ss->buffer;
	}

	return 1;
}

static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;
		for (;;) {
			int sz = write(s->fd, tmp->ptr, tmp->sz);
			if (sz < 0) {
				switch(errno) {
				case EINTR:
					continue;
				case EAGAIN:
					return -1;
				}
				force_close(ss,s, result);
				return INET_SOCKET_CLOSE;
			}
			s->wb_size -= sz;
			if (sz != tmp->sz) {
				tmp->ptr += sz;
				tmp->sz -= sz;
				return -1;
			}
			break;
		}
		list->head = tmp->next;
		write_buffer_free(ss,tmp);
	}
	list->tail = NULL;

	return -1;
}
static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	//if (s->protocol == PROTOCOL_TCP) {
		return send_list_tcp(ss, s, list, result);
	//} else {
		//return send_list_udp(ss, s, list, result);
	//}
}

static inline int
list_uncomplete(struct wb_list *s) {
	struct write_buffer *wb = s->head;
	if (wb == NULL)
		return 0;
	
	return (void *)wb->ptr != wb->buffer;
}

static void
raise_uncomplete(struct socket * s) {
	struct wb_list *low = &s->low;
	struct write_buffer *tmp = low->head;
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	// move head of low list (tmp) to the empty high list
	struct wb_list *high = &s->high;
	assert(high->head == NULL);

	tmp->next = NULL;
	high->head = high->tail = tmp;
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */
static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	assert(!list_uncomplete(&s->low));
	// step 1
	if (send_list(ss,s,&s->high,result) == INET_SOCKET_CLOSE) {
		return INET_SOCKET_CLOSE;
	}
	if (s->high.head == NULL) {
		// step 2
		if (s->low.head != NULL) {
			if (send_list(ss,s,&s->low,result) == INET_SOCKET_CLOSE) {
				return INET_SOCKET_CLOSE;
			}
			// step 3
			if (list_uncomplete(&s->low)) {
				raise_uncomplete(s);
			}
		} else {
			// step 4
			sp_write(ss->event_fd, s->fd, s, false);

			if (s->type == SOCKET_TYPE_HALFCLOSE) {
				force_close(ss, s, result);
				return INET_SOCKET_CLOSE;
			}
		}
	}

	return -1;
}


static int
do_connect(struct socket_server * ss, struct request_message* request,struct socket_message * result) {
	int id = request->session;
	result->opaque = request->source;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	
	assert(request->sz && request->data);
	struct request_connect * rc = (struct request_connect*)request->data;

	const char * ip		= rc->host;
	const int port		= rc->port;


	int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		result->data = strerror(errno);
		goto _failed;
	}

	sockaddr_in addr;
	addr.sin_family			= AF_INET;
	addr.sin_port			= htons(port);
	addr.sin_addr.s_addr	= inet_addr(ip);

	int status = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	if (INET_SOCKET_ERROR == status) {
		result->data = strerror(errno);
		goto _failed;
	}
	
	struct socket * ns = new_fd(ss, id, sock, 0, request->source, true);
	if (ns == NULL) {
		close(sock);
		result->data = "reach skynet socket number limit";
		goto _failed;
	}

	FREE(rc);

	if (status == 0) {
		ns->type = SOCKET_TYPE_CONNECTED;
		return INET_SOCKET_OPEN;
	} else {
		ns->type = SOCKET_TYPE_CONNECTING;
		sp_write(ss->event_fd, ns->fd, ns, true);
	}

	return -1;

_failed:
	FREE(rc);
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return INET_SOCKET_ERROR;
}

static int
do_listen(struct socket_server * ss, struct request_message * request, struct socket_message * result) {
	int id = request->session;
	result->opaque = request->source;
	result->id = id;
	result->ud = 0;
	result->data = NULL;

	assert(request->sz && request->data);
	struct request_listen * rl = (struct request_listen*)request->data;

	const char * addr	= rl->host;
	const int port		= rl->port;

	int listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSocket < 0) {
		result->data = strerror(errno);
		goto _failed;
	}

	sockaddr_in addrIn;
	addrIn.sin_family		= AF_INET;
	addrIn.sin_port			= htons(port);
	addrIn.sin_addr.s_addr	= htonl(INADDR_ANY);

	if (-1 == ::bind(listenSocket, reinterpret_cast<sockaddr*>(&addrIn), sizeof(addrIn))) {
		result->data = strerror(errno);
		goto _failed;
	}

	if (-1 == ::listen(listenSocket, SOMAXCONN)) {
		result->data = strerror(errno);
		goto _failed;
	}

	struct socket* s = new_fd(ss, id, listenSocket, 0, request->source, false);
	if (s == NULL) {
		goto _failed;
	}
	if (sp_add(ss->event_fd, s->fd, s)) {
		force_close(ss, s, result);
		result->data = strerror(errno);
		return INET_SOCKET_ERROR;
	}
	s->type = SOCKET_TYPE_LISTEN;
	s->opaque = request->source;
	result->data = "listen_start";
	return INET_SOCKET_OPEN;
	return -1;

_failed:
	FREE(rl);
	close(listenSocket);
	result->opaque = request->source;
	result->id = id;
	result->ud = 0;
	result->data = "reach socket number limit";
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return INET_SOCKET_ERROR;
}

static int 
do_closesocket(struct socket_server* ss, struct request_message *request, bool shutdown, struct socket_message *result) {
	int id = request->session;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		result->id = id;
		result->opaque = request->source;
		result->ud = 0;
		result->data = NULL;
		return INET_SOCKET_CLOSE;
	} 
	if (!send_buffer_empty(s)) {
		int type = send_buffer(ss, s, result);
		if (-1 != type) {
			return type;
		}
	}

	if (shutdown || send_buffer_empty(s)) {
		force_close(ss,s,result);
		result->id = id;
		result->opaque = request->source;
		return INET_SOCKET_CLOSE;
	}
	s->type = SOCKET_TYPE_HALFCLOSE;

	return -1;
}

static int
do_exit(struct socket_server* ss, struct request_message *request, struct socket_message *result) {
	result->opaque = 0;
	result->id = 0;
	result->ud = 0;
	result->data = NULL;
	return INET_SOCKET_EXIT;
}
static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_message * request, int size, int n) {
 	struct write_buffer * buf = (struct write_buffer *)MALLOC(size);
 	struct send_object so;
	send_object_init(ss, &so, request->data, request->sz);
 	buf->ptr = (char*)so.buffer+n;
 	buf->sz = so.sz - n;
 	buf->buffer = request->data;
 	buf->next = NULL;
 	if (s->head == NULL) {
 		s->head = s->tail = buf;
 	} else {
 		assert(s->tail != NULL);
 		assert(s->tail->next == NULL);
 		s->tail->next = buf;
 		s->tail = buf;
 	}
	return buf;
}
static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_message * request, int n) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, sizeof(write_buffer)/*SIZEOF_TCPBUFFER*/, n);
	s->wb_size += buf->sz;
}
/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.
 */
static int
do_send_socket(struct socket_server *ss, struct request_message * request, struct socket_message *result, int priority, const uint8_t *udp_addres) {
	int id = request->session;
	struct socket * s = &ss->slot[HASH_ID(id)];
	struct send_object so;
	send_object_init(ss, &so, request->data, request->sz);
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 
		|| s->type == SOCKET_TYPE_HALFCLOSE
		|| s->type == SOCKET_TYPE_PACCEPT) {
			so.free_func(request->data);
			return -1;
	}
	if (s->type == SOCKET_TYPE_LISTEN) {
		fprintf(stderr, "socket-server: write to listen fd %d.\n", id);
		so.free_func(request->data);
		return -1;
	}
	if (send_buffer_empty(s) && s->type == SOCKET_TYPE_CONNECTED) {
		//if (s->protocol == PROTOCOL_TCP) {
			int n = write(s->fd, (char *)so.buffer, so.sz);
			if (n<0) {
				switch(errno) {
				case EINTR:
				//case AGAIN_WOULDBLOCK:
					n = 0;
					break;
				default:
					fprintf(stderr, "socket-server: write to %d (fd=%d) error :%s.\n",id,s->fd,strerror(errno));
					force_close(ss,s,result);
					so.free_func(request->data);
					return INET_SOCKET_CLOSE;
				}
			}
			if (n == so.sz) {
				so.free_func(request->data);
				return -1;
			}
			append_sendbuffer(ss, s, request, n);	// add to high priority list, even priority == PRIORITY_LOW
		//} else {
		//	// udp
		//	if (udp_address == NULL) {
		//		udp_address = s->p.udp_address;
		//	}
		//	union sockaddr_all sa;
		//	socklen_t sasz = udp_socket_address(s, udp_address, &sa);
		//	int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
		//	if (n != so.sz) {
		//		append_sendbuffer_udp(ss,s,priority,request,udp_address);
		//	} else {
		//		so.free_func(request->buffer);
		//		return -1;
		//	}
		//}
		sp_write(ss->event_fd, s->fd, s, true);
	} else {
		//if (s->protocol == PROTOCOL_TCP) {
			//if (priority == PRIORITY_LOW) {
			//	append_sendbuffer_low(ss, s, request);
			//} else {
			//	append_sendbuffer(ss, s, request, 0);
			//}
		//} else {
		//	if (udp_address == NULL) {
		//		udp_address = s->p.udp_address;
		//	}
		//	append_sendbuffer_udp(ss,s,priority,request,udp_address);
		//}
	}
	return -1;

}


static int 
process_req(struct socket_server * ss, struct socket_message * result) {
	struct request_message req;
	while (!mq_queue_pop(ss->queue, &req)) {
		switch (req.type) {
		case SOCKET_REQ_CONNECT:
			if (do_connect(ss, &req, result))
				return 1;
			break;
		case SOCKET_REQ_LISTEN:
			if (do_listen(ss, &req, result))
				return 1;
			break;
		case SOCKET_REQ_EXIT:
			do_exit(ss, &req, result);
			break;
		case SOCKET_REQ_SEND:
			do_send_socket(ss, &req, result, 0, 0);
			break;
		case SOCKET_REQ_CLOSE:
			do_closesocket(ss, &req, false, result);
			break;
		case SOCKET_REQ_SHUTDOWN:
			do_closesocket(ss, &req, true, result);
			break;
		default:
			break;
		}
	}

	return 0;
}


int socket_server_poll(struct socket_server * ss, struct socket_message *result, int *more) {
	for (;;) {
		//1.cmd.
		if (process_req(ss, result)) {
			return 1;
		}

		//2.
		if (ss->event_n == ss->event_index) {
			if (process_req(ss, result)) {
				return 1;
			}

			ss->event_n = sp_wait(ss->event_fd, &ss->ev[0], MAX_EVENT);
			if (more) {
				*more = 0;
			}
			ss->event_index = 0;
			if (ss->event_n <= 0) {
				ss->event_n = 0;
				return -1;
			}
		}

		struct event * e = &ss->ev[ss->event_index++];
		struct socket * s = (struct socket *)e->s;
		if (s == NULL) {
			//dispatch pipe message at begin;
			continue;
		}

		switch (s->type) 
		{
		case SOCKET_TYPE_CONNECTING:
			return report_connect(ss, s, result);
			break;
		case SOCKET_TYPE_LISTEN:
			{
				int ok = report_accept(ss, s, result);
				if (ok > 0) {
					return INET_SOCKET_ACCEPT;
				} else if (ok < 0) {
					return INET_SOCKET_ERROR;
				}
				//when ok == 0, retry.
				break;
			}
		case SOCKET_TYPE_INVALID:
			fprintf(stderr, "socket-server: invalid socket!\n");
			break;
		default:
			if (e->read) {
				int type;
				type = forward_message_tcp(ss, s, result);
				if (e->write && type != INET_SOCKET_CLOSE && type != INET_SOCKET_ERROR) {
					// Try to dispatch write message next step if write flag set.
					e->read = false;
					--ss->event_index;
				}
				if (type == -1)
					break;
				return type;
			}
			if (e->write) {
				int type = send_buffer(ss, s, result);
				if (type == -1)
					break;
				return type;
			}
			break;
		}
	}
}


void socket_server_connect(struct socket_server * ss, uintptr_t opaque, const char * addr, int port)
{
	int len = strlen(addr);

	struct request_connect rc;
	rc.port		= port;
	memcpy(rc.host, addr, len);

	struct request_message sm;
	sm.type		= SOCKET_REQ_CONNECT;
	sm.source	= opaque;
	sm.session	= 0;
	sm.sz		= sizeof(rc) + len;
	sm.data		= MALLOC(sm.sz);
	memcpy(sm.data, &rc, sm.sz);

	mq_queue_push(ss->queue, &sm);
}

void socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog)
{
	int len = strlen(addr);
	struct request_listen rl;
	rl.port		= port;
	memcpy(rl.host, addr, len);

	struct request_message sm;
	sm.type	= SOCKET_REQ_LISTEN;
	sm.source	= opaque;
	sm.session = 0;
	sm.sz		= sizeof(rl) +len;
	sm.data	= MALLOC(sm.sz);
	memcpy(sm.data, &rl, sm.sz);

	mq_queue_push(ss->queue, &sm);
}

void socket_server_close(struct socket_server * ss, uintptr_t opaque, int id)
{
	struct request_message sm;
	sm.type = SOCKET_REQ_CLOSE;
	sm.source = opaque;
	sm.session = id;
	sm.sz = 0;
	sm.data = NULL;

	mq_queue_push(ss->queue, &sm);
}

void socket_server_shutdown(struct socket_server * ss, uintptr_t opaque, int id) {
	struct request_message sm;
	sm.type = SOCKET_REQ_SHUTDOWN;
	sm.source = opaque;
	sm.session = id;
	sm.sz = 0;
	sm.data = NULL;

	mq_queue_push(ss->queue, &sm);
}

//int64_t socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz)
void socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz)
{
	struct request_message sm;
	sm.source = 0;
	sm.session = id;
	sm.type = SOCKET_REQ_SEND;
	sm.sz = sz;
	sm.data = (void *)buffer;

	mq_queue_push(ss->queue, &sm);
}

