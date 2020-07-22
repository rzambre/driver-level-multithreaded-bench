#define _GNU_SOURCE
#define _SVID_SOURCE
#define _XOPEN_SOURCE

#include "ucx_shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <time.h>

#include <mpi.h>
#include <omp.h>
#include <uct/api/uct.h>
#include <ucs/async/async_fwd.h>

#define ITERS_SMALL					(8388608) // 2e23
#define ITERS_LARGE					(1048576) // 2e20
#define WARMUP_ITERS_SMALL			(1048576) // 2e20
#define WARMUP_ITERS_LARGE			(131072)  // 2e17
#define LARGE_THRESHOLD				4096

/**
 * This benchmark simply puts a message from the
 * sender to the receiver using the UCT API  
 */

/* Type for array elements in 'ucs_cpu_set_t'.  */
typedef unsigned long int ucs_cpu_mask_t;


/* Size definition for CPU sets.  */
#define UCS_CPU_SETSIZE  1024
#define UCS_NCPUBITS     (8 * sizeof(ucs_cpu_mask_t))

#define UCS_CPU_ZERO(_cpusetp) \
do { \
	int _i; \
	for ( _i = 0; _i <  (int)(UCS_CPU_SETSIZE / UCS_NCPUBITS); ++_i) { \
		((_cpusetp)->ucs_bits)[_i] = 0; \
	} \
} while (0)

uct_worker_h			worker;
uct_md_h				md;
uct_iface_h				iface;
uct_allocated_memory_t	buf_mem;
uct_ep_h 				ep;

uct_rkey_bundle_t dest_rkey_bundle;

void *my_buf;

int rank, size;

double *t_elapsed, *my_msg_rate;

