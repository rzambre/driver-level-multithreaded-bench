#include <inttypes.h>

#define CACHE_LINE_SIZE             64

struct test_size_param {
	int size;
	int enable_flags;
};

struct qp_flow_vars {
	int rem_qp_depth;
	char padding[CACHE_LINE_SIZE - 1*sizeof(int)];
};

struct thread_flow_vars {
	int comp_count;
	char padding[CACHE_LINE_SIZE - 1*sizeof(int)];
};

extern char* dev_name;
extern int all_sizes;
extern int user_messages;
extern int transfer_size;
extern int messages;
extern int warmup_messages;
extern int qp_depth;
extern int num_threads;
extern int num_qps;
extern int postlist;
extern int mod_comp;
extern int no_inlining;
extern int use_td;
extern int use_cq_ex;
extern int way_ctx_sharing;
extern int way_pd_sharing;
extern int way_mr_sharing;
extern int way_buf_sharing;
extern int way_cq_sharing;
extern int way_qp_sharing;
extern struct test_size_param bw_test_sizes[];
extern const unsigned int bw_test_cnt;

static inline int use_size(int index);
static inline int64_t min(int64_t a, int64_t b);
static inline int64_t max(int64_t a, int64_t b);
static inline uint64_t get_cycles();

void parse_args(int op, char *optarg);
void print_usage(const char *argv0, int res_sharing_bench);
void print_resSharing_usage(const char *argv0);
void show_perf(int tsize, int messages_per_thread, double *elapsed);
int lcm(int a, int b);
int gcd(int a, int b);

static inline int use_size(int index)
{
	return (bw_test_sizes[index].enable_flags);
}

static inline int64_t max(int64_t a, int64_t b)
{
	return (a > b) ? a : b;
}

static inline int64_t min(int64_t a, int64_t b)
{
	return (a < b) ? a : b;
}

static inline uint64_t get_cycles()
{
	unsigned hi, lo;
	/* rdtscp modifies $ecx (=$rcx) register. */
	__asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi) : : "rcx");
	uint64_t cycle = ((uint64_t)lo) | (((int64_t)hi) << 32);
	return cycle;
}
