#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ucx_shared.h"

char *device_name;
char *transport_name;
int all_sizes;
int message_size;
int window_size;
int user_messages;
int num_messages;
int user_warmup_messages;
int num_warmup_messages;

int num_threads;

int way_ctx_sharing;
int way_worker_sharing;

struct test_size_param bw_test_sizes[]= {
	{ 1 <<  0, 1 }, { (1 <<  1) + (1 <<  0), 0 },
	{ 1 <<  1, 1 }, { (1 <<  1) + (1 <<  0), 0 },
	{ 1 <<  2, 1 }, { (1 <<  2) + (1 <<  1), 0 },
	{ 1 <<  3, 1 }, { (1 <<  3) + (1 <<  2), 0 },
	{ 1 <<  4, 1 }, { (1 <<  4) + (1 <<  3), 0 },
	{ 1 <<  5, 1 }, { (1 <<  5) + (1 <<  4), 0 },
	{ 1 <<  6, 1 }, { (1 <<  6) + (1 <<  5), 0 },
	{ 1 <<  7, 1 }, { (1 <<  7) + (1 <<  6), 0 },
	{ 1 <<  8, 1 }, { (1 <<  8) + (1 <<  7), 0 },
	{ 1 <<  9, 1 }, { (1 <<  9) + (1 <<  8), 0 },
	{ 1 << 10, 1 }, { (1 << 10) + (1 <<  9), 0 },
	{ 1 << 11, 1 }, { (1 << 11) + (1 << 10), 0 },
	{ 1 << 12, 1 }, { (1 << 12) + (1 << 11), 0 },
	{ 1 << 13, 1 }, { (1 << 13) + (1 << 12), 0 },
	{ 1 << 14, 1 }, { (1 << 14) + (1 << 13), 0 },
	{ 1 << 15, 1 }, { (1 << 15) + (1 << 14), 0 },
	{ 1 << 16, 1 }, { (1 << 16) + (1 << 15), 0 },
	{ 1 << 17, 0 }, { (1 << 17) + (1 << 16), 0 },
	{ 1 << 18, 0 }, { (1 << 18) + (1 << 17), 0 },
	{ 1 << 19, 0 }, { (1 << 19) + (1 << 18), 0 },
	{ 1 << 20, 0 }, { (1 << 20) + (1 << 19), 0 },
	{ 1 << 21, 0 }, { (1 << 21) + (1 << 20), 0 },
	{ 1 << 22, 0 }, { (1 << 22) + (1 << 21), 0 },
	{ 1 << 23, 0 },
};

const unsigned int bw_test_cnt = (sizeof bw_test_sizes / sizeof bw_test_sizes[0]);

void print_usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  mpiexec -n 2 -ppn 1 -bind-to core -hosts <sender>,<receiver> %s <options>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -d, --device=<dev>     use UCX device <dev>\n");
	printf("  -t, --transport=<transport>     use UCX transport <transport>\n");
	printf("  -s, --size=<size>      size of message to exchange (default: 8B)\n");
	printf("  -w, --window=<wsize>      number of messages before checking for progress (default: 64)\n");
	printf("  -m, --messages=<msgs>   number of messages to transfer (default 1024)\n");
	printf("  -u, --warmup-messages=<wmsgs>   number of messages to transfer for warmup (default 512)\n");
	printf("  -T, --threads=<#threads>     number of threads\n");
	printf("  -C, --way-ctx-sharing=<ctx-sharing>     degree of sharing the UCP context between threads\n");
	printf("  -W, --way-worker-sharing=<worker-sharing>     degree of sharing the UCP worker between threads\n");
}

void parse_args(int op, char *optarg)
{
	switch (op) {
		case 'd':
			device_name = strdup(optarg);
			break;
		case 't':
			transport_name = strdup(optarg);
			break;
		case 's':
			all_sizes = 0;
			message_size = atoi(optarg);
			break;
		case 'w':
			window_size = atoi(optarg);
			break;
		case 'm':
			user_messages = 1;
			num_messages = atoi(optarg);
			break;
		case 'u':
			user_warmup_messages = 1;
			num_warmup_messages = atoi(optarg);
			break;
		case 'T':
			num_threads = atoi(optarg);
			break;
		case 'C':
			way_ctx_sharing = atoi(optarg);
			break;
		case 'W':
			way_worker_sharing = atoi(optarg);
			break;
	}
}

/*void show_perf(int tsize, int messages_per_thread, double *elapsed)
{
	static int header = 1;

	double size_MB_per_thread = (double) messages_per_thread * tsize  / 1e6;
	double Mmsgs_per_thread = messages_per_thread / 1e6;

	int thread_i;
	double tot_mr = 0;
	double tot_bw = 0;
	double max_elapsed = elapsed[0];
	for (thread_i = 0; thread_i < num_threads; thread_i++) {
		double mr = Mmsgs_per_thread / elapsed[thread_i];
		double bw = size_MB_per_thread / elapsed[thread_i];
		tot_mr += mr;
		tot_bw += bw;
		if (elapsed[thread_i] > max_elapsed)
			max_elapsed = elapsed[thread_i];
	}
	
	if (header) {
		printf("%-10s\t%-10s\t%-10s\t%-10s\t%10s\t%10s\t%10s\n",
			"Size", "#Threads", "#QPs", "Msgs/QP",
			"MB/sec", "Mmsgs/s", "Time (s)");
		header = 0;
	}

	printf("%-10d\t", tsize);
	printf("%-10d\t", num_threads);
	printf("%-10d\t", num_qps);
	printf("%-10d\t", messages_per_thread);
	printf("%10.2f\t%10.2f\t%10.2f\n", tot_bw, tot_mr, max_elapsed);
}*/

/* Recursive function to return gcd of a and b */
int gcd(int a, int b)
{
	// base case
	if (a == b)
		return a;
	
	// a is greater
	if (a > b)
		return gcd(a-b, b);
	return gcd(a, b-a);
}

/* LCM of two numbers */
int lcm(int a, int b)
{
	return (a*b)/gcd(a,b);
}
