#include "socket_server.h"
#include "socket_poll.h"

#include <stdint.h> //uint8_t, uint32_t...
#include <stdio.h> //for fprintf.
#include <stdlib.h> //for malloc
#include <string.h> //for memset
#include <assert.h>

#define PROTOCOL_TCP 0
#define PROTOCOL_UDP 1
#define PROTOCOL_UDPv6 2

#define MAX_SOCKET_P 16
#define MAX_SOCKET (1<<MAX_SOCKET_P)
#define MAX_EVENT 64
#define MIN_READ_BUFFER 64

#define SOCKET_TYPE_INVALID 0
#define SOCKET_TYPE_RESERVE 1
#define SOCKET_TYPE_PLISTEN 2
#define SOCKET_TYPE_LISTEN 3
#define SOCKET_TYPE_CONNECTING 4
#define SOCKET_TYPE_CONNECTED 5
#define SOCKET_TYPE_HALFCLOSE 6
#define SOCKET_TYPE_PACCEPT 7
#define SOCKET_TYPE_BIND 8

#define PRIOROTY_HIGH 0
#define PRIOROTY_LOW 1

#define HASH_ID(id) (((unsigned int)id) % MAX_SOCKET)

#define UDP_ADDRESS_SIZE 19
#define MAX_INFO 128

//// EAGAIN and EWOULDBLOCK may be not the same value.
//#if (EAGAIN != EWOULDBLOCK)
//#define AGAIN_WOULDBLOCK EAGAIN : case EWOULDBLOCK
//#else
#define AGAIN_WOULDBLOCK EAGAIN
//#endif

#define MALLOC malloc
#define FREE free

struct write_buffer
{
	struct write_buffer * next;
	void * buffer;
	char * ptr;
	int sz;
	bool userobject;
	uint8_t udp_address[UDP_ADDRESS_SIZE];
};

#define SIZEOF_TCPBUFFER (offsetof(struct write_buffer, udp_address[0]))
#define SIZEOF_UDPBUFFER (sizeof(struct write_buffer))

struct wb_list {
	struct write_buffer * head;
	struct write_buffer * tail;
};

struct socket {
	uintptr_t		opaque;		//?
	struct wb_list	high;		//high send buffer;
	struct wb_list	low;		//low send buffer;
	int64_t			wb_size;	//write buffer size;
	int				fd;			// socket handle.
	int				id;			//index for MAX_SOCKET;
	uint16_t		protocol;	//tcp,udp,updv6
	uint16_t		type;		//?
	union {
		int size;				//?
		uint8_t udp_address[UDP_ADDRESS_SIZE];
	} p;
};

struct socket_server {
	poll_fd event_fd;
	int alloc_id;
	int event_n;
	int event_index;
	struct socket_object_interface soi;
	struct event ev[MAX_EVENT];
	struct socket slot[MAX_SOCKET];
	char buffer[MAX_INFO];
	struct skynet_mq * request;

	int checkctrl;
};

//////////////////////////////////////////////////////////////////////////
// request event struct.
struct request_open {
	int id;
	int port;
	uintptr_t opaque;
	char host[1];
};

struct request_send {
	int id;
	int sz;
	char * buffer;
};

struct request_send_udp {
	int id;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_setudp {
	int id;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_close {
	int id;
	int shutdown;
	uintptr_t opaque;
};

struct request_listen {
	int id;
	int fd;
	uintptr_t opaque;
};

struct request_bind {
	int id;
	int fd;
	uintptr_t opaque;
};

struct request_start {
	int id;
	uintptr_t opaque;
};

struct request_setopt {
	int id;
	int what;
	int value;
};

struct request_udp {
	int id;
	int fd;
	int family;
	uintptr_t opaque;
};
// request event struct.
//////////////////////////////////////////////////////////////////////////
union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

//////////////////////////////////////////////////////////////////////////
// the first byte is TYPE
// 
// S start socket;
// B bind socket;
// L listen socket
// O connect to (open)
// X exit
// D send packet(high)
// P send packet(low)
// A send Udp packet;
// T set opt
// U create udp socket
//C set udp address.

struct request_packet {
	uint8_t header[8]; // 6 bytes dummy
	union {
		char buffer[256];
		struct request_open open;
		struct request_send send;
		struct request_send_udp send_udp;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_start start;
		struct request_setopt setopt;
		struct request_udp udp;
		struct request_setudp set_udp;
	};
	uint8_t dummy[256];
};

// union socketaddr_all {
// 	struct socketaddr s;
// 	struct socketaddr_in v4;
// 	struct socketaddr_in6 v6;
// };

struct send_object {
	void * buffer;
	int sz;
	void (*free_func)(void *);
};


//////////////////////////////////////////////////////////////////////////
// inner function;
static inline void
write_buffer_free(struct socket_server * ss, struct write_buffer * wb) {
	if (wb->userobject) {
		ss->soi.free(wb->buffer);
	} else {
		FREE(wb->buffer);
	}
	FREE(wb);
}

static inline void 
clear_wb_list(struct wb_list * list) {
	list->head = NULL;
	list->tail = NULL;
}

static void 
free_wb_list(struct socket_server* ss, struct wb_list * list) {
	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		write_buffer_free(ss, tmp);
	}
	clear_wb_list(list);
}


//////////////////////////////////////////////////////////////////////////
// api.
struct socket_server * socket_server_create() {
	int i = 0;
	poll_fd efd = sp_create();
	if (sp_invalid(efd)) {
		fprintf(stderr, "socket-server: create event poll failed.\n");
		return NULL;
	}

