#define _GNU_SOURCE
#define _SVID_SOURCE
#define _XOPEN_SOURCE

#include "shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <time.h>

#include <mpi.h>
#include <omp.h>
#include <infiniband/verbs.h>

#define ITERS_SMALL					(8388608) // 2e23
#define ITERS_LARGE					(1048576) // 2e20
#define WARMUP_ITERS_SMALL			(1048576) // 2e20
#define WARMUP_ITERS_LARGE			(131072)  // 2e17
#define LARGE_THRESHOLD				4096
#define DEF_IB_PORT					1
#define DEF_TX_DEPTH 				128
#define DEF_RX_DEPTH 				512
#define DEF_POSTLIST 				32
#define DEF_MOD_COMP 				64
#define DEF_NUM_THREADS				1
#define DEF_ALL_SIZES 				1
#define DEF_NO_INLINING 			0
#define DEF_USE_TD					0
#define DEF_USE_CQ_EX				0

#define DEF_CTX_SHARING				1
#define DEF_PD_SHARING				1
#define DEF_MR_SHARING				1
#define DEF_BUF_SHARING				1
#define DEF_CQ_SHARING				1
#define DEF_QP_SHARING				1

/*
 * In this benchmark, the master thread creates
 * all resources. The threads are used only to
 * drive the critical path.
 * */

struct ibv_device **dev_list;
struct ibv_device *dev;
struct ibv_context **dev_context;
struct ibv_pd **pd;
struct ibv_pd **parent_d;
struct ibv_td **td;
struct ibv_mr **mr;
struct ibv_cq **cq;
struct ibv_cq_ex **cq_ex;
struct ibv_qp **qp;
struct ibv_port_attr ib_port_attr;
struct ibv_send_wr *send_wqe;
struct ibv_recv_wr *recv_wqe;

void **buf;
int ib_port;
int max_inline_size;
int cq_depth;

int num_threads;
int num_ctxs;
int num_pds;
int num_mrs;
int num_bufs;
int num_cqs;
int num_qps;
int buf_size;

int *my_qp_index;
int *dest_qp_index;
int *my_lid;
int *dest_lid;
int *my_psn;
int *dest_psn;
int *my_rkey;
int *dest_rkey;
unsigned long int *my_addr;
unsigned long int *dest_addr;
struct qp_flow_vars *flow_vars_of_qps;
struct thread_flow_vars *flow_vars_of_threads;

int rank, size;
double *t_elapsed;

static int alloc_res(void)
{
	int ret;
	int buf_i;
	
	my_qp_index 	= malloc(num_qps * sizeof(int) ); 
	dest_qp_index 	= malloc(num_qps * sizeof(int) ); 
	my_psn 			= malloc(num_qps * sizeof(int) );  
	dest_psn 		= malloc(num_qps * sizeof(int) );  
	my_lid 			= malloc(num_qps * sizeof(int) ); 
	dest_lid 		= malloc(num_qps * sizeof(int) );
	my_rkey 		= malloc(num_qps * sizeof(int) );
	dest_rkey 		= malloc(num_qps * sizeof(int) );
	my_addr 		= malloc(num_qps * sizeof(unsigned long int) );
	dest_addr 		= malloc(num_qps * sizeof(unsigned long int) );
	t_elapsed		= malloc(num_threads * sizeof(double) );
	posix_memalign( (void**) &flow_vars_of_qps		, CACHE_LINE_SIZE, num_qps 		* sizeof(struct qp_flow_vars	) );
	posix_memalign( (void**) &flow_vars_of_threads	, CACHE_LINE_SIZE, num_threads	* sizeof(struct thread_flow_vars) );
	if (!my_qp_index || !dest_qp_index || !my_psn || !dest_psn || !my_lid || !dest_lid || !my_addr || !dest_addr || !t_elapsed || !flow_vars_of_qps || !flow_vars_of_threads) {
		fprintf(stderr, "Failure in allocating information resources\n");
		return EXIT_FAILURE;
	}

	/* Allocate device contexts */
	dev_context = malloc(num_ctxs * (sizeof *dev_context) );
	if (!dev_context) {
		fprintf(stderr, "Failure in allocating device contexts\n");
		return EXIT_FAILURE;
	}

	/* Allocate protection domains */
	pd = malloc(num_pds * (sizeof *pd) );
	if (!pd) {
		fprintf(stderr, "Failure in allocating protection domains\n");
		return EXIT_FAILURE;
	}

	if (use_td && way_qp_sharing == 1) {
		/* Allocate parent domains */
		parent_d = malloc(num_qps * (sizeof *parent_d) );
		if (!parent_d) {
			fprintf(stderr, "Failure in allocating parent domains\n");
			return EXIT_FAILURE;
		}

		/* Allocate thread domains */
		td = malloc(num_qps * (sizeof *td) );
		if (!td) {
			fprintf(stderr, "Failure in allocating thread domains\n");
			return EXIT_FAILURE;
		}
	}

	/* Allocate array of buffers */
	buf = (void**) malloc(num_bufs * sizeof(void*));
	if (buf == NULL) {
		fprintf(stderr, "Cannot allocate array of buffers\n");
		return EXIT_FAILURE;
	}
	
	/* Allocate contiguous buffer for all the buffers */
	void *contig_buf;
	unsigned int page_size = sysconf(_SC_PAGESIZE);
	buf_size = transfer_size;
	if (buf_size % CACHE_LINE_SIZE)
		buf_size += (CACHE_LINE_SIZE - transfer_size % CACHE_LINE_SIZE); // add padding to fill cache line
	ret = posix_memalign(&contig_buf, page_size, num_bufs * buf_size);
	if (ret) {
		fprintf(stderr, "Error in posix_memalign\n");
		return ret;
	}
	memset(contig_buf, 0, num_bufs * buf_size);

	/* Assign buffers from the contiguous buffer */
	for (buf_i = 0; buf_i < num_bufs; buf_i++) {
		buf[buf_i] = (char*) contig_buf + buf_i*buf_size;
	}

	/* Allocate memory regions only on receiver here; sender will allocate in init_res (unsure how much needed, if needed) */
	if (rank) {
		mr = malloc(num_mrs * (sizeof *mr) );
		if (!mr) {
			fprintf(stderr, "Failure in allocating memory regions\n");
			return EXIT_FAILURE;
		}
	}
	
	/* Allocate CQs */
	if (use_cq_ex) {
		cq_ex = malloc(num_cqs * (sizeof *cq_ex) );
		if (!cq_ex) {
			fprintf(stderr, "Failure in allocating extended completion queues\n");
			return EXIT_FAILURE;
		}
	} else {
		cq = malloc(num_cqs * (sizeof *cq) );
		if (!cq) {
			fprintf(stderr, "Failure in allocating completion queues\n");
			return EXIT_FAILURE;
		}	
	}

	/* Allocate QPs */
	qp = malloc(num_qps * (sizeof *qp) );
	if (!qp) {
		fprintf(stderr, "Failure in allocating queue pairs\n");
		return EXIT_FAILURE;
	}

	return 0; 
}

