#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sched.h>
#include <assert.h>

#include "cpu.h"
#include "http_parsing.h"
#include "netlib.h"
#include "debug.h"

#define MAX_FLOW_NUM  (10000)

#define RCVBUF_SIZE (2*1024)
#define SNDBUF_SIZE (8*1024)

#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define HTTP_HEADER_LEN 1024
#define URL_LEN 128

#define MAX_FILES 30

#define NAME_LIMIT 256
#define FULLNAME_LIMIT 512

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#ifndef MAX_CPUS
#define MAX_CPUS		16
#endif

/*----------------------------------------------------------------------------*/
struct file_cache
{
	char name[NAME_LIMIT];
	char fullname[FULLNAME_LIMIT];
	uint64_t size;
	char *file;
};

/*----------------------------------------------------------------------------*/
struct server_vars
{
	char request[HTTP_HEADER_LEN];
	int recv_len;
	int request_len;
	long int total_read, total_sent;
	uint8_t done;
	uint8_t rspheader_sent;
	uint8_t keep_alive;

	int fidx;						// file cache index
	char fname[NAME_LIMIT];				// file name
	long int fsize;					// file size
};

/*----------------------------------------------------------------------------*/
struct thread_context
{
	int core;
	int listener;				// 每个线程自己的 listener（SO_REUSEPORT）
	int ep;				// 每个线程自己的 epoll
	struct server_vars *svars;
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	uint64_t rx_calls;
	uint64_t tx_calls;
	uint64_t total_requests;
	uint64_t http_200;
	uint64_t http_404;
	uint64_t accepts;
	uint64_t closes;
	uint64_t errors;
	volatile int active_conns;
};

/*----------------------------------------------------------------------------*/
static int num_cores;
static int core_limit;
static pthread_t app_thread[MAX_CPUS];
static int done[MAX_CPUS];
static int backlog = -1;
static struct thread_context *thread_ctx_list[MAX_CPUS];
static pthread_t stats_thread;
static volatile int stop_stats;
static int stats_thread_started;

/*----------------------------------------------------------------------------*/
const char *www_main;
static struct file_cache fcache[MAX_FILES];
static int nfiles;

/*----------------------------------------------------------------------------*/
static int finished;

/*----------------------------------------------------------------------------*/
static char *
StatusCodeToString(int scode)
{
	switch (scode) {
		case 200:
			return "OK";
			break;

		case 404:
			return "Not Found";
			break;
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
void
CleanServerVariable(struct server_vars *sv)
{
	sv->recv_len = 0;
	sv->request_len = 0;
	sv->total_read = 0;
	sv->total_sent = 0;
	sv->done = 0;
	sv->rspheader_sent = 0;
	sv->keep_alive = 0;
}

/*----------------------------------------------------------------------------*/
void 
CloseConnection(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	epoll_ctl(ctx->ep, EPOLL_CTL_DEL, sockid, NULL);
	close(sockid);
	if (ctx) {
		__sync_fetch_and_add(&ctx->closes, 1);
		int active = __sync_add_and_fetch(&ctx->active_conns, -1);
		if (active < 0) {
			__sync_add_and_fetch(&ctx->active_conns, 1);
		}
	}
}

/*----------------------------------------------------------------------------*/
static int 
SendUntilAvailable(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	int ret;
	int sent;
	int len;

	if (sv->done || !sv->rspheader_sent) {
		return 0;
	}

	sent = 0;
	ret = 1;
	while (ret > 0) {
		len = MIN(SNDBUF_SIZE, sv->fsize - sv->total_sent);
		if (len <= 0) {
			break;
		}
		ret = write(sockid, fcache[sv->fidx].file + sv->total_sent, len);
		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}
			TRACE_APP("Connection closed with client.\n");
			__sync_fetch_and_add(&ctx->errors, 1);
			break;
		}
		TRACE_APP("Socket %d: write try: %d, ret: %d\n", sockid, len, ret);
		__sync_fetch_and_add(&ctx->tx_bytes, ret);
		__sync_fetch_and_add(&ctx->tx_calls, 1);
		sent += ret;
		sv->total_sent += ret;
	}

	if (sv->total_sent >= fcache[sv->fidx].size) {
		struct epoll_event ev;
		sv->done = TRUE;
		finished++;

		if (sv->keep_alive) {
			/* if keep-alive connection, wait for the incoming request */
			ev.events = EPOLLIN;
			ev.data.fd = sockid;
			epoll_ctl(ctx->ep, EPOLL_CTL_MOD, sockid, &ev);

			CleanServerVariable(sv);
		} else {
			/* else, close connection */
			CloseConnection(ctx, sockid, sv);
		}
	}

	return sent;
}