	struct socket_server * ss = (struct socket_server *)MALLOC(sizeof(*ss));
	ss->event_fd = efd;
	
	for (int i = 0; i < MAX_SOCKET; ++i) {
		struct socket* s = &ss->slot[i];
		s->type = SOCKET_TYPE_INVALID;
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
	}
	ss->alloc_id = 0;
	ss->event_n = 0;
	ss->event_index = 0;
	memset(&ss->soi, 0, sizeof(ss->soi));

	return ss;
}

static inline void 
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
static inline void
check_wb_list(struct wb_list * list) {
	assert(list->head == NULL);
	assert(list->tail == NULL);
}

static struct socket*
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool add) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	assert(s->type == SOCKET_TYPE_INVALID);

	if (add) {
		if (sp_add(ss->event_fd, fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	s->id = id;
	s->fd = fd;
	s->protocol = protocol;
	s->p.size = MIN_READ_BUFFER;
	s->opaque = opaque;
	s->wb_size = 0;
	check_wb_list(&s->high);
	check_wb_list(&s->low);
	return s;
}

static int
open_socket(struct socket_server * ss, struct request_open * request, struct socket_message * result) {
	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	struct socket * ns;
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;
	char port[16];
	sprintf(port, "%d", request->port);
	memset(&ai_hints, 0, sizeof(ai_hints));
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	status = getaddrinfo( request->host, port, &ai_hints, &ai_list );
	if ( status != 0 ) {
		result->data = (char *)gai_strerror(status);
		goto _failed;
	}
	int sock= -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock < 0 ) {
			continue;
		}
		//socket_keepalive(sock);
		sp_noblocking(sock);
		status = connect( sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if ( status != 0 && errno != EINPROGRESS) {
			close(sock);
			sock = -1;
			continue;
		}
		break;
	}

	if (sock < 0) {
		result->data = strerror(errno);
		goto _failed;
	}

	ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque, true);
	if (ns == NULL) {
		close(sock);
		result->data = "reach skynet socket number limit";
		goto _failed;
	}

	if(status == 0) {
		ns->type = SOCKET_TYPE_CONNECTED;
		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;
		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			result->data = ss->buffer;
		}
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	} else {
		ns->type = SOCKET_TYPE_CONNECTING;
		sp_write(ss->event_fd, ns->fd, ns, true);
	}

	freeaddrinfo( ai_list );
	return -1;
_failed:
	freeaddrinfo( ai_list );
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERROR;
}

