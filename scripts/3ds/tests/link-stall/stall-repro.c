/* Host-side reproduction of the 3dslink mirror's stall path (NEXT-SPRINT.md backlog
 * item 15, part C of the A6 runbook). Not the port's code: the send loop below is a copy
 * of tee_write()'s (ports/3ds/source/a1_3ds_platform.cpp: MSG_DONTWAIT, poll(POLLOUT)
 * in 100 ms steps, give up after kLinkStallMs = 500), run over a loopback TCP
 * connection whose reader never reads (a `kill -STOP`ped 3dslink). It shows how many
 * bytes the sender gets rid of before the stall path runs: the sender's send buffer
 * plus the reader's receive buffer, whatever the line rate. The 3DS's soc buffer sizes
 * are not known here (UNVERIFIED), so this measures the mechanism, not the console.
 *
 * Usage: stall-repro SNDBUF RCVBUF LINE_BYTES   (0 = the system default buffer)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const int kLinkStallMs = 500;

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* tee_write()'s socket half. Returns 0 while the mirror is up, 1 once it is dropped. */
static int mirror_write(int fd, const char* ptr, size_t len, int* stalled)
{
	size_t sent = 0;
	int waited_ms = 0;
	while (sent < len)
	{
		ssize_t n = send(fd, ptr + sent, len - sent, MSG_DONTWAIT);
		if (n > 0)
		{
			sent += n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waited_ms < kLinkStallMs)
		{
			const int step = 100 < kLinkStallMs - waited_ms ? 100 : kLinkStallMs - waited_ms;
			struct pollfd p = {fd, POLLOUT, 0};
			poll(&p, 1, step);
			waited_ms += step;
			continue;
		}
		*stalled = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
		return 1;
	}
	return 0;
}

static int getbuf(int fd, int opt)
{
	int v = 0;
	socklen_t l = sizeof v;
	getsockopt(fd, SOL_SOCKET, opt, &v, &l);
	return v;
}

int main(int argc, char** argv)
{
	if (argc != 4)
	{
		fprintf(stderr, "usage: %s SNDBUF RCVBUF LINE_BYTES (0 = default)\n", argv[0]);
		return 2;
	}
	const int sndbuf = atoi(argv[1]), rcvbuf = atoi(argv[2]), line_bytes = atoi(argv[3]);
	if (line_bytes <= 0 || line_bytes > 65536)
		return 2;

	int ls = socket(AF_INET, SOCK_STREAM, 0);
	if (rcvbuf > 0)
		setsockopt(ls, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);   /* inherited by accept() */
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(ls, (struct sockaddr*)&a, sizeof a) || listen(ls, 1))
		return perror("bind/listen"), 1;
	socklen_t al = sizeof a;
	getsockname(ls, (struct sockaddr*)&a, &al);

	int cs = socket(AF_INET, SOCK_STREAM, 0);
	if (sndbuf > 0)
		setsockopt(cs, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
	if (connect(cs, (struct sockaddr*)&a, sizeof a))
		return perror("connect"), 1;
	int rs = accept(ls, NULL, NULL);   /* the stopped host: never read */
	if (rs < 0)
		return perror("accept"), 1;
	/* macOS's send() blocked here despite MSG_DONTWAIT (seen with `sample`), so the host
	   copy also makes the socket non-blocking: the semantics tee_write() relies on. */
	fcntl(cs, F_SETFL, fcntl(cs, F_GETFL) | O_NONBLOCK);

	char* line = malloc(line_bytes);
	memset(line, 'x', line_bytes);
	line[line_bytes - 1] = '\n';

	long total = 0;
	int stalled = 0;
	const double t0 = now_ms();
	double full_at = 0;
	for (;;)
	{
		const double t = now_ms();
		if (mirror_write(cs, line, line_bytes, &stalled))
		{
			full_at = t;
			break;
		}
		total += line_bytes;
		if (t - t0 > 20000)
		{
			/* The buffers keep growing (loopback autotuning): each write found room within
			   its 500 ms budget for 20 s, so the stall path never ran. */
			printf("no stall in 20 s: %ld bytes accepted (sndbuf %d, rcvbuf %d)\n", total, getbuf(cs, SO_SNDBUF),
			       getbuf(rs, SO_RCVBUF));
			return 1;
		}
	}
	const double give_up_ms = now_ms() - full_at;
	printf("sndbuf %d (asked %d), rcvbuf %d (asked %d), line %d B\n", getbuf(cs, SO_SNDBUF), sndbuf,
	       getbuf(rs, SO_RCVBUF), rcvbuf, line_bytes);
	printf("accepted before the stall: %ld bytes; then %s after %.0f ms of polling (%.0f ms total)\n", total,
	       stalled ? "dropped as \"stalled\"" : "dropped as \"host went away\"", give_up_ms, now_ms() - t0);
	printf("time to reach the stall at this backlog: at 170 B/s %.0f s; at 10 KB/s %.1f s; at 30 KB/s %.1f s\n",
	       total / 170.0, total / 10240.0, total / 30720.0);
	return stalled ? 0 : 1;
}
