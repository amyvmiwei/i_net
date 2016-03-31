#ifndef socket_select_h
#define socket_select_h

struct socket_unit {
	int fd;
	bool write;
	void * ud;
};

struct aeApiState {
	fd_set rfds, wfds;
	struct socket_unit sockets[FD_SETSIZE];
	int n;
};

#define zmalloc malloc
#define zfree free

bool sp_invalid(poll_fd fd) {return true;}

poll_fd sp_create() 
{
	aeApiState* state = (aeApiState*)zmalloc(sizeof(*state));
	if (!state) return NULL;

	state->n = 0;
	memset(state->sockets, 0, sizeof(struct socket_unit)*FD_SETSIZE);
	FD_ZERO(&state->rfds);
	FD_ZERO(&state->wfds);
	return state;
}
void sp_release(poll_fd fd) 
{
	aeApiState *state = (aeApiState *)fd;
	zfree(state);
}

int sp_add(poll_fd fd, int sock, void * ud) 
{
	aeApiState *state = (aeApiState *)fd;
#ifdef WIN32
	if (state->n >= FD_SETSIZE-1)
#else
	if (socket >= FD_SETSIZE)
		return -1;
#endif

	if (sock < 0)
		return 1;

	int i;
	for (i = 0 ; i < state->n; ++i) {
		struct socket_unit* un = &state->sockets[i];
		if (un->fd == sock) {
			un->ud = ud;
			un->write = false;
			return 0;
		}
	}

	struct socket_unit *un = &state->sockets[state->n++];
	un->fd = sock;
	un->ud = ud;
	un->write = false;

	return 0;
}

void sp_del(poll_fd fd, int sock)
{
	aeApiState *state = (aeApiState*)fd;

	int i;
	for (i = 0; i < state->n; ++i) {
		struct socket_unit * su = &state->sockets[i];
		if (su->fd == sock) {
			--state->n;

			while(i < state->n) { //是不是有问题??
				state->sockets[i] = state->sockets[i+1]; //.
				++i;
			}
			return;
		}
	}
}

void sp_write(poll_fd fd, int sock, void * ud, bool enable) 
{
	aeApiState *state = (aeApiState *)fd;

	int i;
	for (i = 0; i < state->n; ++i) {
		struct socket_unit *su = &state->sockets[i];
		if (su->fd == sock) {
			su->fd = sock;
			su->ud = ud;
			su->write = enable;
			return;
		}
	}
}

int sp_wait(poll_fd fd, struct event* e, int max) 
{
	aeApiState* state = (aeApiState *)fd;

	int i;
	int maxfd = 0;
	FD_ZERO(&state->rfds);
	FD_ZERO(&state->wfds);
	//FD_SET(state->);
	for (i = 0; i < state->n; ++i) {
		struct socket_unit *su = &state->sockets[i];
		if (su->fd > maxfd) {
			maxfd = su->fd;
		}

		FD_SET(su->fd, &state->rfds);
		if(su->write) {
			FD_SET(su->fd, &state->wfds);
		}
	}

	int n = select(maxfd + 1, &state->rfds, &state->wfds, NULL, NULL);
	if (n <= 0) {
		return 0;
	}
	
	int retn = 0;
	for (i = 0; i < state->n && max > 0 && n > 0 ; i++) {
		struct socket_unit *su = &state->sockets[i];
		int fd = su->fd;
		bool set = false;
		if (FD_ISSET(fd, &state->rfds)) {
			e[retn].s = su->ud;
			e[retn].read = true;
			set = true;
		}
		if (su->write && FD_ISSET(fd, &state->wfds)) {
			e[retn].s = su->ud;
			e[retn].write = true;
			set = true;
		}
		if (set) {
			++retn;
			--max;
			--n;
		}
	}
	return retn;
}

void sp_nonblocking(int sock) {}

#endif