/*----------------------------------------------------------------------------*/
static int 
HandleReadEvent(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	struct epoll_event ev;
	char buf[HTTP_HEADER_LEN];
	char url[URL_LEN];
	char response[HTTP_HEADER_LEN];
	int scode;						// status code
	time_t t_now;
	char t_str[128];
	char keepalive_str[128];
	int rd;
	int i;
	int len;
	int sent;

	/* HTTP request handling */
	rd = read(sockid, buf, HTTP_HEADER_LEN);
	if (rd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			__sync_fetch_and_add(&ctx->errors, 1);
		}
		return rd;
	}
	if (rd == 0) {
		return rd;
	}
	__sync_fetch_and_add(&ctx->rx_bytes, rd);
	__sync_fetch_and_add(&ctx->rx_calls, 1);
	memcpy(sv->request + sv->recv_len, 
		(char *)buf, MIN(rd, HTTP_HEADER_LEN - sv->recv_len));
	sv->recv_len += rd;
	sv->request_len = find_http_header(sv->request, sv->recv_len);
	if (sv->request_len <= 0) {
		TRACE_ERROR("Socket %d: Failed to parse HTTP request header.\n"
				"read bytes: %d, recv_len: %d, "
				"request_len: %d, strlen: %ld, request: \n%s\n", 
				sockid, rd, sv->recv_len, 
				sv->request_len, strlen(sv->request), sv->request);
		return rd;
	}

	http_get_url(sv->request, sv->request_len, url, URL_LEN);
	TRACE_APP("Socket %d URL: %s\n", sockid, url);
	sprintf(sv->fname, "%s%s", www_main, url);
	TRACE_APP("Socket %d File name: %s\n", sockid, sv->fname);

	sv->keep_alive = FALSE;
	if (http_header_str_val(sv->request, "Connection: ", 
				strlen("Connection: "), keepalive_str, 128)) {	
		if (strstr(keepalive_str, "Keep-Alive")) {
			sv->keep_alive = TRUE;
		} else if (strstr(keepalive_str, "Close")) {
			sv->keep_alive = FALSE;
		}
	}

	/* Find file in cache */
	scode = 404;
	for (i = 0; i < nfiles; i++) {
		if (strcmp(sv->fname, fcache[i].fullname) == 0) {
			sv->fsize = fcache[i].size;
			sv->fidx = i;
			scode = 200;
			break;
		}
	}
	TRACE_APP("Socket %d File size: %ld (%ldMB)\n", 
			sockid, sv->fsize, sv->fsize / 1024 / 1024);

	/* Response header handling */
	time(&t_now);
	strftime(t_str, 128, "%a, %d %b %Y %X GMT", gmtime(&t_now));
	if (sv->keep_alive)
		sprintf(keepalive_str, "Keep-Alive");
	else
		sprintf(keepalive_str, "Close");

	sprintf(response, "HTTP/1.1 %d %s\r\n"
			"Date: %s\r\n"
			"Server: Webserver on Native Linux TCP (SO_REUSEPORT)\r\n"
			"Content-Length: %ld\r\n"
			"Connection: %s\r\n\r\n", 
			scode, StatusCodeToString(scode), t_str, sv->fsize, keepalive_str);
	len = strlen(response);
	TRACE_APP("Socket %d HTTP Response: \n%s", sockid, response);
	sent = write(sockid, response, len);
	TRACE_APP("Socket %d Sent response header: try: %d, sent: %d\n", 
			 sockid, len, sent);
	if (sent > 0) {
		__sync_fetch_and_add(&ctx->tx_bytes, sent);
		__sync_fetch_and_add(&ctx->tx_calls, 1);
	} else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		__sync_fetch_and_add(&ctx->errors, 1);
	}
	if (sent != len) {
		TRACE_ERROR("Failed to send complete response header\n");
	}
	sv->rspheader_sent = TRUE;

	ev.events = EPOLLIN | EPOLLOUT;
	ev.data.fd = sockid;
	epoll_ctl(ctx->ep, EPOLL_CTL_MOD, sockid, &ev);

	SendUntilAvailable(ctx, sockid, sv);

	__sync_fetch_and_add(&ctx->total_requests, 1);
	if (scode == 200) {
		__sync_fetch_and_add(&ctx->http_200, 1);
	} else {
		__sync_fetch_and_add(&ctx->http_404, 1);
	}

	return rd;
}

