#include "shared.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

char *dev_name;
int all_sizes;
int user_messages = 0;
int transfer_size;
int messages;
int warmup_messages;
int qp_depth;
int num_threads;
int num_qps;
int num_comps;
int mod_comp;
int postlist;
int no_inlining;
int use_td;
int use_cq_ex;
int way_ctx_sharing;
int way_pd_sharing;
int way_mr_sharing;
int way_buf_sharing;
int way_cq_sharing;
int way_qp_sharing;

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

void print_resSharing_usage(const char *argv0)
{
	printf("  -X, --way-ctx-sharing=<x-way-ctx-sharing>    degree of CTX sharing\n");
	printf("  -P, --way-pd-sharing=<x-way-pd-sharing>    degree of PD sharing\n");
	printf("  -M, --way-mr-sharing=<x-way-mr-sharing>    degree of MR sharing\n");
	printf("  -b, --way-buf-sharing=<x-way-buf-sharing>    degree of buf sharing\n");
	printf("  -C, --way-cq-sharing=<x-way-cq-sharing>    degree of CQ sharing\n");	
	printf("  -E, --way-qp-sharing=<x-way-qp-sharing>    degree of QP sharing\n");	
}

void print_usage(const char *argv0, int res_sharing_bench)
{
	printf("Usage:\n");
	printf("  OMP_PLACES=cores OMP_PROC_BIND=close mpiexec -n 2 -ppn 1 -bind-to core:<num-threads> -hosts <sender>,<receiver> %s <options>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
	printf("  -S, --size=<size>      size of message to exchange (default 4096)\n");
	printf("  -D, --qp-depth=<depth>  depth of the qp (default 128 on sender; 512 on receiver)\n");
	printf("  -M, --messages=<msgs>   number of messages to transfer (default 2e23)\n");
	printf("  -q, --num-qps=<qps>    number of queue pairs (default 1)\n");
	printf("  -p, --postlist=<postlist-length>    number of WQEs to send in one postlist (default 32)\n");
	printf("  -Q, --mod-comp=<mod-comp>    number of WQEs for whic a CQE should be generated (default 64)\n");
	printf("  -n, --no-inlining=<no-inlining>    whether or not to send payload inlined in the WQE (default 0 i.e. messages will be sent inline)\n");
	printf("  -o, --use-td=<use-td>    whether or not to use Thread Domains (effective only without QP-sharing)\n");
	printf("  -x, --use-cq-ex=<use-cq-ex>    whether or not to use Extended CQs\n");

	if (res_sharing_bench)
		print_resSharing_usage(argv0);
}

void parse_args(int op, char *optarg)
{
	switch (op) {
		case 'S':
			all_sizes = 0;
			transfer_size = atoi(optarg);
			break;
		case 'd':
			dev_name = strdup(optarg);
			break;
		case 'm':
			user_messages = 1;
			messages = atoi(optarg);
			break;
		case 'D':
			qp_depth = atoi(optarg);
			break;
		case 't':
			num_threads = atoi(optarg);
			break;
		case 'p':
			postlist = atoi(optarg);
			break;
		case 'Q':
			mod_comp = atoi(optarg);
			break;
		case 'n':
			no_inlining = 1;
			break;
		case 'o':
			use_td = 1;
			break;
		case 'x':
			use_cq_ex = 1;
			break;
		case 'X':
			way_ctx_sharing = atoi(optarg);
			break;
		case 'P':
			way_pd_sharing = atoi(optarg);
			break;
		case 'M':
			way_mr_sharing = atoi(optarg);
			break;
		case 'b':
			way_buf_sharing = atoi(optarg);
			break;
		case 'C':
			way_cq_sharing = atoi(optarg);
			break;
		case 'E':
			way_qp_sharing = atoi(optarg);
			break;
	}
}

void show_perf(int tsize, int messages_per_thread, double *elapsed)
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
}

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
