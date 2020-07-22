#include <inttypes.h>

#define CACHE_LINE_SIZE		64

#define DEF_MESSAGE_SIZE		8
#define DEF_WINDOW_SIZE			64
#define DEF_NUM_MESSAGES		8388608 // 2e23
#define DEF_NUM_WARMUP_MESSAGES	1048576 // 2e20
#define DEF_NUM_THREADS			1
#define DEF_WAY_CTX_SHARING 	1
#define DEF_WAY_WORKER_SHARING 	1

struct test_size_param {
	int size;
	int enable_flags;
};

extern char* device_name;
extern char* transport_name;
extern int all_sizes;
extern int message_size;
extern int window_size;
extern int user_messages;
extern int num_messages;
extern int user_warmup_messages;
extern int num_warmup_messages;
extern int num_threads;
extern int way_ctx_sharing;
extern int way_worker_sharing;
extern struct test_size_param bw_test_sizes[];
extern const unsigned int bw_test_cnt;

int num_ctxs;
int num_workers;

static inline int use_size(int index);
static inline int64_t min(int64_t a, int64_t b);
static inline int64_t max(int64_t a, int64_t b);
static inline uint64_t get_cycles();

void parse_args(int op, char *optarg);
void print_usage(const char *argv0);
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