static int send_list_tcp(struct socket_server * ss, struct socket * s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;
		for (;;) {
			int sz = write(s->fd, tmp->ptr, tmp->sz);
			if (sz < 0) {
				switch(errno) {
				case -1/*EINTR*/:
					continue;
				case AGAIN_WOULDBLOCK:
					return -1;
				}
				force_close(ss, s, result);
				return SOCKET_CLOSE;
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
		write_buffer_free(ss, tmp);
	}
	list->tail = NULL;
}

static int
send_list(struct socket_server * ss, struct socket * s, struct wb_list * list, struct socket_message * result) {
	if (s->protocol == PROTOCOL_TCP) {
		return send_list_tcp(ss, s, list, result);
	} else {

	}
}

static inline int
list_uncomplete(struct wb_list * s) {
	struct write_buffer * wb = s->head;
	if (wb == NULL)
		return 0;
	return (void*)wb->ptr != wb->buffer;
}

static void 
raise_uncomplete(struct socket *s ) {
	struct wb_list * low = &s->low;
	struct write_buffer * tmp = low->head;
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	//move head of low list (tmp) to the empty high list
	struct wb_list * high = &s->high;
	assert(high->head == NULL);

	tmp->next = NULL;
	high->head = high->tail = tmp;
}

static inline int
send_buffer_empty(struct socket * s) {
	return (s->high.head == NULL && s->low.head == NULL);
}

static int 
report_connect(struct socket_server * ss, struct socket * s, struct socket_message * result) {
	int error;
	socklen_t len = sizeof(error);
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);
	if (code < 0 || error) {
		force_close(ss, s, result);
		if (code >= 0)
			result->data = strerror(error);
		else
			result->data = strerror(errno);
		return SOCKET_ERROR;
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
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return SOCKET_OPEN;
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
			id = ss->alloc_id & 0x7fffffff;
		}
		struct socket *s = &ss->slot[HASH_ID(id)];
		if (s->type == SOCKET_TYPE_INVALID) {
			//if (ATOM_CAS(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {
			if (s->type == SOCKET_TYPE_INVALID) {
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

//return 0 when failed or -1 when limit.????
static int
report_accept(struct socket_server* ss, struct socket * s, struct socket_message * result) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);
	int client_fd = (int)accept(s->fd, &u.s, &len);
	if (client_fd < 0) {
#ifndef WIN32
		if (errno == EMFILE || errno == ENFILE) {
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = strerror(errno);
			return -1;
		} else {
			return 0;
		}
#else
		return 0;
#endif
	}

	int id = reserve_id(ss);
	if (id < 0) {
		close(client_fd);
		return 0;
	}

//	sockat_keepalive(client_fd);
	sp_noblocking(client_fd);
	struct socket* ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);
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

// return -1 (ignore) when error.
static int 
forward_message_tcp(struct socket_server * ss, struct socket * s, struct socket_message * result) {
	int sz = s->p.size;
	char * buffer = (char *)MALLOC(sz);
	int n = (int)read(s->fd, buffer, sz);
	if (n < 0) {
		FREE(buffer);
		switch(errno) {
		case /*EINTR*/-1:
			break;
		case AGAIN_WOULDBLOCK:
			fprintf(stderr, "socket-server: EAGAIN capture.\n");
			break;
		default:
			//close when error.
			force_close(ss, s, result);
			result->data = strerror(errno);
			return SOCKET_ERROR;
		}
		return -1;
	}

	if (n == 0) {
		FREE(buffer);
		force_close(ss, s, result);
		return SOCKET_CLOSE;
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
	return SOCKET_DATA;
}


/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */
static int 
send_buffer(struct socket_server* ss, struct socket * s, struct socket_message * result) {
	assert(!list_uncomplete(&s->low));
	// step 1
	if (send_list(ss,s,&s->high,result) == SOCKET_CLOSE) {
		return SOCKET_CLOSE;
	}
	if (s->high.head == NULL) {
		// step 2
		if (s->low.head != NULL) {
			if (send_list(ss,s,&s->low,result) == SOCKET_CLOSE) {
				return SOCKET_CLOSE;
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
				return SOCKET_CLOSE;
			}
		}
	}

	return -1;
}

int socket_server_poll(struct socket_server * ss, struct socket_message *result, int *more)
{
	for (;;)
	{
		if (ss->checkctrl) {

		}

		if (ss->event_index == ss->event_n)
		{
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
			ss->checkctrl = 1;
			if (more) {
				*more = 0;
			}

			ss->event_index = 0;
			if (ss->event_n <= 0) {
				ss->event_n = 0;
				return -1;
			}
		}

		struct event * e = &ss->ev[ss->event_index];
		struct socket * s = (struct socket *)e->s;
		if (s == NULL) {
			continue;
		}

		switch (s->type) {
		case SOCKET_TYPE_CONNECTING:
			return report_connect(ss, s, result);
		case SOCKET_TYPE_LISTEN: 
		{
			int ok = report_accept(ss, s, result);
			if (ok > 0) {
				return SOCKET_ACCEPT; //³É¹¦;
			} if (ok < 0) {
				return SOCKET_ERROR; //limit.
			}

			//when ok == 0.retry; some thing error!.
			break;
		}
		case SOCKET_TYPE_INVALID:
			fprintf(stderr, "socket-server: invalid socket\n");
			break;
		default:
			if (e->read) {
				int type;
				if (s->protocol == PROTOCOL_TCP) {
					type = forward_message_tcp(ss, s, result);
				} else {
					//type = forward_message_udp(ss, s, result);
					//if (type == SOCKET_UDP) {
					//	// try read again
					//	--ss->event_index;
					//	return SOCKET_UDP;
					//}
				}

				if (e->write && type != SOCKET_CLOSE && type != SOCKET_ERROR) {
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