/*----------------------------------------------------------------------------*/
int 
AcceptConnection(struct thread_context *ctx, int listener)
{
	struct server_vars *sv;
	struct epoll_event ev;
	int c;
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	int flags;
	int nodelay = 1;
	int quickack = 1;

	/* Use accept4() with SOCK_NONBLOCK for better performance */
	c = accept4(listener, (struct sockaddr *)&addr, &addrlen, SOCK_NONBLOCK);
	if (c < 0) {
		/* Fallback to accept() if accept4() not available */
		c = accept(listener, (struct sockaddr *)&addr, &addrlen);
		if (c >= 0) {
			flags = fcntl(c, F_GETFL, 0);
			fcntl(c, F_SETFL, flags | O_NONBLOCK);
		}
	}

	if (c >= 0) {
		if (c >= MAX_FLOW_NUM) {
			TRACE_ERROR("Invalid socket id %d.\n", c);
			close(c);
			return -1;
		}

		/* TCP optimizations: disable Nagle algorithm and enable quick ACK */
		/* These optimizations don't affect fairness comparison */
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
		setsockopt(c, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));

		sv = &ctx->svars[c];
		CleanServerVariable(sv);
		__sync_fetch_and_add(&ctx->accepts, 1);
		__sync_fetch_and_add(&ctx->active_conns, 1);
		TRACE_INFO("Accepted new connection %d on core %d\n", c, ctx->core);
		ev.events = EPOLLIN;  /* Level-triggered mode */
		ev.data.fd = c;
		epoll_ctl(ctx->ep, EPOLL_CTL_ADD, c, &ev);
		TRACE_APP("Socket %d registered.\n", c);

	} else {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			TRACE_ERROR("accept() error %s\n", 
					strerror(errno));
		}
	}

	return c;
}

/*----------------------------------------------------------------------------*/
struct thread_context *
InitializeServerThread(int core)
{
	struct thread_context *ctx;
	cpu_set_t cpuset;

	/* affinitize application thread to a CPU core */
	CPU_ZERO(&cpuset);
	CPU_SET(core, &cpuset);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
		perror("pthread_setaffinity_np");
	}

	ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
	if (!ctx) {
		TRACE_ERROR("Failed to create thread context!\n");
		return NULL;
	}
	ctx->core = core;

	/* create epoll descriptor */
	ctx->ep = epoll_create1(0);
	if (ctx->ep < 0) {
		perror("epoll_create1");
		free(ctx);
		TRACE_ERROR("Failed to create epoll descriptor!\n");
		return NULL;
	}

	/* allocate memory for server variables */
	ctx->svars = (struct server_vars *)
			calloc(MAX_FLOW_NUM, sizeof(struct server_vars));
	if (!ctx->svars) {
		close(ctx->ep);
		free(ctx);
		TRACE_ERROR("Failed to create server_vars struct!\n");
		return NULL;
	}

	return ctx;
}

