/*
 * ntest: Simple test for lost or reordered packets.
 * Copyright (c) 2006 Stanislav Maslovski <stanislav.maslovski@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#ifdef __CYGWIN__
#include <cygwin/in.h>
#endif

struct head {
	struct timeval stamp;
	unsigned long seq, size, recvd, lost, reord;
	char chk_sum;
} *rb, *sb;

unsigned short port = 30000;
unsigned short listen_port = 0;
unsigned short packet_size = 1000;
unsigned long rate = 10;

unsigned long max_seq, recv_cnt, lost_cnt, reordered_cnt;

extern char *optarg;
extern int optind;

#define diff_time(t1, t2) (1000000*((t1).tv_sec - (t2).tv_sec) + (t1).tv_usec - (t2).tv_usec)

void die(char *msg)
{
	perror(msg);
	exit(1);
}

void exit_usage(void)
{
	fprintf(stderr, "Usage: ntest [ -r rate] [ -s packet_size] [-l listen_port] [ -p port] host\n");
	exit(1);
}

void signal_handler(int sign)
{
	switch(sign) {
	case SIGINT:
		fprintf(stdout, "\n\nLocal statistics:\n"
			"   Sent: %lu, Received: %lu, Lost: %lu, Reordered: %lu, Loss %%: %g%%\n"
			"\nRemote statistics:\n"
			"   Sent: %lu, Received: %lu, Lost: %lu, Reordered: %lu, Loss %%: %g%%\n\n",
			sb->seq, recv_cnt, lost_cnt, reordered_cnt, recv_cnt ? (100.0*lost_cnt)/recv_cnt : 0.0,
			rb->seq, rb->recvd, rb->lost, rb->reord, rb->recvd ? (100.0*rb->lost)/rb->recvd : 0.0);
		_exit(0);
	}
}

inline char sum(struct head *hp)
{
	char sum = 0;
	char *p = (char *)hp;
	int cnt = sizeof(struct head);

	while (cnt--)
		sum += *p++;

	return sum;
}

int main(int argc, char *argv[])
{
	int s, ret, wait;
	struct sockaddr_in sa;
	struct timeval tv, ts;
	long period, delta;
	fd_set r_set, w_set, e_set;
	char str[128];

	/* process_options */
	while ((ret = getopt(argc, argv, "l:p:r:s:")) != -1) {
		switch(ret) {
		case 'l':
			listen_port = atoi(optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'r':
			rate = atoi(optarg);
			break;
		case 's':
			packet_size = atoi(optarg);
			if (packet_size < sizeof(struct head))
				packet_size = sizeof(struct head);
			break;
		case '?':
			exit_usage();
		}
	}

	if(!listen_port)
		listen_port = port;

#ifdef DEBUG
	fprintf(stderr, "Args: port: %d, listen: %d, rate: %lu, size: %d, argc: %d, optind: %d, host: %s\n",
	       port, listen_port, rate, packet_size, argc, optind, argv[optind]);
#endif

	if (argc - optind == 1) {
		struct hostent *p;
		if (p = gethostbyname(argv[optind])) {
			sa.sin_family = p->h_addrtype;
			sa.sin_port   = htons(port);
			memcpy(&sa.sin_addr, p->h_addr, p->h_length);
		} else {
			fputs("Host lookup failed.\n", stderr);
			exit(1);
		}
	} else
		exit_usage();

	/* create socket and bind it */
	s = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s != -1) {
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port   = htons(listen_port);
		sa.sin_addr.s_addr = INADDR_ANY;
		if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == -1)
			die("Can't bind socket");
	} else
		die("Can't create socket");

	/* allocate buffers (zeroed) */
	rb = calloc(packet_size, 1);
	sb = calloc(packet_size, 1);
	if (!rb || !sb)
		die("Buffer allocation failed");

	/* set counters */
	max_seq = recv_cnt = lost_cnt = reordered_cnt = 0;

	/* set signal handler */
	if (signal(SIGINT, signal_handler) == SIG_ERR)
		die("Can't install signal handler");
	
	/* doing real work now */
	fputs("Processing the test. Press ^C to stop...\n\n", stdout);

	FD_ZERO(&r_set);
	FD_ZERO(&w_set);
	FD_ZERO(&e_set);
	FD_SET(s, &r_set);
	FD_SET(s, &w_set);
	FD_SET(s, &e_set);

	gettimeofday(&sb->stamp, NULL);
	tv.tv_usec = period = 1000000/rate;
	tv.tv_sec  = 0;

	wait = 1;

	/* main loop */
	while((ret = select(s + 1, &r_set, &w_set, &e_set, &tv)) != -1) {
#ifdef DEBUG
		gettimeofday(&ts, NULL);
		delta = period - diff_time(ts, sb->stamp);
		fprintf(stderr, "%ld.%06ld: r:%1d, ws:%1d, wo:%1d, e:%1d, delta: %ld\n", ts.tv_sec, ts.tv_usec,
				FD_ISSET(s, &r_set), FD_ISSET(s, &w_set),
				FD_ISSET(1, &w_set), FD_ISSET(s, &e_set), delta);
#endif
		if (ret > 0) {
			/* treat exceptions */
			if (FD_ISSET(s, &e_set))
				die("Socket exception");
			/* write statistics */
			if (FD_ISSET(1, &w_set)) {
				ret = sprintf(str, "Local s: %ld r: %ld l: %ld o: %ld    Remote s: %ld r: %ld l: %ld o: %ld\r",
						sb->seq, recv_cnt, lost_cnt, reordered_cnt,
						rb->seq, rb->recvd, rb->lost, rb->reord);
				write(1, str, ret);
				/* statistics just printed */
				FD_CLR(1, &w_set);
			}
			/* receive packet */
			if (FD_ISSET(s, &r_set)) {
				unsigned sa_len = sizeof(sa);
				ret = recvfrom(s, rb, packet_size, 0, (struct sockaddr *)&sa, &sa_len);
				wait = 0;
				if(rb->seq)
					recv_cnt++;
				if (rb->seq < max_seq)
					reordered_cnt++;
				else
					max_seq = rb->seq;
				lost_cnt = max_seq - recv_cnt;
				/* statistics updated */
				FD_SET(1, &w_set);
			}
			/* send packet */
			if (FD_ISSET(s, &w_set)) {
				if (!wait)
					sb->seq++;
				sb->size = packet_size;
				gettimeofday(&ts, NULL);
				sb->stamp = ts;
				sb->recvd = recv_cnt;
				sb->lost  = lost_cnt;
				sb->reord = reordered_cnt;
				sb->chk_sum = 0;
				sb->chk_sum = sum(sb);
				sendto(s, sb, packet_size, 0, (struct sockaddr *)&sa, sizeof(sa));
				/* packet just sent */
				FD_CLR(s, &w_set);
				/* statistics updated */
				FD_SET(1, &w_set);
			}
		}

		FD_SET(s, &r_set);
		FD_SET(s, &e_set);

		gettimeofday(&ts, NULL);
		delta = period - diff_time(ts, sb->stamp);
		if (delta < 0) {
			delta += period;
			if (delta < 0)
				delta = period/2;
			FD_SET(s, &w_set);
		}
		tv.tv_sec = 0;
		tv.tv_usec = delta;
	}
#ifdef DEBUG
	fprintf(stderr, "s: %d, tv_usec: %ld\n", s, (long)tv.tv_usec);
#endif
	die("Error in select()");
}
