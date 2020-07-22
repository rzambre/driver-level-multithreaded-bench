#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <netdb.h>
#include <assert.h>
#include <time.h>

#include <mpi.h>
#include <omp.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_tagged.h>

#include "shared.h"

#define ITERS_SMALL		(1000)
#define ITERS_LARGE		(500)  
#define WARMUP_ITERS_SMALL 	(10)
#define WARMUP_ITERS_LARGE	(2)
#define LARGE_THRESHOLD		8192

struct fi_info *fi, *fi_dup, *hints;
struct fid_fabric *fabric;
struct fid_domain *domain;
struct fid_cntr **txcntr_array, **rxcntr_array;
struct fid_av **av_array;
struct fi_context tx_ctx, rx_ctx;
struct fi_context *tx_ctx_arr = NULL, *rx_ctx_arr = NULL;
struct fi_av_attr av_attr = {
	.type = FI_AV_MAP,
//	.rx_ctx_bits /* address bits to identify rx ctx */
//	.ep_per_node /* # endpoints per fabric address (for provider to optimize resource allocation) */
//	.*name
//	.*map_addr /* base mmap address */
//	flags /* operation flags */
	.count = 1
};
struct fi_cntr_attr cntr_attr = {
	.wait_obj = FI_WAIT_NONE,
	.flags = 0
//	.events /* type of events to count */
//	.*wait_set /* optional wait set */
};
fi_addr_t *addr_array;

struct timespec start, end;
struct timespec tot_start, tot_end;

uint64_t *tx_seq_arr, *rx_seq_arr;
char *tx_buf, *rx_buf;
char txctrl_buf, rxctrl_buf;
size_t buf_size;
char default_port[8] = "9228";

int ep_cnt = 4;
struct fid_ep **ep_array;
char *local_addr, *remote_addr;
size_t addrlen = 0;

struct ft_opts opts;
int timeout = -1;

int rank, size;

struct test_size_param bw_test_sizes[] = {
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
	{ 1 << 13, 0 }, { (1 << 13) + (1 << 12), 0 },
	{ 1 << 14, 0 }, { (1 << 14) + (1 << 13), 0 },
	{ 1 << 15, 0 }, { (1 << 15) + (1 << 14), 0 },
	{ 1 << 16, 0 }, { (1 << 16) + (1 << 15), 0 },
	{ 1 << 17, 0 }, { (1 << 17) + (1 << 16), 0 },
	{ 1 << 18, 0 }, { (1 << 18) + (1 << 17), 0 },
	{ 1 << 19, 0 }, { (1 << 19) + (1 << 18), 0 },
	{ 1 << 20, 0 }, { (1 << 20) + (1 << 19), 0 },
	{ 1 << 21, 0 }, { (1 << 21) + (1 << 20), 0 },
	{ 1 << 22, 0 }, { (1 << 22) + (1 << 21), 0 },
	{ 1 << 23, 0 },
};

const unsigned int bw_test_cnt = (sizeof bw_test_sizes / sizeof bw_test_sizes[0]);

static inline int use_size(int index, int enable_flags)
{
	return (enable_flags & bw_test_sizes[index].enable_flags);
}

static void usage(char *name, char *desc)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s [OPTIONS]\t\tstart server\n", name);
	fprintf(stderr, "  %s [OPTIONS] <host>\tconnect to server\n", name);

	if (desc)
		fprintf(stderr, "\n%s\n", desc);

	fprintf(stderr, "\nOptions:\n");
	FT_PRINT_OPTS_USAGE("-d <domain>", "domain name");
	FT_PRINT_OPTS_USAGE("-p <provider>", "specific provider name eg sockets, verbs");
	//FT_PRINT_OPTS_USAGE("-e <ep_type>", "Endpoint type: msg|rdm|dgram (default:rdm)");
	FT_PRINT_OPTS_USAGE("-h", "display this help output");

	return;
}

static void csusage(char *name, char *desc)
{
	usage(name, desc);
	FT_PRINT_OPTS_USAGE("-I <number>", "number of iterations");
	FT_PRINT_OPTS_USAGE("-S <size>", "specific transfer size");
	//FT_PRINT_OPTS_USAGE("-t <type>", "completion type [queue, counter]");
	//FT_PRINT_OPTS_USAGE("-c <method>", "completion method [spin, sread, fd]");
	FT_PRINT_OPTS_USAGE("-h", "display this help output");

	return;
}