static int run_bench(void)
{
	int ret = 0;

	t_elapsed = calloc(num_threads, sizeof(double));
	my_msg_rate = calloc(num_threads, sizeof(double));
	if (!t_elapsed || !my_msg_rate) {
		fprintf(stderr, "Error in callocating t_elapsed and my_msg_rate\n");
		ret = EXIT_FAILURE;
		goto exit;
	}

	#pragma omp parallel private(ret)
	{
	int tid;
	ucs_status_t status;

	double t_start;

	ucs_async_context_t *async;

	uct_md_resource_desc_t *md_resources;
	uct_tl_resource_desc_t *tl_resources;
	uct_md_config_t *md_config;
	uct_iface_config_t *iface_config;
	uct_iface_params_t iface_params;
	uct_iface_attr_t iface_attr;
	uct_md_attr_t md_attr;
	uct_device_addr_t *my_dev_addr, *dest_dev_addr;
	uct_iface_addr_t *my_iface_addr, *dest_iface_addr;
	uct_ep_addr_t *my_ep_addr, *dest_ep_addr;

	void *my_rkey_buffer, *dest_rkey_buffer;

	unsigned int num_md_resources, num_tl_resources;
	int md_i, tl_i;
	int rkey_buffer_size;
	int dev_addr_len, iface_addr_len, ep_addr_len;

	unsigned long int my_buf_addr, dest_buf_addr;

	int comp;

	tid = omp_get_thread_num();

	/* Create an asynchronous context for the UCT worker */
	status = ucs_async_context_create(UCS_ASYNC_MODE_THREAD, &async);
	// The mode can either be SIGNAL, THREAD, POLL, LAST. TODO: Pasha, what do these mean?
	if (status != UCS_OK) {
		fprintf(stderr, "Error in creating UCS Async Context\n");
		ret = EXIT_FAILURE;
		exit(0);
	}

	/* Create a UCT worker */
	status = uct_worker_create(async, UCS_THREAD_MODE_SINGLE, &worker);
	// The thread mode decides the multi-thread access type to the worker. TODO: multi-thread
	if (status != UCS_OK) {
		fprintf(stderr, "Error in creating UCT Worker\n");
		ret = EXIT_FAILURE;
		exit(0);
	}

	/* Query the available Memory Domains */
	status = uct_query_md_resources(&md_resources, &num_md_resources);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in querying UCT MD Resources\n");
		ret = EXIT_FAILURE;
		exit(0);
	}
	
	/* Iterate through the Memory Domains */
	for (md_i = 0; md_i < num_md_resources; md_i++) {
		status = uct_md_config_read(md_resources[md_i].md_name, NULL, NULL, &md_config);
		if (status != UCS_OK) {
			fprintf(stderr, "Error in reading UCT MD's config\n");
			ret = EXIT_FAILURE;
			exit(0);
		}

		status = uct_md_open(md_resources[md_i].md_name, md_config, &md);
		uct_config_release(md_config);
		if (status != UCS_OK) {
			fprintf(stderr, "Error in opening UCT MD\n");
			ret = EXIT_FAILURE;
			exit(0);
		}

		/* Query the Memory Domain for the available communication resources */
		status = uct_md_query_tl_resources(md, &tl_resources, &num_tl_resources);
		if (status != UCS_OK) {
			fprintf(stderr, "Error in querying UCT TL Resources\n");
			ret = EXIT_FAILURE;
			exit(0);
		}

		/*Iterate through the available transport and see if we match the requested ones */
		for (tl_i = 0; tl_i < num_tl_resources; tl_i++) {
			if (!strcmp(device_name, tl_resources[tl_i].dev_name) &&
				!strcmp(transport_name, tl_resources[tl_i].tl_name)) {
				fprintf(stderr, "Using transport %s and device %s\n", tl_resources[tl_i].tl_name, tl_resources[tl_i].dev_name);
				uct_release_tl_resource_list(tl_resources);
				uct_release_md_resource_list(md_resources);
				goto found_match;
			}
		}
		uct_release_tl_resource_list(tl_resources);
		uct_md_close(md);
	}

	fprintf(stderr, "Error in find the requested device and transport combo\n");
	ret = EXIT_FAILURE;
	exit(0);

found_match:
	/* Open interface */
	status = uct_md_iface_config_read(md, transport_name, NULL, NULL, &iface_config);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in reading UCT MD IFACE's Config\n");
		ret = EXIT_FAILURE;
		exit(0);
	}

	UCS_CPU_ZERO(&iface_params.cpu_mask);
	iface_params.open_mode = UCT_IFACE_OPEN_MODE_DEVICE;
	iface_params.mode.device.tl_name = transport_name;
	iface_params.mode.device.dev_name = device_name;
	iface_params.stats_root = NULL; // TODO: if no work, change to ucs_stats_get_root
	iface_params.rx_headroom = 0;	

	status = uct_iface_open(md, worker, &iface_params, iface_config, &iface);
	uct_config_release(iface_config);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in opening UCT IFACE\n");
		ret = EXIT_FAILURE;
		exit(0);
	}

	/* Allocate memory */
	status = uct_iface_mem_alloc(iface, message_size, UCT_MD_MEM_ACCESS_ALL, "iface_buf_mem", &buf_mem);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in allocating UCT iface memory\n");
		ret = EXIT_FAILURE;
		exit(0);
	}
	my_buf = buf_mem.address;
	my_buf_addr = (unsigned long int) buf_mem.address;

	/* Query and get addresses */
	status = uct_iface_query(iface, &iface_attr);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in querying UCT iface\n");
		ret = EXIT_FAILURE;
		exit(0);
	}
	dev_addr_len = iface_attr.device_addr_len;
	iface_addr_len = iface_attr.iface_addr_len;
	ep_addr_len = iface_attr.ep_addr_len;
	
	status = uct_md_query(md, &md_attr);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in querying UCT Memory Domain\n");
		ret = EXIT_FAILURE;
		exit(0);
	}

	if (md_attr.cap.flags & (UCT_MD_FLAG_ALLOC|UCT_MD_FLAG_REG))
		rkey_buffer_size = md_attr.rkey_packed_size;
	 else
		rkey_buffer_size = 0;

	my_dev_addr = calloc(1, dev_addr_len);
	dest_dev_addr = calloc(1, dev_addr_len);
	if (!my_dev_addr || !dest_dev_addr) {
		fprintf(stderr, "Error in allocating buffers for mine and destination's device address\n");
		ret = EXIT_FAILURE;
		exit(0);
	}
	status = uct_iface_get_device_address(iface, my_dev_addr);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in getting UCT device address\n");
		ret = EXIT_FAILURE;
		exit(0);
	}
	
	my_iface_addr = calloc(1, iface_addr_len);
	dest_iface_addr = calloc(1, iface_addr_len);
	if (!my_iface_addr || !dest_iface_addr) {
		fprintf(stderr, "Error in allocating buffers for mine and destination's iface address\n");
		ret = EXIT_FAILURE;
		exit(0);
	}
	status = uct_iface_get_address(iface, my_iface_addr);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in getting UCT iface address\n");
		ret = EXIT_FAILURE;
		exit(0);
	}
	
	my_rkey_buffer = calloc(1, rkey_buffer_size);
	dest_rkey_buffer = calloc(1, rkey_buffer_size);
	if (!my_rkey_buffer || !dest_rkey_buffer) {
		fprintf(stderr, "Error in allocating buffers for mine and destination's rkeys\n");
		ret = EXIT_FAILURE;
		exit(0);
	}

	if (rkey_buffer_size > 0) {
		status = uct_md_mkey_pack(md, buf_mem.memh, my_rkey_buffer);
		if (status != UCS_OK) {
			fprintf(stderr, "Error in getting rkey info\n");
			ret = EXIT_FAILURE;
			exit(0);
		}
	}
	
	status = uct_ep_create(iface, &ep);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in creating a UCT endpoint\n");
		ret = EXIT_FAILURE;
		exit(0);
	}
	
	my_ep_addr = calloc(1, ep_addr_len);
	dest_ep_addr = calloc(1, ep_addr_len);
	if (!my_ep_addr || !dest_ep_addr) {
		fprintf(stderr, "Error in allocating buffers for mine and destination's EP addresses\n");
		ret = EXIT_FAILURE;
		exit(0);
	}
	status = uct_ep_get_address(ep, my_ep_addr);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in creating a UCT endpoint\n");
		ret = EXIT_FAILURE;
		exit(0);
	}

	MPI_Barrier(MPI_COMM_WORLD);
	
	if (!rank) {
		// Sender
		int target = 1;
		
	  	/* Exchange rkey */
		MPI_Send(my_rkey_buffer, rkey_buffer_size, MPI_BYTE, target, tid, MPI_COMM_WORLD);
		MPI_Recv(dest_rkey_buffer, rkey_buffer_size, MPI_BYTE, target, tid, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
	  	
		/* Exchange device addresses */
		MPI_Send(my_dev_addr, dev_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD);
		MPI_Recv(dest_dev_addr, dev_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

	  	/* Exchange iface addresses */
		MPI_Send(my_iface_addr, iface_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD);
		MPI_Recv(dest_iface_addr, iface_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

	  	/* Exchange EP addresses */
		MPI_Send(my_ep_addr, ep_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD);
		MPI_Recv(dest_ep_addr, ep_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD, MPI_STATUS_IGNORE);	
		
	  	/* Exchange buffer addresses */
		MPI_Send(&my_buf_addr, 1 , MPI_UNSIGNED_LONG, target, tid, MPI_COMM_WORLD);
		MPI_Recv(&dest_buf_addr, 1, MPI_UNSIGNED_LONG, target, tid, MPI_COMM_WORLD, MPI_STATUS_IGNORE);	
		
	} else {
		// Receiver
		int target = 0;

		/* Exchange rkey */
		MPI_Recv(dest_rkey_buffer, rkey_buffer_size, MPI_BYTE, target, tid, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		MPI_Send(my_rkey_buffer, rkey_buffer_size, MPI_BYTE, target, tid, MPI_COMM_WORLD);
	  	
		/* Exchange device addresses */
		MPI_Recv(dest_dev_addr, dev_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		MPI_Send(my_dev_addr, dev_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD);
	  	
		/* Exchange iface addresses */
		MPI_Recv(dest_iface_addr, iface_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		MPI_Send(my_iface_addr, iface_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD);
	  	
		/* Exchange EP addresses */
		MPI_Recv(dest_ep_addr, ep_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD, MPI_STATUS_IGNORE);	
		MPI_Send(my_ep_addr, ep_addr_len, MPI_BYTE, target, tid, MPI_COMM_WORLD);
		
		/* Exchange buffer addresses */
		MPI_Recv(&dest_buf_addr, 1, MPI_UNSIGNED_LONG, target, tid, MPI_COMM_WORLD, MPI_STATUS_IGNORE);	
		MPI_Send(&my_buf_addr, 1 , MPI_UNSIGNED_LONG, target, tid, MPI_COMM_WORLD);
	}

	if (!uct_iface_is_reachable(iface, dest_dev_addr, dest_iface_addr)) {
		fprintf(stderr, "Destination address is not reachable\n");
		ret = EXIT_FAILURE;
		exit(0);
	}

	status = uct_rkey_unpack(dest_rkey_buffer, &dest_rkey_bundle);
	if (status != UCS_OK) {
		fprintf(stderr, "Error in unpacking the destination's rkey\n");
		ret = EXIT_FAILURE;
		exit(0);
	}

	status = uct_ep_connect_to_ep(ep, dest_dev_addr, dest_ep_addr);	
	if (status != UCS_OK) {
		fprintf(stderr, "Error in connecting with the destination EP\n");
		ret = EXIT_FAILURE;
		exit(0);
	}

	uct_iface_progress_enable(iface, UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);

	MPI_Barrier(MPI_COMM_WORLD);
	
	int num_posts = 0;
	int window_post;
	int num_completed = 0;
#pragma omp single
	{
	MPI_Barrier(MPI_COMM_WORLD);
	}
	if (!rank) {
		t_start = MPI_Wtime();
		while (num_posts < num_messages) {
			for (window_post = 0; window_post < window_size; window_post++) {
			/* Post */
				status = uct_ep_put_short(ep, my_buf, message_size, dest_buf_addr, dest_rkey_bundle.rkey);
				if (status != UCS_OK) {
					fprintf(stderr, "Error in RDMA write of remote memory; Post# %d, %d completed\n", num_posts+window_post, num_completed);
					ret = EXIT_FAILURE;
					exit(0);
				}
			}
			num_posts += window_size;
			/* Progress */
			do {
				comp = uct_worker_progress(worker);
				num_completed += comp;
			} while (comp != 0);
		}
		t_elapsed[tid] = MPI_Wtime() - t_start;
#pragma omp single
		{
		MPI_Barrier(MPI_COMM_WORLD);
		}
		printf("%d: Num_posts: %d; Num completed: %d\n", tid, num_posts, num_completed);
		my_msg_rate[tid] = (double) num_posts / t_elapsed[tid] / 1e6;
#pragma omp single
		{
			int tid_i;
			double tot_msg_rate = 0;
			for (tid_i = 0; tid_i < num_threads; tid_i++)
				tot_msg_rate += my_msg_rate[tid];
			printf("Message Rate: %10.2f\n", tot_msg_rate);
		}
	} else {
#pragma omp single
		{
		MPI_Barrier(MPI_COMM_WORLD);
		}
	}	
	}

exit:
	return ret;	
}

int main (int argc, char *argv[])
{
	int op, ret;

	MPI_Init(&argc, &argv);
	
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	if (size != 2) {
		fprintf(stderr, "Supporting only two processes at the moment\n");
		ret = EXIT_FAILURE;
		goto clean_mpi;
	}

	MPI_Comm_rank(MPI_COMM_WORLD, &rank);

	// TODO: move this to a struct called test_params and initialize using a function
	device_name = NULL;
	
	message_size = DEF_MESSAGE_SIZE;
	window_size = DEF_WINDOW_SIZE;
	num_messages = DEF_NUM_MESSAGES;
	num_warmup_messages = DEF_NUM_WARMUP_MESSAGES;
	num_threads = DEF_NUM_THREADS;

	struct option long_options[] = {
		{.name = "device",			.has_arg = 1, .val = 'd'},
		{.name = "transport",		.has_arg = 1, .val = 't'},
		{.name = "size",			.has_arg = 1, .val = 's'},
		{.name = "window",			.has_arg = 1, .val = 'w'},
		{.name = "messages",		.has_arg = 1, .val = 'm'},
		{.name = "warmup_messages",	.has_arg = 1, .val = 'u'},
		{.name = "threads",			.has_arg = 1, .val = 'T'},
		{0 , 0 , 0, 0}
	};

	while (1) {
		op = getopt_long(argc, argv, "h?d:t:s:w:m:u:T:", long_options, NULL);
		if (op == -1)
			break;
		
		switch (op) {
			case '?':
			case 'h':
				print_usage(argv[0]);
				return -1;
			default:
				parse_args(op, optarg);
				break;
		}
	}

	if (optind < argc) {
		print_usage(argv[0]);
		return -1;
	}

	omp_set_num_threads(num_threads);

	ret = run_bench();
	if (ret) {
		fprintf(stderr, "Error in running bench \n");
		ret = EXIT_FAILURE;
	}

clean_mpi:
	MPI_Finalize();
	
	return ret;
}