static int init_res(void)
{
	
	int ret;
	int dev_i, ctx_i, pd_i, mr_i, buf_i, cq_i, qp_i;

	/* Acquire Device List */
	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		fprintf(stderr, "Failed to get IB devices list");
		return EXIT_FAILURE;
	}

	/* Acquire Device Name */

	/* If a device name wasn't specified, use the
	*  first device found. Otherwise, iterate through
	*  the list to find the device with a matching name
	*/
	if (!dev_name) {
		dev = *dev_list;
		if (!dev) {
			fprintf(stderr, "No IB devices found\n");
			return EXIT_FAILURE;
		}
	} else {
		for (dev_i = 0; dev_list[dev_i]; ++dev_i)
			if (!strcmp(ibv_get_device_name(dev_list[dev_i]), dev_name))
				break;
		dev = dev_list[dev_i];
		if (!dev) {
			fprintf(stderr, "IB device %s not found\n", dev_name);
			return EXIT_FAILURE;
		}
	}

	/* Acquire Device Contexts */	
	for (ctx_i = 0; ctx_i < num_ctxs; ctx_i++) {
		dev_context[ctx_i] = ibv_open_device(dev);
		if (!dev_context[ctx_i]) {
			fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(dev));
			ret = EXIT_FAILURE;
			goto clean_dev_list;
		}
	}

	/* Open up Protection Domains */
	for (pd_i = 0; pd_i < num_pds; pd_i++) {
		ctx_i = pd_i / (num_pds / num_ctxs);
		pd[pd_i] = ibv_alloc_pd(dev_context[ctx_i]);
		if (!pd[pd_i]) {
			fprintf(stderr, "Couldn't allocate PD\n");
			ret = EXIT_FAILURE;
			goto clean_dev_context;
		}
	}
	
	if (use_td && way_qp_sharing == 1) {
		for (qp_i = 0; qp_i < num_qps; qp_i++) {
			ctx_i = qp_i / (num_qps / num_ctxs);
			pd_i = qp_i / (num_qps / num_pds);
			
			/* Open up Thread Domains */
			struct ibv_td_init_attr td_init;
			td_init.comp_mask = 0;
			td_init.independent = 1;
			td[qp_i] = ibv_alloc_td(dev_context[ctx_i], &td_init);
			if (!td[qp_i]) {
				fprintf(stderr, "Couldn't allocate TD\n");
				ret = EXIT_FAILURE;
				goto clean_pd;
			}

			/* Open up Parent Domains */
			struct ibv_parent_domain_init_attr pd_init;
			pd_init.pd = pd[pd_i];
			pd_init.td = td[qp_i];
			pd_init.comp_mask = 0;
			parent_d[qp_i] = ibv_alloc_parent_domain(dev_context[ctx_i], &pd_init);
			if (!parent_d[qp_i]) {
				fprintf(stderr, "Couldn't allocate Parent D\n");
				ret = EXIT_FAILURE;
				goto clean_td;
			}
		}
	}

	/* Create Completion Queues */
	for (cq_i = 0; cq_i < num_cqs; cq_i++) {
		ctx_i = cq_i / (num_cqs / num_ctxs);
		if (use_cq_ex) {
			struct ibv_cq_init_attr_ex cq_ex_attr = {
				.cqe = cq_depth,
				.cq_context = NULL, 
				.channel = NULL,
				.comp_vector = 0,
				.wc_flags = 0,
				.comp_mask = IBV_CQ_INIT_ATTR_MASK_FLAGS,
				.flags = (way_cq_sharing == 1) ? IBV_CREATE_CQ_ATTR_SINGLE_THREADED : 0,
			};
			cq_ex[cq_i] = ibv_create_cq_ex(dev_context[ctx_i], &cq_ex_attr);
			if (!cq_ex[cq_i]) {
				fprintf(stderr, "Couldn't create extended CQ\n");
				ret = EXIT_FAILURE;
				goto clean_parent_d;
			}
		} else {
			cq[cq_i] = ibv_create_cq(dev_context[ctx_i], cq_depth, NULL, NULL, 0);
			if (!cq[cq_i]) {
				fprintf(stderr, "Couldn't create CQ\n");
				ret = EXIT_FAILURE;
				goto clean_parent_d;
			}
		}
	}

	/* Create Queue Pairs and transition them to the INIT state */
	for (qp_i = 0; qp_i < num_qps; qp_i++) {
		pd_i = qp_i / (num_qps / num_pds);
		mr_i = qp_i / (num_qps / num_mrs);
		cq_i = qp_i / (num_qps / num_cqs);
		ctx_i  = qp_i / (num_qps / num_ctxs);
		struct ibv_qp_init_attr qp_init_attr = {
			.send_cq = (use_cq_ex) ? ibv_cq_ex_to_cq(cq_ex[cq_i]) : cq[cq_i],
			.recv_cq = (use_cq_ex) ? ibv_cq_ex_to_cq(cq_ex[cq_i]) : cq[cq_i], // the same CQ for sending and receiving
			.cap     = {
				.max_send_wr  = (!rank) ? qp_depth : 1, // maximum number of outstanding WRs that can be posted to the SQ in this QP
				.max_recv_wr  = (!rank) ? 1 : qp_depth, // maximum number of outstanding WRs that can be posted to the RQ in this QP
				.max_send_sge = 1,
				.max_recv_sge = 1,
			},
			.qp_type = IBV_QPT_RC,
			.sq_sig_all = 0, // all send_wqes posted will generate a WC
		};

		if (use_td && way_qp_sharing == 1)
			qp[qp_i] = ibv_create_qp(parent_d[qp_i], &qp_init_attr); // this puts the QP in the RESET state
		else	
			qp[qp_i] = ibv_create_qp(pd[pd_i], &qp_init_attr); // this puts the QP in the RESET state
		if (!qp[qp_i]) {
			fprintf(stderr, "Couldn't create QP\n");
			ret = EXIT_FAILURE;
			goto clean_cq;
		}		
		my_qp_index[qp_i] = qp[qp_i]->qp_num;
		my_psn[qp_i] = 0;

		struct ibv_qp_attr attr;
		ret = ibv_query_qp(qp[qp_i], &attr, IBV_QP_CAP, &qp_init_attr);
		if (ret) {
			fprintf(stderr, "Failure in querying the QP\n");
			goto clean_qp;
		}		
		max_inline_size = qp_init_attr.cap.max_inline_data;

		struct ibv_qp_attr qp_attr = {
			.qp_state = IBV_QPS_INIT,
			.pkey_index = 0, // according to examples
			.port_num = ib_port,
			.qp_access_flags = IBV_ACCESS_REMOTE_WRITE, // no atomics
		};

		/* Initialize the QP to the INIT state */
		ret = ibv_modify_qp(qp[qp_i], &qp_attr,
					IBV_QP_STATE		|
					IBV_QP_PKEY_INDEX	|
					IBV_QP_PORT 		|
					IBV_QP_ACCESS_FLAGS);
		if (ret) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_qp;
		}

		/* Query port to get my LID */
		ret = ibv_query_port(dev_context[ctx_i], ib_port, &ib_port_attr);
		if (ret) {
			fprintf(stderr, "Failed to get port info\n");
			return ret;
		}
		my_lid[qp_i] = ib_port_attr.lid;
	}

	if (!rank) {
		int need_mrs = (transfer_size > max_inline_size) || no_inlining; 

		if (need_mrs) {
			/* Allocate memory regions on sender */
			mr = malloc(num_mrs * (sizeof *mr) );
			if (!mr) {
				fprintf(stderr, "Failure in allocating memory regions\n");
				ret = EXIT_FAILURE;
				goto clean_qp;
			}	
		} else {
			for (qp_i = 0; qp_i < num_qps; qp_i++) {
				my_rkey[qp_i] = 0;
				my_addr[qp_i] = 0;
			}
			goto done;	
		}
	}
	
	/* Register Memory */
	int bufs_per_mr = (way_mr_sharing > way_buf_sharing) ? num_bufs / num_mrs : 1;
	int mr_size = bufs_per_mr * buf_size;
	for (mr_i = 0; mr_i < num_mrs; mr_i++) {
		pd_i = mr_i / (num_mrs / num_pds);
		buf_i = (way_mr_sharing > way_buf_sharing) ? (mr_i * bufs_per_mr) : (mr_i / (num_mrs / num_bufs));
		
		mr[mr_i] = ibv_reg_mr(pd[pd_i], buf[buf_i], mr_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
		if (!mr[mr_i]) {
			fprintf(stderr, "Couldn't register MR\n");
			ret = EXIT_FAILURE;
			goto clean_qp;
		}	
	}

	for (qp_i = 0; qp_i < num_qps; qp_i++) {
		mr_i = qp_i / (num_qps / num_mrs);
		buf_i = qp_i / (num_qps / num_bufs);
		my_rkey[qp_i] = mr[mr_i]->rkey;
		my_addr[qp_i] = (unsigned long int) buf[buf_i];
	}
	
done:
	return 0; // success

clean_qp:
	for (qp_i = 0; qp_i < num_qps; qp_i++) { ibv_destroy_qp(qp[qp_i]); }
	
clean_cq:
	for (cq_i = 0; cq_i < num_cqs; cq_i++) { ibv_destroy_cq( (use_cq_ex) ? ibv_cq_ex_to_cq(cq_ex[cq_i]) : cq[cq_i]); }

clean_parent_d:
	if (use_td && way_qp_sharing == 1)
		for (qp_i = 0; qp_i < num_qps; qp_i++) { ibv_dealloc_pd(parent_d[qp_i]); }

clean_td:
	if (use_td && way_qp_sharing == 1)
		for (qp_i = 0; qp_i < num_qps; qp_i++) { ibv_dealloc_td(td[qp_i]); }

clean_pd:
	if (buf) {
		free(buf[0]);
		free(buf);
	}
	for (pd_i = 0; pd_i < num_pds; pd_i++) { ibv_dealloc_pd(pd[pd_i]); }

clean_dev_context:
	for (ctx_i = 0; ctx_i < num_ctxs; ctx_i++) { ibv_close_device(dev_context[ctx_i]); }

clean_dev_list:
	ibv_free_device_list(dev_list);

	return ret; // error
}

