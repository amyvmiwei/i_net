#ifndef __INET_BASE_H__
#define __INET_BASE_H__

#include <stdint.h>

#define INET_SOCKET_SUCCESS -1
#define INET_SOCKET_DATA 0
#define INET_SOCKET_CLOSE 1
#define INET_SOCKET_ACCEPT 2
#define INET_SOCKET_ERROR 3
#define INET_SOCKET_EXIT 4
#define INET_SOCKET_OPEN 5

enum SOCKET_REQ
{
	SOCKET_REQ_INVALID = -1,
	SOCKET_REQ_CONNECT,
	SOCKET_REQ_LISTEN,
	SOCKET_REQ_EXIT,		//...
	SOCKET_REQ_SEND,
	SOCKET_REQ_CLOSE,
	SOCKET_REQ_SHUTDOWN,
	SOCKET_REQ_COUNT,
};

struct socket_server;
struct socket_message {
	int id;
	uintptr_t opaque;
	int ud;
	char *data;
};

struct socket_server * socket_server_create();
void socket_server_release(struct socket_server *);
int socket_server_poll(struct socket_server *, struct socket_message *result, int *more);

void socket_server_exit(struct socket_server*); //....
void socket_server_close(struct socket_server *, uintptr_t opaque, int id);
void socket_server_shutdown(struct socket_server*, uintptr_t opaque, int id);

// return -1 when error
//int64_t socket_server_send(struct socket_server *, int id, const void * buffer, int sz);
void socket_server_send(struct socket_server *, int id, const void * buffer, int sz);

// ctrl command below returns id
void socket_server_listen(struct socket_server *, uintptr_t opaque, const char * addr, int port, int backlog);
void socket_server_connect(struct socket_server *, uintptr_t opaque, const char * addr, int port);


#endif