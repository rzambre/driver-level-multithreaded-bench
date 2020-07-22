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

#include <ucp/api/ucp.h>
#include <ucp/api/ucp_def.h>

#define ITERS_SMALL					(8388608)       // 2e23
#define ITERS_LARGE					(1048576)       // 2e20
#define WARMUP_ITERS_SMALL			(1048576)       // 2e20
#define WARMUP_ITERS_LARGE			(131072)        // 2e17
#define LARGE_THRESHOLD				4096

/**
 * This benchmark simply sends a message from the
 * sender to the receiver using the UCP API
 */

struct endpoint {
    int tid;
};

struct outstanding {
    int counter;
    char padding[60];
};

int rank, size;
struct outstanding *num_outstanding;
double *t_elapsed;

static void recv_cb(void *request, ucs_status_t status, ucp_tag_recv_info_t * info)
{
    ucp_request_free(request);  // having outstanding receive requests on the receiver is slower
}

static void send_cb(void *request, ucs_status_t status)
{
    struct endpoint *ep = (struct endpoint *) request;

    --(num_outstanding[ep->tid].counter);

    ucp_request_free(request);
}

static void request_init_cb(void *request)
{
    struct endpoint *ep = (struct endpoint *) request;

    ep->tid = -1;
}

static int run_bench(void)
{
    int ret = 0;
    ucs_status_t status;
    size_t worker_address_length;

    ucp_config_t *config;
    ucp_params_t ucp_params;
    ucp_worker_params_t worker_params;
    ucp_ep_params_t ep_params;

    ucp_context_h *context;
    ucp_worker_h *worker;
    ucp_address_t **worker_address, **remote_worker_address;
    ucp_ep_h *ep;

    int ctx_i, worker_i, ep_i;

    // Read UCP configuration from environment
    status = ucp_config_read(NULL, NULL, &config);
    if (status != UCS_OK) {
        fprintf(stderr, "Error in reading UCP configuration\n");
        ret = EXIT_FAILURE;
        goto exit;
    }
    // Learning purpose:
    //ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);

    // Create UCP's communication context
    ucp_params.features = UCP_FEATURE_TAG;
    ucp_params.request_size = sizeof(struct endpoint);
    ucp_params.request_init = request_init_cb;
    ucp_params.estimated_num_eps = way_ctx_sharing;
    ucp_params.mt_workers_shared = (way_ctx_sharing == 1) ? 0 : ((num_workers == num_ctxs) ? 0 : 1);    // TODO: why is thread safety required?
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES |
        UCP_PARAM_FIELD_REQUEST_SIZE |
        UCP_PARAM_FIELD_REQUEST_INIT |
        UCP_PARAM_FIELD_MT_WORKERS_SHARED | UCP_PARAM_FIELD_ESTIMATED_NUM_EPS;

    context = malloc(num_ctxs * sizeof *context);
    if (!context) {
        fprintf(stderr, "Error in allocating memory for UCP contexts\n");
        ret = EXIT_FAILURE;
        goto exit;
    }

    for (ctx_i = 0; ctx_i < num_ctxs; ctx_i++) {
        status = ucp_init(&ucp_params, config, &context[ctx_i]);
        if (status != UCS_OK) {
            fprintf(stderr, "Error in initializing UCP context\n");
            ret = EXIT_FAILURE;
            goto exit;
        }
    }

    // Create a UCP worker
    worker_params.thread_mode = (way_worker_sharing > 1) ?
        UCS_THREAD_MODE_MULTI : UCS_THREAD_MODE_SINGLE;
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;

    worker = malloc(num_workers * sizeof *worker);
    if (!worker) {
        fprintf(stderr, "Error in allocating memory for UCP workers\n");
        ret = EXIT_FAILURE;
        goto exit;
    }

    for (worker_i = 0; worker_i < num_workers; worker_i++) {
        ctx_i = worker_i / (num_workers / num_ctxs);
        status = ucp_worker_create(context[ctx_i], &worker_params, &worker[worker_i]);
        if (status != UCS_OK) {
            fprintf(stderr, "Error in creating the UCP worker\n");
            ret = EXIT_FAILURE;
            goto exit;
        }
    }

    // Get address of a UCP worker
    worker_address = malloc(num_workers * sizeof *worker_address);
    if (!worker_address) {
        fprintf(stderr, "Error in allocating memory for addresses of UCP workers\n");
        ret = EXIT_FAILURE;
        goto exit;
    }

    for (worker_i = 0; worker_i < num_workers; worker_i++) {
        status =
            ucp_worker_get_address(worker[worker_i], &worker_address[worker_i],
                                   &worker_address_length);
        if (status != UCS_OK) {
            fprintf(stderr, "Error in getting the address of a UCP Worker\n");
            ret = EXIT_FAILURE;
            goto exit;
        }
    }

    // Exchange information to connect endpoints
    remote_worker_address = malloc(num_workers * sizeof *remote_worker_address);
    if (!remote_worker_address) {
        fprintf(stderr, "Error in allocating memory for addresses of remote UCP workers\n");
        ret = EXIT_FAILURE;
        goto exit;
    }
    for (worker_i = 0; worker_i < num_workers; worker_i++) {
        remote_worker_address[worker_i] = malloc(worker_address_length);
    }

    if (rank) {
        // Receiver
        for (worker_i = 0; worker_i < num_workers; worker_i++) {
            MPI_Recv(remote_worker_address[worker_i], worker_address_length, MPI_BYTE, 0, 0,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Send(worker_address[worker_i], worker_address_length, MPI_BYTE, 0, 0,
                     MPI_COMM_WORLD);
        }
    } else {
        // Sender
        for (worker_i = 0; worker_i < num_workers; worker_i++) {
            MPI_Send(worker_address[worker_i], worker_address_length, MPI_BYTE, 1, 0,
                     MPI_COMM_WORLD);
            MPI_Recv(remote_worker_address[worker_i], worker_address_length, MPI_BYTE, 1, 0,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }
    }
    for (worker_i = 0; worker_i < num_workers; worker_i++) {
        ucp_worker_release_address(worker[worker_i], worker_address[worker_i]);
    }

    // Create UCP endpoints
    ep = malloc(num_threads * sizeof *ep);
    if (!ep) {
        fprintf(stderr, "Error in allocating memory for UCP endpoints\n");
        ret = EXIT_FAILURE;
        goto exit;
    }

    for (ep_i = 0; ep_i < num_threads; ep_i++) {
        worker_i = ep_i / (num_threads / num_workers);

        ep_params.address = remote_worker_address[worker_i];
        ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;

        status = ucp_ep_create(worker[worker_i], &ep_params, &ep[ep_i]);
        if (status != UCS_OK) {
            fprintf(stderr, "Error in creating a UCP Endpoint\n");
            ret = EXIT_FAILURE;
            goto exit;
        }
    }

    // Flush worker to complete wireup TODO: is this really needed?
    for (worker_i = 0; worker_i < num_workers; worker_i++) {
        status = ucp_worker_flush(worker[worker_i]);
        if (status != UCS_OK) {
            fprintf(stderr, "Error in flushing a UCP Worker\n");
            ret = EXIT_FAILURE;
            goto exit;
        }
    }

    t_elapsed = calloc(num_threads, sizeof *t_elapsed);
    if (!t_elapsed) {
        fprintf(stderr, "Error in allocating memory for array of elapsed time\n");
        ret = EXIT_FAILURE;
        goto exit;
    }

    num_outstanding = calloc(num_threads, sizeof *num_outstanding);
    if (!num_outstanding) {
        fprintf(stderr,
                "Error in allocating memory for array of counters for outstanding requests\n");
        ret = EXIT_FAILURE;
        goto exit;
    }

    if (rank) {
        // Receiver
#pragma omp parallel private(ret) firstprivate(num_messages, worker)
        {
            int tid, worker_i;
            int recv_tag;
            uint64_t tag_mask;
            int recv_buffer_size;
            void *recv_buffer;
            struct endpoint *request;
            //int max_outstanding;
            int num_posts;

            tid = omp_get_thread_num();
            worker_i = tid / (num_threads / num_workers);

            recv_buffer_size = message_size;
            recv_buffer = malloc(recv_buffer_size);
            recv_tag = tid;
            tag_mask = 0xffffffffffffffff;

            num_posts = 0;
            //max_outstanding = 16;

            *((double *) recv_buffer) = 0;

#pragma omp single
            {
                MPI_Barrier(MPI_COMM_WORLD);
            }
            // Post receive
            while (num_posts < num_messages) {
                request = ucp_tag_recv_nb(worker[worker_i], recv_buffer, recv_buffer_size,
                                          ucp_dt_make_contig(1), recv_tag, tag_mask, recv_cb);
#ifdef ERRCHK
                if (UCS_PTR_IS_ERR(request)) {
                    fprintf(stderr, "Error in tagged recv on thread %d\n", omp_get_thread_num());
                    ret = EXIT_FAILURE;
                    exit(ret);
                }
#endif
                // Progress the posted receive
                while (!ucp_request_is_completed(request)) {
                    ucp_worker_progress(worker[worker_i]);
                }
                ++num_posts;
            }
            // Progress all outstanding requests
            free(recv_buffer);
        }
    } else {
        // Sender
#pragma omp parallel private(ret) firstprivate(num_messages, ep, worker, t_elapsed)
        {
            int tid, worker_i;
            int send_tag, send_buffer_size;
            void *send_buffer;
            struct endpoint *request;
            int max_outstanding;
            int num_posts;
            double t_start;

            tid = omp_get_thread_num();
            worker_i = tid / (num_threads / num_workers);

            send_buffer_size = message_size;
            send_buffer = malloc(send_buffer_size);
            *((double *) send_buffer) = 10;
            send_tag = tid;

            num_posts = 0;
            max_outstanding = 16;

            // Post send
#pragma omp single
            {
                MPI_Barrier(MPI_COMM_WORLD);
            }
            t_start = MPI_Wtime();
            while (num_posts < num_messages) {
                // How many times to call progress inside
                // the while loop could be a control variable.
                while (num_outstanding[tid].counter > max_outstanding) {
                    ucp_worker_progress(worker[worker_i]);
                }
                request =
                    ucp_tag_send_nb(ep[tid], send_buffer, send_buffer_size, ucp_dt_make_contig(1),
                                    send_tag, send_cb);
#ifdef ERRCHK
                if (UCS_PTR_IS_ERR(request)) {
                    fprintf(stderr, "Error in tagged send\n");
                    ret = EXIT_FAILURE;
                    exit(ret);
                }
#endif
                if (UCS_PTR_STATUS(request) != UCS_OK) {
                    // This is an outstanding send request
                    request->tid = tid;
                    ++(num_outstanding[tid].counter);
                }
                ++num_posts;
            }
            // Progress all outstanding requests
            while (num_outstanding[tid].counter > 0) {
                ucp_worker_progress(worker[worker_i]);
            }
            t_elapsed[tid] = MPI_Wtime() - t_start;
            free(send_buffer);
        }
        // Calculate the message rate
        int thread_i;
        double msg_rate, my_msg_rate;

        printf("%-10s\t%-10s\n", "Thread", "Mmsgs/s");
        msg_rate = 0;
        for (thread_i = 0; thread_i < num_threads; thread_i++) {
            my_msg_rate = num_messages / t_elapsed[thread_i] / 1e6;
            printf("%-10d\t%-10.2f\n", thread_i, my_msg_rate);
            msg_rate += my_msg_rate;
        }
        printf("\n%-10s\t%-10s\t%-10s\n", "Size", "Threads", "Mmsgs/s");
        printf("%-10d\t", message_size);
        printf("%-10d\t", num_threads);
        printf("%10.2f\n", msg_rate);
    }

    MPI_Barrier(MPI_COMM_WORLD);

    for (ep_i = 0; ep_i < num_threads; ep_i++) {
        ucp_ep_destroy(ep[ep_i]);
    }
    for (worker_i = 0; worker_i < num_workers; worker_i++) {
        free(remote_worker_address[worker_i]);
        ucp_worker_destroy(worker[worker_i]);
    }
    for (ctx_i = 0; ctx_i < num_ctxs; ctx_i++) {
        ucp_cleanup(context[ctx_i]);
    }
    free(remote_worker_address);
    free(worker_address);
    free(ep);
    free(worker);
    free(context);
    free(num_outstanding);
    free(t_elapsed);

  exit:
    return ret;
}

int main(int argc, char *argv[])
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

    message_size = DEF_MESSAGE_SIZE;
    num_messages = DEF_NUM_MESSAGES;
    num_threads = DEF_NUM_THREADS;
    way_ctx_sharing = DEF_WAY_CTX_SHARING;
    way_worker_sharing = DEF_WAY_WORKER_SHARING;

    struct option long_options[] = {
        {.name = "size",.has_arg = 1,.val = 's'},
        {.name = "messages",.has_arg = 1,.val = 'm'},
        {.name = "threads",.has_arg = 1,.val = 'T'},
        {.name = "way-ctx-sharing",.has_arg = 1,.val = 'C'},
        {.name = "way-worker-sharing",.has_arg = 1,.val = 'W'},
        {0, 0, 0, 0}
    };

    while (1) {
        op = getopt_long(argc, argv, "h?s:m:T:C:W:", long_options, NULL);
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

    if (num_threads % way_ctx_sharing) {
        fprintf(stderr, "The number of UCP contexts must be a factor of the number of threads\n");
        ret = EXIT_FAILURE;
        goto clean_mpi;
    } else
        num_ctxs = num_threads / way_ctx_sharing;
    if (num_threads % way_worker_sharing || way_worker_sharing > way_ctx_sharing) {
        fprintf(stderr,
                "The number of UCP workers must be a factor of the number of threads and cannot be shared across UCP contexts\n");
        ret = EXIT_FAILURE;
        goto clean_mpi;
    } else
        num_workers = num_threads / way_worker_sharing;

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
