#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

struct epoll_struct
{
	int efd;
};

static bool 
sp_invalid(poll_fd pfd) {
	epoll_struct* es = (epoll_struct*)pfd;
	return es->efd == -1;
}

static poll_fd
sp_create() {
	epoll_struct* p = new epoll_struct;
	p->efd = epoll_create(1024);

	return p;
}

static void
sp_release(poll_fd pfd) {
	epoll_struct *es = (epoll_struct *)pfd;
	close(es->efd);
}

static int 
sp_add(poll_fd pfd, int sock, void *ud) {
	epoll_struct *es = (epoll_struct *)pfd;
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = ud;
	if (epoll_ctl(es->efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
		return 1;
	}
	return 0;
}

static void 
sp_del(poll_fd pfd, int sock) {
	epoll_struct *es = (epoll_struct *)pfd;
	epoll_ctl(es->efd, EPOLL_CTL_DEL, sock , NULL);
}

static void 
sp_write(poll_fd pfd, int sock, void *ud, bool enable) {
	epoll_struct *es = (epoll_struct *)pfd;

	struct epoll_event ev;
	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;
	epoll_ctl(es->efd, EPOLL_CTL_MOD, sock, &ev);
}

static int 
sp_wait(poll_fd pfd, struct event *e, int max) {
	epoll_struct *es = (epoll_struct *)pfd;

	struct epoll_event ev[max];
	int n = epoll_wait(es->efd , ev, max, -1);
	int i;
	for (i=0;i<n;i++) {
		e[i].s = ev[i].data.ptr;
		unsigned flag = ev[i].events;
		e[i].write = (flag & EPOLLOUT) != 0;
		e[i].read = (flag & EPOLLIN) != 0;
	}

	return n;
}

static void
sp_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if ( -1 == flag ) {
		return;
	}

	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