/*----------------------------------------------------------------------------*/
int 
CreateListeningSocket(struct thread_context *ctx)
{
	int listener;
	struct epoll_event ev;
	struct sockaddr_in saddr;
	int ret;
	int flags;
	int reuse = 1;

	/* create socket and set it as nonblocking */
	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		perror("socket");
		TRACE_ERROR("Failed to create listening socket!\n");
		return -1;
	}

	/* Set SO_REUSEPORT for load balancing across threads */
	ret = setsockopt(listener, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
	if (ret < 0) {
		perror("setsockopt SO_REUSEPORT");
		TRACE_ERROR("Failed to set SO_REUSEPORT (may not be supported on this kernel)\n");
		/* Continue anyway, but without SO_REUSEPORT */
	}

	flags = fcntl(listener, F_GETFL, 0);
	ret = fcntl(listener, F_SETFL, flags | O_NONBLOCK);
	if (ret < 0) {
		perror("fcntl");
		TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
		close(listener);
		return -1;
	}

	/* bind to port 80 */
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = htons(80);
	ret = bind(listener, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		perror("bind");
		TRACE_ERROR("Failed to bind to the listening socket!\n");
		close(listener);
		return -1;
	}

	/* listen (backlog: can be configured) */
	ret = listen(listener, backlog);
	if (ret < 0) {
		perror("listen");
		TRACE_ERROR("listen() failed!\n");
		close(listener);
		return -1;
	}
	
	/* wait for incoming accept events */
	ev.events = EPOLLIN;
	ev.data.fd = listener;
	epoll_ctl(ctx->ep, EPOLL_CTL_ADD, listener, &ev);

	TRACE_INFO("Thread %d: Listener socket %d created with SO_REUSEPORT\n", 
			ctx->core, listener);

	return listener;
}