void benchmark_usage(void)
{
	//FT_PRINT_OPTS_USAGE("-v", "enables data_integrity checks");
	FT_PRINT_OPTS_USAGE("-W", "window size (for bandwidth tests)\n\n");
}

static void parseinfo(int op, char *optarg, struct fi_info *hints)
{
	switch (op) {
	case 'd':
		if (!hints->domain_attr) {
			hints->domain_attr = malloc(sizeof *(hints->domain_attr));
			if (!hints->domain_attr) {
				perror("malloc");
				exit(EXIT_FAILURE);
			}
		}
		hints->domain_attr->name = strdup(optarg);
		break;
	case 'p':
		if (!hints->fabric_attr) {
			hints->fabric_attr = malloc(sizeof *(hints->fabric_attr));
			if (!hints->fabric_attr) {
				perror("malloc");
				exit(EXIT_FAILURE);
			}
		}
		hints->fabric_attr->prov_name = strdup(optarg);
		break;
	// case 'e':
	// 	if (!strncasecmp("msg", optarg, 3))
	// 		hints->ep_attr->type = FI_EP_MSG;
	// 	if (!strncasecmp("rdm", optarg, 3))
	// 		hints->ep_attr->type = FI_EP_RDM;
	// 	if (!strncasecmp("dgram", optarg, 5))
	// 		hints->ep_attr->type = FI_EP_DGRAM;
	// 	break;
	default:
		/* let getopt handle unknown opts*/
		break;

	}
}

static void parse_benchmark_opts(int op, char *optarg)
{
	switch (op) {
	// case 'v':
	// 	opts.options |= FT_OPT_VERIFY_DATA;
	// 	break;
	case 'W':
		opts.window_size = atoi(optarg);
		break;
	default:
		break;
	}
}

void parsecsopts(int op, char *optarg, struct ft_opts *opts)
{
	switch (op) {
	case 'I':
		opts->options |= FT_OPT_ITER;
		opts->iterations = atoi(optarg);
		break;
	case 'S':
		opts->options |= FT_OPT_SIZE;
		opts->transfer_size = atoi(optarg);
		break;
	// case 'c':
	// 	if (!strncasecmp("sread", optarg, 5))
	// 		opts->comp_method = FT_COMP_SREAD;
	// 	else if (!strncasecmp("fd", optarg, 2))
	// 		opts->comp_method = FT_COMP_WAIT_FD;
	// 	break;
	// case 't':
	// 	if (!strncasecmp("counter", optarg, 7)) {
	// 		opts->options |= FT_OPT_RX_CNTR | FT_OPT_TX_CNTR;
	// 		opts->options &= ~(FT_OPT_RX_CQ | FT_OPT_TX_CQ);
	// 	}
	// 	break;
	// case 'l':
	// 	opts->options |= FT_OPT_ALIGN;
	// 	break;
	default:
		/* let getopt handle unknown opts*/
		break;
	}
}

static int wait_for_comp(struct fid_cntr *cntr, uint64_t total)
{
	int ret;

	ret = fi_cntr_wait(cntr, total, timeout);
	if (ret)
		FT_PRINTERR("fi_cntr_wait", ret);

	return 0;
}

