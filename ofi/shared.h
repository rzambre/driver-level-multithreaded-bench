#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_tagged.h>

#define BENCHMARK_OPTS "vkj:W:"

#define FT_PRINTERR(call, retv) \
	do { fprintf(stderr, call "(): %s:%d, ret=%d (%s)\n", __FILE__, __LINE__, \
			(int) (retv), fi_strerror((int) -(retv))); } while (0)

#define MIN(a,b) (((a)<(b))?(a):(b))                                                     
#define MAX(a,b) (((a)>(b))?(a):(b))
#define ADDR_OPTS "B:P:s:a:"
#define FAB_OPTS "f:d:p:"
#define INFO_OPTS FAB_OPTS "e:"
#define CS_OPTS ADDR_OPTS "I:S:mc:t:w:l"
#define FT_DEFAULT_SIZE		(1 << 0)
#define FT_OPTS_USAGE_FORMAT "%-30s %s"
#define FT_PRINT_OPTS_USAGE(opt, desc) fprintf(stderr, FT_OPTS_USAGE_FORMAT "\n", opt, desc)
#define INIT_OPTS (struct ft_opts) \
	{	.options = FT_OPT_RX_CQ | FT_OPT_TX_CQ, \
		.iterations = 1000, \
		.warmup_iterations = 10, \
		.transfer_size = 1024, \
		.window_size = 64, \
		.sizes_enabled = FT_DEFAULT_SIZE, \
		.argc = argc, .argv = argv \
	}

#define FT_CLOSE_FID(fd)						\
	do {								\
		int ret;						\
		if ((fd)) {						\
			ret = fi_close(&(fd)->fid);			\
			if (ret)					\
				fprintf(stderr, "fi_close: %s(%d) fid %d",	\
					fi_strerror(-ret), 		\
					ret,				\
					(int) (fd)->fid.fclass);	\
			fd = NULL;					\
		}							\
	} while (0)

#define FT_CLOSEV_FID(fd, cnt)			\
	do {					\
		int i;				\
		if (!(fd))			\
			break;			\
		for (i = 0; i < (cnt); i++) {	\
			FT_CLOSE_FID((fd)[i]);	\
		}				\
	} while (0)

enum {
	FT_OPT_ACTIVE		= 1 << 0,
	FT_OPT_ITER		= 1 << 1,
	FT_OPT_SIZE		= 1 << 2,
	FT_OPT_RX_CQ		= 1 << 3,
	FT_OPT_TX_CQ		= 1 << 4,
	FT_OPT_RX_CNTR		= 1 << 5,
	FT_OPT_TX_CNTR		= 1 << 6,
	FT_OPT_VERIFY_DATA	= 1 << 7,
	FT_OPT_ALIGN		= 1 << 8,
	FT_OPT_BW		= 1 << 9,
};

enum {
	FT_UNSPEC,
	FT_EP_CNT,
};

enum precision {
	NANO = 1,
	MICRO = 1000,
	MILLI = 1000000,
	SEC = 1000000000,
};

struct test_size_param {
	int size;
	int enable_flags;
};

struct ft_opts {
	int iterations;
	int warmup_iterations;
	int transfer_size;
	int window_size;
	char *src_port;
	char *dst_port;
	char *src_addr;
	char *dst_addr;
	//char *av_name;
	int sizes_enabled;
	int options;
	//enum ft_comp_method comp_method;
	//int machr;
	int argc;
	char **argv;
};

extern struct fi_info *fi, *fi_dup, *hints, *fi_pep;
extern struct fid_pep *pep; // not needed but just to make it work 
extern struct fid_mr *mr, no_mr;
extern struct fid_fabric *fabric;
extern struct fid_domain *domain;
extern struct fid_cq *txcq, *rxcq;
extern struct fid_cntr *txcntr, *rxcntr; // not really needed but need it in ft_inject_progress
extern struct fid_av *av;
extern struct fid_eq *eq;
extern struct fi_context tx_ctx, rx_ctx;
extern struct fi_context *tx_ctx_arr, *rx_ctx_arr;
extern struct fid_wait *waitset; // not really needed
extern struct fi_eq_attr eq_attr;
extern struct fi_av_attr av_attr;
extern struct fi_cq_attr cq_attr;
extern struct fi_cntr_attr cntr_attr;
extern struct fid_ep *ep;
//extern struct remote_fi_addr;

extern struct ft_opts opts;
extern struct timespec start, end;
extern int timeout;
extern char *buf, *tx_buf, *rx_buf;
extern size_t buf_size, tx_size, rx_size;
extern uint64_t tx_seq, rx_seq, tx_cq_cntr, rx_cq_cntr;
extern struct test_size_param bw_test_sizes[];
extern const unsigned int bw_test_cnt;
extern char default_port[8];