/*----------------------------------------------------------------------------*/
static void *
StatsThread(void *arg)
{
	uint64_t prev_rx_bytes[MAX_CPUS] = {0};
	uint64_t prev_tx_bytes[MAX_CPUS] = {0};
	uint64_t prev_rx_calls[MAX_CPUS] = {0};
	uint64_t prev_tx_calls[MAX_CPUS] = {0};
	uint64_t prev_errors[MAX_CPUS] = {0};
	uint64_t prev_requests[MAX_CPUS] = {0};
	struct timespec prev_ts, now_ts;

	(void)arg;

	clock_gettime(CLOCK_MONOTONIC, &prev_ts);

	while (!stop_stats) {
		sleep(1);
		clock_gettime(CLOCK_MONOTONIC, &now_ts);

		double interval = (now_ts.tv_sec - prev_ts.tv_sec) +
			(now_ts.tv_nsec - prev_ts.tv_nsec) / 1000000000.0;
		if (interval <= 0.0)
			interval = 1.0;

		double total_rx_bytes = 0.0;
		double total_tx_bytes = 0.0;
		uint64_t total_rx_calls = 0;
		uint64_t total_tx_calls = 0;
		uint64_t total_errors = 0;
		uint64_t total_requests = 0;
		int total_flows = 0;

		for (int i = 0; i < core_limit; i++) {
			struct thread_context *ctx = thread_ctx_list[i];
			if (!ctx)
				continue;

			uint64_t rx_bytes = __sync_fetch_and_add(&ctx->rx_bytes, 0);
			uint64_t tx_bytes = __sync_fetch_and_add(&ctx->tx_bytes, 0);
			uint64_t rx_calls = __sync_fetch_and_add(&ctx->rx_calls, 0);
			uint64_t tx_calls = __sync_fetch_and_add(&ctx->tx_calls, 0);
			uint64_t errors = __sync_fetch_and_add(&ctx->errors, 0);
			uint64_t requests = __sync_fetch_and_add(&ctx->total_requests, 0);
			int flows = __sync_fetch_and_add(&ctx->active_conns, 0);

			uint64_t delta_rx_bytes = rx_bytes - prev_rx_bytes[i];
			uint64_t delta_tx_bytes = tx_bytes - prev_tx_bytes[i];
			uint64_t delta_rx_calls = rx_calls - prev_rx_calls[i];
			uint64_t delta_tx_calls = tx_calls - prev_tx_calls[i];
			uint64_t delta_errors = errors - prev_errors[i];
			uint64_t delta_requests = requests - prev_requests[i];

			double rx_gbps = (double)delta_rx_bytes * 8.0 / (interval * 1e9);
			double tx_gbps = (double)delta_tx_bytes * 8.0 / (interval * 1e9);
			double rx_ops = delta_rx_calls / interval;
			double tx_ops = delta_tx_calls / interval;
			double req_per_sec = delta_requests / interval;

			fprintf(stdout,
				"[CPU%2d] flows: %6d, RX: %7.0f(op/s) (err: %5" PRIu64 "), %5.2lf(Gbps), TX: %7.0f(op/s), %5.2lf(Gbps), HTTP: %7.0f(req/s)\n",
				i, flows, rx_ops, delta_errors, rx_gbps, tx_ops, tx_gbps, req_per_sec);

			prev_rx_bytes[i] = rx_bytes;
			prev_tx_bytes[i] = tx_bytes;
			prev_rx_calls[i] = rx_calls;
			prev_tx_calls[i] = tx_calls;
			prev_errors[i] = errors;
			prev_requests[i] = requests;

			total_flows += flows;
			total_rx_bytes += delta_rx_bytes;
			total_tx_bytes += delta_tx_bytes;
			total_rx_calls += delta_rx_calls;
			total_tx_calls += delta_tx_calls;
			total_errors += delta_errors;
			total_requests += delta_requests;
		}

		double total_rx_gbps = total_rx_bytes * 8.0 / (interval * 1e9);
		double total_tx_gbps = total_tx_bytes * 8.0 / (interval * 1e9);
		double total_rx_ops = total_rx_calls / interval;
		double total_tx_ops = total_tx_calls / interval;
		double total_req_per_sec = total_requests / interval;

		fprintf(stdout,
			"[ ALL ] flows: %6d, RX: %7.0f(op/s) (err: %5" PRIu64 "), %5.2lf(Gbps), TX: %7.0f(op/s), %5.2lf(Gbps), HTTP: %7.0f(req/s)\n",
			total_flows, total_rx_ops, total_errors, total_rx_gbps,
			total_tx_ops, total_tx_gbps, total_req_per_sec);

		fflush(stdout);
		prev_ts = now_ts;
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
void *
RunServerThread(void *arg)
{
	int core = *(int *)arg;
	struct thread_context *ctx;
	int listener;
	int ep;
	struct epoll_event *events;
	int nevents;
	int i, ret;
	int do_accept;
	
	/* initialization */
	ctx = InitializeServerThread(core);
	if (!ctx) {
		TRACE_ERROR("Failed to initialize server thread.\n");
		return NULL;
	}
	ep = ctx->ep;

	events = (struct epoll_event *)
			calloc(MAX_EVENTS, sizeof(struct epoll_event));
	if (!events) {
		TRACE_ERROR("Failed to create event struct!\n");
		exit(-1);
	}

	/* Each thread creates its own listener with SO_REUSEPORT */
	listener = CreateListeningSocket(ctx);
	if (listener < 0) {
		TRACE_ERROR("Failed to create listening socket.\n");
		exit(-1);
	}
	ctx->listener = listener;
	__sync_synchronize();
	thread_ctx_list[core] = ctx;
	__sync_synchronize();

	TRACE_INFO("Thread %d: Server thread started, listening on port 80\n", core);

	while (!done[core]) {
		nevents = epoll_wait(ep, events, MAX_EVENTS, -1);
		if (nevents < 0) {
			if (errno != EINTR)
				perror("epoll_wait");
			break;
		}

		do_accept = FALSE;
		for (i = 0; i < nevents; i++) {

			if (events[i].data.fd == listener) {
				/* if the event is for the listener, accept connection */
				do_accept = TRUE;

			} else if (events[i].events & EPOLLERR) {
				int err;
				socklen_t len = sizeof(err);

				/* error on the connection */
				TRACE_APP("[CPU %d] Error on socket %d\n", 
						core, events[i].data.fd);
				__sync_fetch_and_add(&ctx->errors, 1);
				if (getsockopt(events[i].data.fd, 
						SOL_SOCKET, SO_ERROR, (void *)&err, &len) == 0) {
					if (err != ETIMEDOUT) {
						fprintf(stderr, "Error on socket %d: %s\n", 
								events[i].data.fd, strerror(err));
					}
				} else {
					perror("getsockopt");
				}
				CloseConnection(ctx, events[i].data.fd, 
						&ctx->svars[events[i].data.fd]);

			} else if (events[i].events & EPOLLIN) {
				ret = HandleReadEvent(ctx, events[i].data.fd, 
						&ctx->svars[events[i].data.fd]);

				if (ret == 0) {
					/* connection closed by remote host */
					CloseConnection(ctx, events[i].data.fd, 
							&ctx->svars[events[i].data.fd]);
				} else if (ret < 0) {
					/* if not EAGAIN, it's an error */
					if (errno != EAGAIN && errno != EWOULDBLOCK) {
						CloseConnection(ctx, events[i].data.fd, 
								&ctx->svars[events[i].data.fd]);
					}
				}

			} else if (events[i].events & EPOLLOUT) {
				struct server_vars *sv = &ctx->svars[events[i].data.fd];
				if (sv->rspheader_sent) {
					SendUntilAvailable(ctx, events[i].data.fd, sv);
				} else {
					TRACE_APP("Socket %d: Response header not sent yet.\n", 
							events[i].data.fd);
				}

			} else {
				assert(0);
			}
		}

		/* if do_accept flag is set, accept connections (batch accept for better performance) */
		/* In level-triggered mode, if there are still pending connections after this batch,
		 * epoll will report EPOLLIN again in the next epoll_wait() call */
		if (do_accept) {
			int accept_count = 0;
			/* Accept all available connections, but limit batch size to avoid starving
			 * other events (read/write on established connections) */
			while (accept_count < 100) {  /* Max 100 per epoll_wait() iteration */
				ret = AcceptConnection(ctx, listener);
				if (ret < 0) {
					/* No more connections available (EAGAIN), break and wait for next epoll event */
					break;
				}
				accept_count++;
			}
			/* If we hit the limit (100), there might be more connections pending.
			 * In level-triggered mode, epoll will report EPOLLIN again, so we'll
			 * continue accepting in the next epoll_wait() iteration */
		}

	}

	thread_ctx_list[core] = NULL;
	__sync_synchronize();
	close(listener);
	close(ep);
	free(events);
	free(ctx->svars);
	free(ctx);
	pthread_exit(NULL);

	return NULL;
}

/*----------------------------------------------------------------------------*/
void
SignalHandler(int signum)
{
	int i;

	for (i = 0; i < core_limit; i++) {
		if (app_thread[i] == pthread_self()) {
			done[i] = TRUE;
		} else {
			if (!done[i]) {
				pthread_kill(app_thread[i], signum);
			}
		}
	}
	stop_stats = 1;

}

/*----------------------------------------------------------------------------*/
static void
printHelp(const char *prog_name)
{
	TRACE_CONFIG("%s -p <path_to_www/> "
		     "[-N num_cores] [-c <per-process core_id>] [-b backlog] [-h]\n",
		     prog_name);
	exit(EXIT_SUCCESS);
}

/*----------------------------------------------------------------------------*/
int 
main(int argc, char **argv)
{
	DIR *dir;
	struct dirent *ent;
	int fd;
	int ret;
	uint64_t total_read;
	int cores[MAX_CPUS];
	int process_cpu;
	int i, o;

	num_cores = GetNumCPUs();
	core_limit = num_cores;
	process_cpu = -1;
	dir = NULL;

	if (argc < 2) {
		TRACE_CONFIG("$%s directory_to_service\n", argv[0]);
		return FALSE;
	}

	while (-1 != (o = getopt(argc, argv, "N:p:c:b:h"))) {
		switch (o) {
		case 'p':
			/* open the directory to serve */
			www_main = optarg;
			dir = opendir(www_main);
			if (!dir) {
				TRACE_CONFIG("Failed to open %s.\n", www_main);
				perror("opendir");
				return FALSE;
			}
			break;
		case 'N':
			core_limit = mystrtol(optarg, 10);
			if (core_limit > num_cores) {
				TRACE_CONFIG("CPU limit should be smaller than the "
					     "number of CPUs: %d\n", num_cores);
				return FALSE;
			}
			break;
		case 'c':
			process_cpu = mystrtol(optarg, 10);
			if (process_cpu > core_limit) {
				TRACE_CONFIG("Starting CPU is way off limits!\n");
				return FALSE;
			}
			break;
		case 'b':
			backlog = mystrtol(optarg, 10);
			break;
		case 'h':
			printHelp(argv[0]);
			break;
		}
	}
	
	if (dir == NULL) {
		TRACE_CONFIG("You did not pass a valid www_path!\n");
		exit(EXIT_FAILURE);
	}

	memset(thread_ctx_list, 0, sizeof(thread_ctx_list));
	stop_stats = 0;
	stats_thread_started = 0;
	
	nfiles = 0;
	while ((ent = readdir(dir)) != NULL) {
		struct stat st;
		
		if (strcmp(ent->d_name, ".") == 0)
			continue;
		else if (strcmp(ent->d_name, "..") == 0)
			continue;

		snprintf(fcache[nfiles].name, NAME_LIMIT, "%s", ent->d_name);
		snprintf(fcache[nfiles].fullname, FULLNAME_LIMIT, "%s/%s",
			 www_main, ent->d_name);
		
		/* Check if it's a regular file before opening */
		if (stat(fcache[nfiles].fullname, &st) < 0) {
			perror("stat");
			continue;
		}
		
		/* Skip directories and other non-regular files */
		if (!S_ISREG(st.st_mode)) {
			TRACE_INFO("Skipping non-regular file: %s\n", fcache[nfiles].name);
			continue;
		}
		
		fcache[nfiles].size = st.st_size;
		
		fd = open(fcache[nfiles].fullname, O_RDONLY);
		if (fd < 0) {
			perror("open");
			continue;
		}

		fcache[nfiles].file = (char *)malloc(fcache[nfiles].size);
		if (!fcache[nfiles].file) {
			TRACE_CONFIG("Failed to allocate memory for file %s\n", 
				     fcache[nfiles].name);
			perror("malloc");
			continue;
		}

		TRACE_INFO("Reading %s (%lu bytes)\n", 
				fcache[nfiles].name, fcache[nfiles].size);
		total_read = 0;
		while (1) {
			ret = read(fd, fcache[nfiles].file + total_read, 
					fcache[nfiles].size - total_read);
			if (ret < 0) {
				break;
			} else if (ret == 0) {
				break;
			}
			total_read += ret;
		}
		if (total_read < fcache[nfiles].size) {
			free(fcache[nfiles].file);
			continue;
		}
		close(fd);
		nfiles++;

		if (nfiles >= MAX_FILES)
			break;
	}

	finished = 0;

	/* if backlog is not specified, set it to 4K */
	if (backlog == -1) {
		backlog = 4096;
	}
	
	/* register signal handler */
	signal(SIGINT, SignalHandler);

	TRACE_INFO("Application initialization finished.\n");
	TRACE_INFO("Starting %d server threads with SO_REUSEPORT\n", core_limit);
	if (pthread_create(&stats_thread, NULL, StatsThread, NULL) == 0) {
		stats_thread_started = 1;
	} else {
		perror("pthread_create (stats)");
		TRACE_ERROR("Failed to create statistics thread.\n");
	}

	for (i = ((process_cpu == -1) ? 0 : process_cpu); i < core_limit; i++) {
		cores[i] = i;
		done[i] = FALSE;
		
		if (pthread_create(&app_thread[i], 
				   NULL, RunServerThread, (void *)&cores[i])) {
			perror("pthread_create");
			TRACE_CONFIG("Failed to create server thread.\n");
				exit(EXIT_FAILURE);
		}
		if (process_cpu != -1)
			break;
	}
	
	for (i = ((process_cpu == -1) ? 0 : process_cpu); i < core_limit; i++) {
		pthread_join(app_thread[i], NULL);

		if (process_cpu != -1)
			break;
	}

	if (stats_thread_started) {
		stop_stats = 1;
		pthread_join(stats_thread, NULL);
	}

	closedir(dir);
	return 0;
}