static int init_av(void)
{
	int ret, i;

	/* Get local addresses */

	addrlen = 0;

	ret = fi_getname(&ep_array[0]->fid, local_addr, &addrlen);
	if (ret != -FI_ETOOSMALL) {
		FT_PRINTERR("fi_getname", ret);
		return ret;
	}

	local_addr = malloc(addrlen * ep_cnt); // this is an array of addresses
	remote_addr = malloc(addrlen * ep_cnt); // this is an array of addresses

	/* Get local addresses of all eps */
	for (i = 0; i < ep_cnt; i++) {
		ret = fi_getname(&ep_array[i]->fid, local_addr + addrlen * i, &addrlen);
		if (ret) {
			FT_PRINTERR("fi_getname", ret);
			return ret;
		}
	}

	/* Exchange the addresses */

	if (rank) {
		/* CLIENT */
		MPI_Send(local_addr, addrlen * ep_cnt, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
		MPI_Recv(remote_addr, addrlen * ep_cnt, MPI_CHAR, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
	} else {
		/* SERVER */
		MPI_Recv(remote_addr, addrlen * ep_cnt, MPI_CHAR, 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		MPI_Send(local_addr, addrlen * ep_cnt, MPI_CHAR, 1, 0, MPI_COMM_WORLD);
	}

	/* Insert the remote addresses into the AVs */

	for (i = 0; i < ep_cnt; i++) {
		ret = fi_av_insert(av_array[i], remote_addr + addrlen * i, 1, &addr_array[i], 0, NULL);
		if (ret < 0) {
			FT_PRINTERR("fi_av_insert", ret);
			return ret;
		} else if (ret != 1) {
			fprintf(stderr, "fi_av_insert: number of addresses inserted = %d;"
			       " number of addresses given = %zd\n", ret, (size_t) 1);
			return -EXIT_FAILURE;
		}
	}

	free(local_addr);
	free(remote_addr);
	return 0;
}

static void show_perf(int tsize, int window_size, int iterations, double elapsed)
{

	static int header = 1;

	int64_t total_iterations = window_size * iterations;
	int64_t total_messages = total_iterations * ep_cnt; // each iteration is sending ep_cnt messages
	int64_t total_bytes = total_messages * tsize;

	double bw = total_bytes / 1e6 / elapsed; // MB/s
	double mr = total_messages / elapsed; // Messages/s

	if (header) {
		printf("%-10s\t%-10s\t%-10s\t%-10s\t%10s\t%10s\t%10s\n",
			"size", "iters", "bytes",
			"messages", "time", "MB/sec", "Messages/s");
		header = 0;
	}

	printf("%-10d\t", tsize);
	printf("%-10d\t", total_iterations);
	printf("%-10d\t", total_bytes);
	printf("%-10d\t", total_messages);

	printf("%8.2fs\t%10.2f\t%10.2f\n",
		elapsed, bw, mr);

}

static int multi_ep_sync(void)
{

	if (rank) {
		/* CLIENT */
		#pragma omp parallel
		{
			int ret, i;

			/* Get thread id */
			i = omp_get_thread_num();

			ret = fi_send(ep_array[i], tx_buf + buf_size * i, 1, NULL, addr_array[i], &tx_ctx_arr[i * opts.window_size]);
			if (ret)
				FT_PRINTERR("fi_send; sync", ret);
			tx_seq_arr[i] = tx_seq_arr[i] + 1;

			ret = wait_for_comp(txcntr_array[i], tx_seq_arr[i]);
			if (ret)
				FT_PRINTERR("wait_for_comp; sync", ret);

			ret = fi_recv(ep_array[i], rx_buf + buf_size * i, buf_size, NULL, addr_array[i], &rx_ctx_arr[i * opts.window_size]);
			if (ret) 
				FT_PRINTERR("fi_recv; sync", ret);
			rx_seq_arr[i] = rx_seq_arr[i] + 1;

			ret = wait_for_comp(rxcntr_array[i], rx_seq_arr[i]);
			if (ret)
				FT_PRINTERR("wait_for_comp; sync", ret);
		}
	} else {
		/* SERVER */
		#pragma omp parallel
		{
			int ret, i;

			/* Get thread id */
			i = omp_get_thread_num();

			ret = fi_recv(ep_array[i], rx_buf + buf_size * i, buf_size, NULL, addr_array[i], &rx_ctx_arr[i * opts.window_size]);
			if (ret)
				FT_PRINTERR("fi_recv; sync", ret);
			rx_seq_arr[i] = rx_seq_arr[i] + 1;

			ret = wait_for_comp(rxcntr_array[i], rx_seq_arr[i]);
			if (ret)
				FT_PRINTERR("wait_for_comp; sync", ret);

			ret = fi_send(ep_array[i], tx_buf + buf_size * i, 1, NULL, addr_array[i], &tx_ctx_arr[i * opts.window_size]);
			if (ret)
				FT_PRINTERR("fi_send; sync", ret);
			tx_seq_arr[i] = tx_seq_arr[i] + 1;

			ret = wait_for_comp(txcntr_array[i], tx_seq_arr[i]);
			if (ret)
				FT_PRINTERR("wait_for_comp; sync", ret);

		}
	}

	return 0;
}

int multi_ep_bw(void)
{

	int ret, i;
	double t_start = 0, t_end = 0, t_elapsed = 0;

	MPI_Barrier(MPI_COMM_WORLD);
	
	ret = multi_ep_sync();
	if (ret)
		return ret;
	//printf("Done syncing, rank %d\n", rank);

	/* The loop structured allows for the possibility that the sender
	 * immediately overruns the receiving side on the first transfer (or
	 * the entire window). This could result in exercising parts of the
	 * provider's implementation of FI_RM_ENABLED. For better or worse,
	 * some MPI-level benchmarks tend to use this type of loop for measuring
	 * bandwidth.  */

	MPI_Barrier(MPI_COMM_WORLD);

	if (rank) {
		
		/* CLIENT */

			for (i = 0; i < opts.warmup_iterations + opts.iterations; i++) {

				if (i == opts.warmup_iterations) {
					MPI_Barrier(MPI_COMM_WORLD);
					t_start = MPI_Wtime();
					//printf("Start time was: %f\n", t_start);
				}

				#pragma omp parallel
				{
					int j, k, ret;

					k = omp_get_thread_num();

					for (j = 0; j < opts.window_size; j++) {
						if (opts.transfer_size < fi->tx_attr->inject_size) {
							ret = fi_inject(ep_array[k], tx_buf + buf_size * k, opts.transfer_size, addr_array[k]);
							if (ret)
								FT_PRINTERR("fi_inject", ret);
						} else {
							ret = fi_send(ep_array[k], tx_buf + buf_size * k, opts.transfer_size, NULL, addr_array[k], &tx_ctx_arr[k * opts.window_size + j]);
							if (ret)
								FT_PRINTERR("fi_send", ret);
						}
						tx_seq_arr[k] = tx_seq_arr[k] + 1;
					}	

					ret = fi_cntr_wait(txcntr_array[k], tx_seq_arr[k], -1);
					if (ret)
						FT_PRINTERR("fi_cntr_wait", ret);
				}

				MPI_Recv(&rxctrl_buf, 1, MPI_CHAR, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			}

	} else {

		/* SERVER */

		for (i = 0; i < opts.iterations + opts.warmup_iterations; i++) {

			if (i == opts.warmup_iterations)
				MPI_Barrier(MPI_COMM_WORLD);

			#pragma omp parallel
			{
				int j, k, ret;

				k = omp_get_thread_num();
			
				for (j = 0; j < opts.window_size; j++) {
					ret = fi_recv(ep_array[k], rx_buf + buf_size * k, buf_size, NULL, addr_array[k], &rx_ctx_arr[k * opts.window_size + j]);
					if (ret) 
						FT_PRINTERR("fi_recv", ret);
					rx_seq_arr[k] = rx_seq_arr[k] + 1;
				}
				
				ret = fi_cntr_wait(rxcntr_array[k], rx_seq_arr[k], -1);
				if (ret)
					FT_PRINTERR("fi_cntr_wait", ret);

			}

			MPI_Send(&txctrl_buf, 1, MPI_CHAR, 1, 0, MPI_COMM_WORLD);

		}

	}

	MPI_Barrier(MPI_COMM_WORLD);

	if (rank) {
		t_end = MPI_Wtime();
		//printf("End time was: %f\n", t_end);
		t_elapsed = t_end - t_start;
		show_perf(opts.transfer_size, opts.window_size, opts.iterations, t_elapsed);
	}

	return 0;

}

static int init_fabric(void)
{
	int ret = 0;
	int i;

	/* fi_getinfo */

	if (rank) {
		/* CLIENT */
		ret = fi_getinfo(FI_VERSION(1,4), NULL, default_port, 0, hints, &fi);
	
	} else {
		/* SERVER */
		ret = fi_getinfo(FI_VERSION(1,4), NULL, default_port, FI_SOURCE, hints, &fi);

	}
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		return ret;
	}

	/* Get a fi_info corresponding to a wild card port. The first endpoint
	* should use default/given port since that is what is known to both
	* client and server. For other endpoints we should use addresses with
	* random ports to avoid collision. fi_getinfo should return a random
	* port if we don't specify it in the service arg or the hints. This
	* is used only for non-MSG endpoints. */

	struct fi_info *hints_dup;
	hints_dup = fi_dupinfo(hints);
	if (!hints_dup)
		return -FI_ENOMEM;

	free(hints_dup->src_addr);
	free(hints_dup->dest_addr);
	hints_dup->src_addr = NULL;
	hints_dup->dest_addr = NULL;
	hints_dup->src_addrlen = 0;
	hints_dup->dest_addrlen = 0;

	ret = fi_getinfo(FI_VERSION(1,4), NULL, 0, 0, hints_dup, &fi_dup);
	if (ret)
		FT_PRINTERR("fi_getinfo", ret);

	/* fi_fabric */

	ret = fi_fabric(fi->fabric_attr, &fabric, NULL);
	if (ret) {
		FT_PRINTERR("fi_fabric", ret);
		return ret;
	}

	/* fi_domain */
	ret = fi_domain(fabric, fi, &domain, NULL);
	if (ret) {
		FT_PRINTERR("fi_domain", ret);
		return ret;
	}
	
	/* Allocate buffer sizes */

	buf_size = opts.options & FT_OPT_SIZE ?
		opts.transfer_size : bw_test_sizes[bw_test_cnt - 1].size;
	if (buf_size > fi->ep_attr->max_msg_size)
		buf_size = fi->ep_attr->max_msg_size;

	tx_buf = malloc(ep_cnt * buf_size);
	rx_buf = malloc(ep_cnt * buf_size);
	if (!tx_buf || !rx_buf) {
		perror("malloc");
		return -FI_ENOMEM;
	}

	txctrl_buf = 't';
	rxctrl_buf = 'r';

	memset(tx_buf, 0, ep_cnt * buf_size);
	memset(rx_buf, 0, ep_cnt * buf_size);

	/* Allocate TXCNTRs, RXCNTRs, AVs and EPs */

	txcntr_array = calloc(ep_cnt, sizeof *txcntr_array);
	rxcntr_array = calloc(ep_cnt, sizeof *rxcntr_array);
	
	tx_seq_arr = (uint64_t*) calloc(ep_cnt, sizeof(uint64_t));
	rx_seq_arr = (uint64_t*) calloc(ep_cnt, sizeof(uint64_t));
	
	addr_array = calloc(ep_cnt, sizeof *addr_array);
	av_array = calloc(ep_cnt, sizeof *av_array);

	ep_array = calloc(ep_cnt, sizeof *ep_array);

	if (!ep_array || !av_array || !txcntr_array || !rxcntr_array || !tx_seq_arr || !rx_seq_arr)
		return -FI_ENOMEM;

	/* Open up the TXCNTRs, RXCNTRs, AVs and Endpoints */

	for (i = 0; i < ep_cnt; i++) {
		ret = fi_cntr_open(domain, &cntr_attr, &txcntr_array[i], NULL);
		if (ret){
			FT_PRINTERR("fi_cntr_open", ret);
			return ret;
		}
	}

	for (i = 0; i < ep_cnt; i++) {
		ret = fi_cntr_open(domain, &cntr_attr, &rxcntr_array[i], NULL);
		if (ret){
			 FT_PRINTERR("fi_cntr_open", ret);
			 return ret;
		}
	}

	av_attr.ep_per_node = ep_cnt;
	for (i = 0; i < ep_cnt; i++) {
		ret = fi_av_open(domain, &av_attr, &av_array[i], NULL);
		if (ret) {
			FT_PRINTERR("fi_av_open", ret);
			return ret;
		}
	}

	ret = fi_endpoint(domain, fi, &ep_array[0], NULL);
	if (ret) {
		FT_PRINTERR("fi_endpoint", ret);
		return ret;
	}

	for (i = 1; i < ep_cnt; i++) {
		ret = fi_endpoint(domain, fi_dup, &ep_array[i], NULL);
		if (ret) {
			FT_PRINTERR("fi_endpoint", ret);
			return ret;
		}
	}

	/* Bind the EPs to their resources */

	for (i = 0; i < ep_cnt; i++) {

		ret = fi_ep_bind(ep_array[i], &txcntr_array[i]->fid, FI_SEND);
		if (ret) {
			FT_PRINTERR("fi_ep_bind", ret);
			return ret;
		}

		ret = fi_ep_bind(ep_array[i], &rxcntr_array[i]->fid, FI_RECV);
		if (ret) {
			FT_PRINTERR("fi_ep_bind", ret);
			return ret;
		}

		ret = fi_ep_bind(ep_array[i], &av_array[i]->fid, 0);
		if (ret) {
			FT_PRINTERR("fi_ep_bind", ret);
			return ret;
		}

		ret = fi_enable(ep_array[i]);
		if (ret) {
			FT_PRINTERR("fi_enable", ret);
			return ret;
		}

	}

	return 0;
}

static void compute_iterations(void)
{

	if (opts.transfer_size > LARGE_THRESHOLD) {
		opts.iterations = ITERS_LARGE;
		opts.warmup_iterations = WARMUP_ITERS_LARGE;
	} else {
		opts.iterations = ITERS_SMALL;
		opts.warmup_iterations = WARMUP_ITERS_SMALL;
	}

}

static int run_test(void)
{
	int ret, i, j, k;

	// we practically just need one of tx_ctx_arr or rx_ctx_arr since we can use the same ctx_arr for both.
	if (opts.window_size > 0) {
		if (!(tx_ctx_arr = calloc(ep_cnt * opts.window_size, sizeof(struct fi_context))))
			return -FI_ENOMEM;

		if (!(rx_ctx_arr = calloc(ep_cnt * opts.window_size, sizeof(struct fi_context))))
			return -FI_ENOMEM;
	}
	
	if (!(opts.options & FT_OPT_SIZE)) {
		// to run the default test sizes
		for (i = 0; i < bw_test_cnt; i++) { // bw_test_cnt is 45 i.e. there are 45 entries in the test_size array
			if (!use_size(i, opts.sizes_enabled))
				continue;
			/* Set the message size */
			opts.transfer_size = bw_test_sizes[i].size;

			for (k = 0; k < ep_cnt; k++) {
				for (j = 0; j < opts.transfer_size; j++) {
					tx_buf[buf_size * k + j] = 'a'; 
				}
			}

			if (!(opts.options & FT_OPT_ITER))
				compute_iterations();

			ret = multi_ep_bw();
			if (ret)
				return ret;
		}
	} else {
		// if the user has specified custom sizes and iterations

		if (!(opts.options & FT_OPT_ITER))
			compute_iterations();

		for (k = 0; k < ep_cnt; k++) {
			for (j = 0; j < opts.transfer_size; j++) {
				tx_buf[buf_size * k + j] = 'a'; 
			}
		}

		ret = multi_ep_bw();
		if (ret)
			return ret;
	}

	//printf("FINISHED RUNNING TEST\n");

	return ret;
}

static void close_fabric(void)
{

	FT_CLOSEV_FID(ep_array, ep_cnt);
	FT_CLOSEV_FID(txcntr_array, ep_cnt);
	FT_CLOSEV_FID(rxcntr_array, ep_cnt);
	FT_CLOSEV_FID(av_array, ep_cnt);
	FT_CLOSE_FID(domain);
	FT_CLOSE_FID(fabric);

	free(tx_ctx_arr);
	free(rx_ctx_arr);

	fi_freeinfo(fi);
	fi_freeinfo(fi_dup);
	fi_freeinfo(hints);

	free(addr_array);
	free(ep_array);
	free(txcntr_array);
	free(rxcntr_array);
	free(av_array);
	free(tx_buf);
	free(rx_buf);

}

static int run(void)
{
	int ret;

	ret = init_fabric();
	if (ret)
		return ret;

	ret = init_av();
	if (ret)
		return ret;

	ret = run_test();
	if (ret)
		return ret;

	close_fabric();

	return ret;
}

int main(int argc, char **argv)
{
	int op, ret;
	int option_index = 0;

	struct option long_options[] = {
		{"ep-count", required_argument, 0, FT_EP_CNT},
		{0, 0, 0, 0},
	};

	opts = INIT_OPTS;
	opts.options |= FT_OPT_BW;

	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	while ((op = getopt_long(argc, argv, "h" CS_OPTS INFO_OPTS BENCHMARK_OPTS, long_options, &option_index)) != -1) {
		switch (op) {
		case FT_EP_CNT:
			ep_cnt = atoi(optarg);
			if (ep_cnt <= 0) {
				fprintf(stderr, "ep_count needs to be greater than 0\n");
				return EXIT_FAILURE;
			}
			hints->domain_attr->ep_cnt = ep_cnt;
			break;
		default:
			parse_benchmark_opts(op, optarg);
			//ft_parse_addr_opts(op, optarg, &opts);
			parseinfo(op, optarg, hints);
			parsecsopts(op, optarg, &opts);
			break;
		case '?':
		case 'h':
			csusage(argv[0], "A benchmark with multiple endpoints (one domain).");
			benchmark_usage();
			FT_PRINT_OPTS_USAGE("--ep-count <count> (default: 4)",
					"# of endpoints to be opened");
			return EXIT_FAILURE;
		}
	}

	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG;
	hints->mode = FI_CONTEXT;
	hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
	hints->domain_attr->data_progress = FI_PROGRESS_MANUAL;
	hints->domain_attr->control_progress = FI_PROGRESS_MANUAL;
	hints->domain_attr->threading = FI_THREAD_COMPLETION;

	omp_set_num_threads(ep_cnt);
	
	MPI_Init(&argc, &argv);

	MPI_Comm_size(MPI_COMM_WORLD, &size);
	if (size > 2) {
		fprintf(stderr, "Supporting only two processes at the moment\n");
		return -1;
	}

	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		
	ret = run();

	return ret;

}