static int exchange_addr(void)
{

	int i;

	if (!rank) {
		// Sender
		int target = 1;
		for (i = 0; i < num_qps; i++) {
			MPI_Send(&my_lid[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD);
			MPI_Recv(&dest_lid[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			MPI_Send(&my_qp_index[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD);
			MPI_Recv(&dest_qp_index[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			MPI_Send(&my_psn[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD);
			MPI_Recv(&dest_psn[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			MPI_Send(&my_rkey[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD);
			MPI_Recv(&dest_rkey[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);			

			MPI_Send(&my_addr[i], 1, MPI_UNSIGNED_LONG, target, 0, MPI_COMM_WORLD);
			MPI_Recv(&dest_addr[i], 1, MPI_UNSIGNED_LONG, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);			
		}
	} else {
		// RECEIVER
		int target = 0;
		for (i = 0; i < num_qps; i++) {
			MPI_Recv(&dest_lid[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			MPI_Send(&my_lid[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD);

			MPI_Recv(&dest_qp_index[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			MPI_Send(&my_qp_index[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD);

			MPI_Recv(&dest_psn[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			MPI_Send(&my_psn[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD);

			MPI_Recv(&dest_rkey[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			MPI_Send(&my_rkey[i], 1, MPI_INT, target, 0, MPI_COMM_WORLD);	
			
			MPI_Recv(&dest_addr[i], 1, MPI_UNSIGNED_LONG, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			MPI_Send(&my_addr[i], 1, MPI_UNSIGNED_LONG, target, 0, MPI_COMM_WORLD);
		}
	}	

	return 0;
}

static int ts_init_to_rts()
{
	int i, ret;
	
	/* Transition states: INIT->RTR-> RTS */
	for (i = 0; i < num_qps; i++) {
		
		/* Transition to RTR state */
		// NOTE: all value are according to online example, except where specified
		struct ibv_qp_attr qp_attr = {
			.qp_state = IBV_QPS_RTR,
			.ah_attr = {
				.is_global = 0,
				.dlid = dest_lid[i],
				.sl = 0, // set Service Level to 0 (relates to QoS)
				.src_path_bits = 0,
				.port_num = ib_port,
			},
			.path_mtu = IBV_MTU_4096,
			.dest_qp_num = dest_qp_index[i],
			.rq_psn = dest_psn[i],
			.max_dest_rd_atomic = 16, // according to Anuj's benchmark
			.min_rnr_timer = 12
		};

		ret = ibv_modify_qp(qp[i], &qp_attr,
					IBV_QP_STATE 			|
					IBV_QP_AV			|
					IBV_QP_PATH_MTU			|
					IBV_QP_DEST_QPN			|
					IBV_QP_RQ_PSN			|
					IBV_QP_MAX_DEST_RD_ATOMIC	|
					IBV_QP_MIN_RNR_TIMER);
		if (ret) {
			fprintf(stderr, "Error in transitioning QP %d to RTR state\n", i);
			return ret;
		}

		/* Transition to RTS state */
		// NOTE: all value are according to online example, except where specified
		memset(&qp_attr, 0, sizeof(qp_attr)); // reset
		qp_attr.qp_state = IBV_QPS_RTS;
		qp_attr.sq_psn = my_psn[i];
		qp_attr.timeout = 14; 
		qp_attr.retry_cnt = 7;
		qp_attr.rnr_retry = 7;
		qp_attr.max_rd_atomic = 16; // according to Anuj's benchmark

		ret = ibv_modify_qp(qp[i], &qp_attr,
					IBV_QP_STATE		|
					IBV_QP_SQ_PSN		|
					IBV_QP_TIMEOUT		|
					IBV_QP_RETRY_CNT 	|
					IBV_QP_RNR_RETRY	|
					IBV_QP_MAX_QP_RD_ATOMIC );
		if (ret) {
			fprintf(stderr, "Error in transitioning QP %d to RTS state\n", i);
			return ret;
		}

	}

	return 0;
}

static int bench(int posts_per_thread)
{
	int ret = 0;

	if (!rank) {
		// SENDER
		#pragma omp parallel private(ret) firstprivate(posts_per_thread,postlist,mod_comp,qp,cq,cq_ex,flow_vars_of_threads)
		{
			int tid = omp_get_thread_num();
			int qp_i = tid / (num_threads / num_qps);
			int cq_i = qp_i / (num_qps / num_cqs);
			int mr_i = qp_i / (num_qps / num_mrs);

			// Flow-control indices
			int *my_comp_count = &flow_vars_of_threads[tid].comp_count;
			int *my_rem_qp_depth = &flow_vars_of_qps[qp_i].rem_qp_depth;

			int db, k, cqe_i;
			int doorbells = 0;
			int post_count = 0;
			int comp_count = 0;
			*my_comp_count = 0;
			int cur_rem_qp_depth = 0;
			*my_rem_qp_depth = qp_depth;
			int cqe_count = 0;
			int num_comps = cq_depth / way_cq_sharing; // distributing work between the threads (equals as qp_depth/mod_comp)
			
			int send_inline = 0;
			if (!no_inlining && transfer_size <= max_inline_size)
				send_inline |= IBV_SEND_INLINE;
			int buf_i = qp_i / (num_qps / num_bufs);
			
			double t_start = 0;

			struct ibv_sge SGE;
			struct ibv_wc *WC;
			struct ibv_send_wr *send_wqe;
			posix_memalign( (void**) &WC, CACHE_LINE_SIZE, num_comps * sizeof(struct ibv_wc) );
			posix_memalign( (void**) &send_wqe, CACHE_LINE_SIZE, postlist * sizeof(struct ibv_send_wr) );
			struct ibv_send_wr *bad_send_wqe;

			SGE.addr 	= (uintptr_t) buf[buf_i];
			SGE.length 	= transfer_size;
			SGE.lkey 	= (send_inline) ? 0: mr[mr_i]->lkey;
			
			for (k = 0; k < postlist; k++) {
				send_wqe[k].wr_id				= tid;
				send_wqe[k].next				= (k == postlist-1) ? NULL : &send_wqe[k+1];
				send_wqe[k].sg_list				= &SGE;
				send_wqe[k].num_sge				= 1;
				send_wqe[k].opcode				= IBV_WR_RDMA_WRITE;
				send_wqe[k].send_flags			|= send_inline;
				send_wqe[k].wr.rdma.remote_addr	= dest_addr[qp_i];
				send_wqe[k].wr.rdma.rkey		= dest_rkey[qp_i];
			}
			
			#pragma omp single
			{ // only one thread will execute this
				MPI_Barrier(MPI_COMM_WORLD);
			} // implicit barrier for the threads
			
			if (way_qp_sharing == 1 && way_cq_sharing == 1) {
			// Each thread is driving its own QP-CQ pair i.e. ratio of QP:CQ is 1:1
				if (use_cq_ex) {
					struct ibv_poll_cq_attr attr = {};
				// Critical Path START
					t_start = MPI_Wtime();
					while (post_count < posts_per_thread || comp_count < posts_per_thread) { // use stack's counters
						//printf("TID %d: Postcount %d\tCompcount %d\n", tid, post_count, comp_count);
						// Post
						doorbells = min( (posts_per_thread - post_count), (qp_depth - (post_count - comp_count) ) ) / postlist;
						for (db = 0; db < doorbells; db++) {
							for (k = 0; k < postlist; k++) {
								if ( (post_count+k+1) % mod_comp == 0)
									send_wqe[k].send_flags		= IBV_SEND_SIGNALED;
								else
									send_wqe[k].send_flags		= 0; // TODO: investigate why &= ~IBV_SEND_SIGNALED doesn't work
								send_wqe[k].send_flags			|= send_inline;
							}
							ret = ibv_post_send(qp[qp_i], &send_wqe[0], &bad_send_wqe);
							#ifdef ERRCHK
							if (ret) {
								fprintf(stderr, "Thread %d: Error %d in posting send_wqe on QP\n", tid, ret);
								exit(0);
							}
							#endif
							post_count += postlist;
						}
						// Poll
						do {
							ret = ibv_start_poll(cq_ex[cq_i], &attr);
						} while (ret == ENOENT);
						#ifdef ERRCHK
						if (ret && ret != ENOENT) {
							fprintf(stderr, "Thread %d: Error %d in start poll\n", tid, ret);
							exit(0);
						}
						#endif
						for (cqe_i = 1; cqe_i < num_comps; cqe_i++) {
							ret = ibv_next_poll(cq_ex[cq_i]);
							if (ret == ENOENT)
								break;
							#ifdef ERRCHK
							if (ret && ret != ENOENT) {
								fprintf(stderr, "Thread %d: Failure in next poll on extended CQ: %d\n", tid, ret);
								ibv_end_poll(cq_ex[cq_i]);
								exit(0);
							}
							#endif
						}
						ibv_end_poll(cq_ex[cq_i]);
						comp_count += (cqe_i * mod_comp);
					}
					t_elapsed[tid] = MPI_Wtime() - t_start;
					ibv_end_poll(cq_ex[cq_i]);
				// Critical Path END
				} else {
				// Critical Path START
					t_start = MPI_Wtime();
					//uint64_t post_start, post_cycles = 0;
					//uint64_t poll_start, poll_cycles = 0;
					//int poll_count = 0;
					while (post_count < posts_per_thread || comp_count < posts_per_thread) { // use stack's counters
						//printf("TID %d: Postcount %d\tCompcount %d\n", tid, post_count, comp_count);
						// Post
						//post_start = get_cycles();
						doorbells = min( (posts_per_thread - post_count), (qp_depth - (post_count - comp_count) ) ) / postlist;
						for (db = 0; db < doorbells; db++) {
							for (k = 0; k < postlist; k++) {
								if ( (post_count+k+1) % mod_comp == 0)
									send_wqe[k].send_flags		= IBV_SEND_SIGNALED;
								else
									send_wqe[k].send_flags		= 0; // TODO: investigate why &= ~IBV_SEND_SIGNALED doesn't work
								send_wqe[k].send_flags			|= send_inline;
							}
							ret = ibv_post_send(qp[qp_i], &send_wqe[0], &bad_send_wqe);
							#ifdef ERRCHK
							if (ret) {
								fprintf(stderr, "Thread %d: Error %d in posting send_wqe on QP\n", tid, ret);
								exit(0);
							}
							#endif
							post_count += postlist;
						}
						//post_cycles += get_cycles() - post_start;
						// Poll
						//poll_start = get_cycles();
						cqe_count = ibv_poll_cq(cq[cq_i], num_comps, WC);
						#ifdef ERRCHK
						if (cqe_count < 0) {
							fprintf(stderr, "Thread %d: Failure in polling CQ: %d\n", tid, cqe_count);
							exit(0);
						}
						for (cqe_i = 0; cqe_i < cqe_count; cqe_i++) {
							if (WC[cqe_i].status != IBV_WC_SUCCESS) {
								fprintf(stderr, "Thread %d: Failed status %s for %d; cqe_count %d\n", tid,
										ibv_wc_status_str(WC[cqe_i].status), (int) WC[cqe_i].wr_id, cqe_i);
								exit(0);
							}
						}
						#endif
						comp_count += (cqe_count * mod_comp);
						//++poll_count;
						//poll_cycles += get_cycles() - poll_start;
					}
					t_elapsed[tid] = MPI_Wtime() - t_start;
					/*#pragma omp critical
					{
						//printf("%d\t%" PRIu64 "\tcycles per post_send\n", tid, post_cycles / (posts_per_thread/postlist));
						printf("%d\t%" PRIu64 "\tcycles per poll_cq\n", tid, poll_cycles / (poll_count));
					}*/
				// Critical Path END
				}
			}

			if (way_qp_sharing == 1 && way_cq_sharing > 1) {
			// Each thread is driving its own QP. A CQ is shared between multiple QPs. So multiple threads will poll 1 CQ, reading completions of other threads
			// Critical Path START
				t_start = MPI_Wtime();
				while (post_count < posts_per_thread || *my_comp_count < posts_per_thread) { // need to use heap's completion counter
					//printf("Thread %d: Postcount %d\tCompcount %d\n", tid, post_count, *my_comp_count);
					// Post
					doorbells = min( (posts_per_thread - post_count), (qp_depth - (post_count - *my_comp_count) ) ) / postlist;
					for (db = 0; db < doorbells; db++) {
						for (k = 0; k < postlist; k++) {
							if ( (post_count+k+1) % mod_comp == 0)
								send_wqe[k].send_flags		= IBV_SEND_SIGNALED;
							else
								send_wqe[k].send_flags		= 0; // TODO: investigate why &= ~IBV_SEND_SIGNALED doesn't work
							send_wqe[k].send_flags			|= send_inline;
						}
						ret = ibv_post_send(qp[qp_i], &send_wqe[0], &bad_send_wqe);
						#ifdef ERRCHK
						if (ret) {
							fprintf(stderr, "Thread %d: Error %d in posting send_wqe on QP\n", tid, ret);
							exit(0);
						}
						#endif
						post_count += postlist;
					}
					// Poll
					cqe_count = ibv_poll_cq(cq[cq_i], num_comps, WC);
					#ifdef ERRCHK
					if (cqe_count < 0) {
						fprintf(stderr, "Thread %d: Failure in polling CQ: %d\n", tid, cqe_count);
						exit(0);
					}
					#endif
					for (cqe_i = 0; cqe_i < cqe_count; cqe_i++) {
						#pragma omp atomic
						flow_vars_of_threads[WC[cqe_i].wr_id].comp_count += mod_comp; // thread_i could read thread_j's completions
						#ifdef ERRCHK
						if (WC[cqe_i].status != IBV_WC_SUCCESS) {
							fprintf(stderr, "Thread %d: Failed status %s for %d; cqe_count %d\n", tid,
									ibv_wc_status_str(WC[cqe_i].status), (int) WC[cqe_i].wr_id, cqe_i);
							exit(0);
						}
						#endif
					}
				}
				t_elapsed[tid] = MPI_Wtime() - t_start;
			// Critical Path END
			}

			if (way_qp_sharing > 1) {
			// Multiple threads are driving a QP. They coordinate using the remaining QP depth.
			// A CQ may be shared between QPs. Regardless, there will be multiple threads polling 1 CQ each possibly reading other threads' completions
			// Critical Path START
				int decrement_val = max(postlist, mod_comp);
				t_start = MPI_Wtime();
				while (post_count < posts_per_thread || *my_comp_count < posts_per_thread) { // need to use heap's completion counter
					//printf("Thread %d: Postcount %d\tCompcount %d\n", tid, post_count, *my_comp_count);
					// Post
					while ( (cur_rem_qp_depth = *my_rem_qp_depth) ) {
						if (cur_rem_qp_depth / postlist) {
							#pragma omp atomic capture
							{cur_rem_qp_depth = *my_rem_qp_depth; *my_rem_qp_depth -= decrement_val;}
							if (cur_rem_qp_depth <= 0) {
								#pragma omp atomic
								*my_rem_qp_depth += decrement_val;
								break;
							}
							do {
								for (k = 0; k < postlist; k++) {
									if ( (post_count+k+1) % mod_comp == 0)
										send_wqe[k].send_flags      = IBV_SEND_SIGNALED;
									else
										send_wqe[k].send_flags      = 0; // TODO: investigate why &= ~IBV_SEND_SIGNALED doesn't work
									send_wqe[k].send_flags          |= send_inline;
								}
								ret = ibv_post_send(qp[qp_i], &send_wqe[0], &bad_send_wqe);
								#ifdef ERRCHK
								if (ret) {
									fprintf(stderr, "Thread %d: Error %d in posting send_wqe on QP\n", tid, ret);
									exit(0);
								}
								#endif
								post_count += postlist;
							} while (post_count % mod_comp);
						} else
							break;
					}
					// Poll
					cqe_count = ibv_poll_cq(cq[cq_i], num_comps, WC);
					#ifdef ERRCHK
					if (cqe_count < 0) {
						fprintf(stderr, "Thread %d: Failure in polling CQ: %d\n", tid, cqe_count);
						exit(0);
					}
					#endif
					for (cqe_i = 0; cqe_i < cqe_count; cqe_i++) {
						#pragma omp atomic
						flow_vars_of_threads[WC[cqe_i].wr_id].comp_count += mod_comp; // thread_i could read thread_j's completions
						#pragma omp atomic
						*my_rem_qp_depth += mod_comp;
						#ifdef ERRCHK
						if (WC[cqe_i].status != IBV_WC_SUCCESS) {
							fprintf(stderr, "Thread %d: Failed status %s for %d; cqe_count %d\n", tid,
									ibv_wc_status_str(WC[cqe_i].status), (int) WC[cqe_i].wr_id, cqe_i);
							exit(0);
						}
						#endif
					}
				}
				t_elapsed[tid] = MPI_Wtime() - t_start;
			// Critical Path END
			}

			free(send_wqe);
			free(WC);
		}
	} else {
		// RECEIVER
		//struct ibv_recv_wr *bad_recv_wqe; // TODO: when you are incorporating the RECV WQE, you will need this
		MPI_Barrier(MPI_COMM_WORLD);
	}

	return ret;
}

static void compute_messages(void)
{
	if (transfer_size > LARGE_THRESHOLD) {
		warmup_messages = WARMUP_ITERS_LARGE;
		if (!user_messages)
			messages = ITERS_LARGE;
	} else {
		warmup_messages = WARMUP_ITERS_SMALL;
		if (!user_messages)
			messages = ITERS_SMALL;
	}

	/* Convert messages to the next multiple of LCM(num_qps, postlist, mod_comp) if needed */
	int lcm_pq = lcm(postlist, mod_comp);
	int lcm_qpq = lcm(lcm_pq, num_qps);
	messages = (messages % lcm_qpq) ? (messages - (messages % lcm_qpq) + lcm_qpq) : messages;
	warmup_messages = (warmup_messages % lcm_qpq) ? (warmup_messages - (warmup_messages % lcm_qpq) + lcm_qpq) : warmup_messages;
}

static int free_res(void)
{
	int i;
	for (i = 0; i < num_qps; i++) {
		ibv_destroy_qp(qp[i]);
	}
	for (i = 0; i < num_cqs; i++) {
		ibv_destroy_cq( (use_cq_ex) ? ibv_cq_ex_to_cq(cq_ex[i]) : cq[i]);
	}
	if (mr) {
		for (i = 0; i < num_mrs; i++) {
			ibv_dereg_mr(mr[i]);
		}
	}
	for (i = 0; i < num_pds; i++) {
		ibv_dealloc_pd(pd[i]);
	}
	for (i = 0; i < num_ctxs; i++) {
		ibv_close_device(dev_context[i]);
	}
	
	free(buf[0]); // buf[0] points to the contiguous buffer
	free(my_qp_index);
	free(dest_qp_index);
	free(my_psn);
	free(dest_psn);
	free(my_lid);
	free(dest_lid);
	free(my_addr);
	free(dest_addr);
	free(flow_vars_of_qps);
	free(flow_vars_of_threads);
	free(t_elapsed);	

	free(qp);
	free(cq);
	if (mr) {
		free(mr);
	}
	free(buf);
	free(pd);
	free(dev_context);

	return 0;
}

static int run_bench(void)
{
	int ret = 0;

	ret = alloc_res();
	if (ret) {
		fprintf(stderr, "Failure in allocating IB resources\n");
		return ret;
	}	

	ret = init_res();
	if (ret) {
		fprintf(stderr, "Failure in initializing IB resources\n");
		return ret;
	}

	ret = exchange_addr();
	if (ret) {
		fprintf(stderr, "Failure in exchanging addresses\n");
		return ret;
	}

	ret = ts_init_to_rts();
	if (ret) {
		fprintf(stderr, "Failure in transitioning from INIT to RTS state\n");
		return ret;
	}

	// Compute number of messages according to size
	compute_messages();

	#ifdef ERRCHK
	printf("Error checking is ON!\n");
	#endif
	int messages_per_thread = messages / (num_threads / num_threads); 
	ret = bench(messages_per_thread);
	if (ret) {
		fprintf(stderr, "Failure in running bench\n");
		return ret;
	}
	MPI_Barrier(MPI_COMM_WORLD);
	if (!rank) {
		int thread_i;
		for (thread_i = 0; thread_i < num_threads; thread_i++) {
			printf("Thread %d\t%.2f\n", thread_i, t_elapsed[thread_i]);
		}
		printf("\n");
		show_perf(transfer_size, messages_per_thread, t_elapsed);
	}

	ret = free_res();
	if (ret) {
		fprintf(stderr, "Failure in freeing resources\n");
		return ret;
	}

	return ret;
}

static int drive_bench(void)
{
	int size_i, ret = 0;

	if (all_sizes) {
		for (size_i = 0; size_i < bw_test_cnt; size_i++) {
			if (!use_size(size_i))
				continue;
			// Set the message size
			transfer_size = bw_test_sizes[size_i].size;
			// Run bench for this message size
			ret = run_bench();
		}
	} else {
		// Run bench for the user-specified message size
		ret = run_bench();
	}

	return ret;
}

int main (int argc, char *argv[])
{
	int op, ret, provided;

	MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
	if (provided < MPI_THREAD_MULTIPLE) {
		printf("Insufficient threading support\n");
		ret = EXIT_FAILURE;
		goto clean;
	}
	
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	if (size > 2) {
		fprintf(stderr, "Supporting only two processes at the moment\n");
		ret = EXIT_FAILURE;
		goto clean_mpi;
	}

	MPI_Comm_rank(MPI_COMM_WORLD, &rank);

	// TODO: move this to a struct called test_params and initialize using a function
	dev_name = NULL;
	all_sizes = DEF_ALL_SIZES;
	num_threads = DEF_NUM_THREADS;
	qp_depth = !rank ? DEF_TX_DEPTH : DEF_RX_DEPTH;
	mod_comp = DEF_MOD_COMP;
	postlist = DEF_POSTLIST;
	no_inlining = DEF_NO_INLINING;
	use_td = DEF_USE_TD;
	use_cq_ex = DEF_USE_CQ_EX;
	ib_port = DEF_IB_PORT;

	way_ctx_sharing = DEF_CTX_SHARING;
	way_pd_sharing = DEF_PD_SHARING;
	way_mr_sharing = DEF_MR_SHARING;
	way_buf_sharing = DEF_BUF_SHARING;
	way_cq_sharing = DEF_CQ_SHARING;
	way_qp_sharing = DEF_QP_SHARING;

	struct option long_options[] = {
		{.name = "ib-dev",		.has_arg = 1, .val = 'd'},
		{.name = "size",		.has_arg = 1, .val = 'S'},
		{.name = "messages",	.has_arg = 1, .val = 'm'},
		{.name = "qp-depth",	.has_arg = 1, .val = 'D'},
		{.name = "num-threads",	.has_arg = 1, .val = 't'},
		{.name = "postlist",	.has_arg = 1, .val = 'p'},
		{.name = "mod-comp",	.has_arg = 1, .val = 'Q'},
		{.name = "no-inlining",	.has_arg = 0, .val = 'n'},
		{.name = "use-td",		.has_arg = 0, .val = 'o'},
		{.name = "use-cq-ex",	.has_arg = 0, .val = 'x'},
		{.name = "way-ctx-sharing",	.has_arg = 1, .val = 'X'},
		{.name = "way-pd-sharing",	.has_arg = 1, .val = 'P'},
		{.name = "way-mr-sharing",	.has_arg = 1, .val = 'M'},
		{.name = "way-buf-sharing",	.has_arg = 1, .val = 'b'},
		{.name = "way-cq-sharing",	.has_arg = 1, .val = 'C'},
		{.name = "way-qp-sharing",	.has_arg = 1, .val = 'E'},
		{0 , 0 , 0, 0}
	};

	while (1) {
		op = getopt_long(argc, argv, "h?d:S:m:D:t:p:Q:noxX:P:M:b:C:E:", long_options, NULL);
		if (op == -1)
			break;
		
		switch (op) {
			case '?':
			case 'h':
				print_usage(argv[0], 1);
				return -1;
			default:
				parse_args(op, optarg);
				break;
		}
	}

	if (optind < argc) {
		print_usage(argv[0], 1);
		return -1;
	}

	// Compute number of each resource
	if (num_threads % way_qp_sharing) {
		fprintf(stderr, "The number of QPs needs to be a factor of number of threads.\n");
		ret = EXIT_FAILURE;
		goto clean_mpi;
	} else
		num_qps = num_threads / way_qp_sharing;
	if (num_qps % way_ctx_sharing) {
		fprintf(stderr, "The number of CTXs needs to be a factor of number of QPs.\n");
		ret = EXIT_FAILURE;
		goto clean_mpi;
	} else
		num_ctxs = num_qps / way_ctx_sharing;
	if (num_qps % way_pd_sharing || way_pd_sharing > way_ctx_sharing) {
		fprintf(stderr, "The number of PDs needs to be a factor of number of QPs and cannot be shared across CTXs.\n");
		ret = EXIT_FAILURE;
		goto clean_mpi;
	} else
		num_pds = num_qps / way_pd_sharing;
	// Assuming that the MR is needed
	if (num_qps % way_mr_sharing || way_mr_sharing > way_pd_sharing) {
		fprintf(stderr, "The number of MRs needs to be a factor of number of QPs and cannot be shared across PDs.\n");
		ret = EXIT_FAILURE;
		goto clean_mpi;
	} else
		num_mrs = num_qps / way_mr_sharing;
	if (num_qps % way_buf_sharing) {
		fprintf(stderr, "The number of buffers needs to be a factor of number of QPs.\n");
		ret = EXIT_FAILURE;
		goto clean_mpi;
	} else
		num_bufs = num_qps / way_buf_sharing;
	if (num_qps % way_cq_sharing || way_cq_sharing > way_ctx_sharing) {
		fprintf(stderr, "The number of CQs needs to be a factor of number of QPs and cannot be shared accross CTXs.\n");
		ret = EXIT_FAILURE;
		goto clean_mpi;
	} else
		num_cqs = num_qps / way_cq_sharing;

	cq_depth = (qp_depth / mod_comp) * way_cq_sharing;

	/*Start DEBUG*/
	/*if (!rank) {
		printf("SENDER\n");
		int thread_i;
		for (thread_i = 0; thread_i < num_threads; thread_i++) {
			int qp_i = thread_i / (num_threads / num_qps);
			int ctx_i = qp_i / (num_qps / num_ctxs);
			int pd_i = qp_i / (num_qps / num_pds) ;
			int mr_i = qp_i / (num_qps / num_mrs);
			int buf_i = mr_i / (num_mrs / num_bufs);
			int cq_i = qp_i / (num_qps / num_cqs);
			printf("Thread %d\t", thread_i);
			printf("QP %d\t", qp_i);
			printf("CTX %d\t", ctx_i);
			printf("PD %d\t", pd_i);
			printf("MR %d\t", mr_i);
			printf("buf %d\t", buf_i);
			printf("CQ %d\n", cq_i);
		}
		printf("%d Threads, ", num_threads);
		printf("%d CTXs, ", num_ctxs);
		printf("%d PDs, ", num_pds);
		printf("%d MRs, ", num_mrs);
		printf("%d Buffers, ", num_bufs);
		printf("%d CQs, ", num_cqs);
		printf("%d QPs\n", num_qps);

		printf("Inlining: %d\t", !no_inlining);
		printf("Postlist: %d\t", postlist);
		printf("Unsignaled: %d\n", mod_comp);
		MPI_Barrier(MPI_COMM_WORLD);
	} else {
		MPI_Barrier(MPI_COMM_WORLD);
		printf("RECEIVER\n");
		int thread_i;
		for (thread_i = 0; thread_i < num_threads; thread_i++) {
			int qp_i = thread_i / (num_threads / num_qps);
			int ctx_i = qp_i / (num_qps / num_ctxs);
			int pd_i = qp_i / (num_qps / num_pds) ;
			int mr_i = qp_i / (num_qps / num_mrs);
			int buf_i = mr_i / (num_mrs / num_bufs);
			int cq_i = qp_i / (num_qps / num_cqs);
			printf("Thread %d\t", thread_i);
			printf("QP %d\t", qp_i);
			printf("CTX %d\t", ctx_i);
			printf("PD %d\t", pd_i);
			printf("MR %d\t", mr_i);
			printf("buf %d\t", buf_i);
			printf("CQ %d\n", cq_i);
		}
		printf("%d Threads, ", num_threads);
		printf("%d CTXs, ", num_ctxs);
		printf("%d PDs, ", num_pds);
		printf("%d MRs, ", num_mrs);
		printf("%d Buffers, ", num_bufs);
		printf("%d CQs, ", num_cqs);
		printf("%d QPs\n", num_qps);

		printf("Inlining: %d\t", !no_inlining);
		printf("Postlist: %d\t", postlist);
		printf("Unsignaled: %d\n", mod_comp);
	}*/
	/*End DEBUG*/

	omp_set_num_threads(num_threads);

	ret = drive_bench();

clean_mpi:
	MPI_Finalize();
clean:
	return ret;
}
