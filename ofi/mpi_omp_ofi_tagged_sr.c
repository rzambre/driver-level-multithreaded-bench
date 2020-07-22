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

#include <rdma/fabric.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>

#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

#define PAGE_SIZE               4096

#define DEF_NUM_CQ_ENTRIES      8
#define DEF_RX_CTX_BITS         8
#define DEF_OUTSTANDING_RECVS   64

#define DEF_NUM_THREADS         1
#define DEF_MESSAGE_SIZE        8
#define DEF_NUM_MESSAGES        128

/**
 * This is a multiprocess message rate benchmark that uses
 * OFI the way MPICH uses it for sends and receives.
 * - message size: 8 bytes
 * - send: fi_tinjectdata
 * - receive: fi_trecv
 * - Scalable endpoint
 */

int rank, size;

int num_messages;
int message_size;
int num_threads;
int num_outstanding;

typedef struct thread_context {
    struct fid_ep *tx_ctx;
    struct fid_ep *rx_ctx;
    struct fid_cq *cq;
    fi_addr_t other_side_ctx_addr;
    char padding[64];
} thread_context_t;

static int run_bench(void)
{
    int ret = 0, thread_i;
    double *ts_elapsed = NULL;
    thread_context_t *thread_ctx;

    struct fi_info *hints, *prov;
    struct fid_fabric *fabric;
    struct fid_domain *domain;

    struct fi_cq_attr cq_attr;

    struct fi_av_attr av_attr;
    struct fid_av *av;
    int addr_count;

    struct fid_ep *sep;

    struct fi_tx_attr tx_attr;
    struct fi_rx_attr rx_attr;
    fi_addr_t remote_fi_addr;
    char local_addr[FI_NAME_MAX];
    char remote_addr[FI_NAME_MAX];
    size_t addrlen;

    /* Set hints */
    hints = fi_allocinfo();
    if (!hints) {
        printf("Failure in fi_allocinfo\n");
        goto exit;
    }

    hints->fabric_attr->prov_name = "psm2";

    hints->mode = FI_CONTEXT | FI_CONTEXT2 | FI_ASYNC_IOV | FI_RX_CQ_DATA;

    hints->caps = 0ULL;
    hints->caps |= FI_RMA;
    hints->caps |= FI_WRITE;
    hints->caps |= FI_READ;
    hints->caps |= FI_REMOTE_WRITE;
    hints->caps |= FI_REMOTE_READ;
    hints->caps |= FI_ATOMICS;
    hints->caps |= FI_TAGGED;
    hints->caps |= FI_DIRECTED_RECV;
    hints->caps |= FI_MSG;
    hints->caps |= FI_MULTI_RECV;

    hints->addr_format = FI_FORMAT_UNSPEC;

    hints->domain_attr->cq_data_size = 4;
    hints->domain_attr->threading = FI_THREAD_COMPLETION;
    hints->domain_attr->data_progress = FI_PROGRESS_MANUAL;
    hints->domain_attr->control_progress = FI_PROGRESS_MANUAL;
    hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
    hints->domain_attr->av_type = FI_AV_TABLE;
    hints->domain_attr->mr_mode = FI_MR_SCALABLE;
    hints->domain_attr->mode = FI_RESTRICTED_COMP;

    hints->tx_attr->op_flags = FI_COMPLETION;
    hints->tx_attr->op_flags |= FI_DELIVERY_COMPLETE;
    hints->tx_attr->msg_order = FI_ORDER_SAS;
    hints->tx_attr->msg_order |= FI_ORDER_RAR | FI_ORDER_RAW | FI_ORDER_WAR | FI_ORDER_WAW;
    hints->tx_attr->comp_order = FI_ORDER_NONE;

    hints->rx_attr->op_flags = FI_COMPLETION;
    hints->rx_attr->total_buffered_recv = 0;

    hints->ep_attr->type = FI_EP_RDM;
    hints->ep_attr->mem_tag_format = (0x0006000000000000ULL) | 0 | 0 | (0x000000007FFFFFFFULL);

    /* Get a provider that matches the hints */
    ret = fi_getinfo(FI_VERSION(1, 7), NULL, NULL, 0ULL, hints, &prov);
    if (ret) {
        printf("Failure in fi_getinfo\n");
        goto exit;
    }

    /* Open fabric */
    ret = fi_fabric(prov->fabric_attr, &fabric, NULL);
    if (ret) {
        printf("Failure in fi_fabric\n");
        goto exit;
    }

    /* Open domain */
    fi_domain(fabric, prov, &domain, NULL);
    if (ret) {
        printf("Failure in fi_domain\n");
        goto exit;
    }

    /* Create address vector */
    memset(&av_attr, 0, sizeof(av_attr));
    av_attr.type = FI_AV_TABLE;
    av_attr.rx_ctx_bits = DEF_RX_CTX_BITS;
    av_attr.map_addr = 0;
    av_attr.flags = 0;
    av_attr.name = NULL;

    ret = fi_av_open(domain, &av_attr, &av, NULL);
    if (ret) {
        printf("Failure in fi_av_open\n");
        goto exit;
    }

    /* Create the scalable endpoint */
    prov->ep_attr->tx_ctx_cnt = num_threads;
    prov->ep_attr->rx_ctx_cnt = num_threads;

    ret = fi_scalable_ep(domain, prov, &sep, NULL);
    if (ret) {
        printf("Failure in fi_scalable_ep\n");
        goto exit;
    }

    ret = fi_scalable_ep_bind(sep, &av->fid, 0);
    if (ret) {
        printf("Failure in fi_scalable_ep_bind\n");
        goto exit;
    }

    /* Create a TX context, RX context, and a Completion Queue for each thread */
    posix_memalign((void**) &thread_ctx, PAGE_SIZE, num_threads * sizeof(thread_context_t));
    for (thread_i = 0; thread_i < num_threads; thread_i++) {
        /* Create completion queue */
        memset(&cq_attr, 0, sizeof(cq_attr));
        cq_attr.format = FI_CQ_FORMAT_TAGGED;
        ret = fi_cq_open(domain, &cq_attr, &thread_ctx[thread_i].cq, NULL);
        if (ret) {
            printf("Failure in fi_cq_open\n");
            goto exit;
        }

        /* Create the transmit context */
        tx_attr = *(prov->tx_attr);
        tx_attr.op_flags = FI_COMPLETION;
        tx_attr.op_flags |= FI_DELIVERY_COMPLETE;
        tx_attr.caps = FI_TAGGED;
        tx_attr.caps |= FI_RMA;
        tx_attr.caps |= FI_ATOMICS;
        tx_attr.caps |= FI_MSG;

        ret = fi_tx_context(sep, thread_i, &tx_attr, &thread_ctx[thread_i].tx_ctx, NULL);
        if (ret) {
            printf("Failure in fi_tx_context\n");
            goto exit;
        }

        ret =
            fi_ep_bind(thread_ctx[thread_i].tx_ctx, &thread_ctx[thread_i].cq->fid,
                       FI_SEND | FI_SELECTIVE_COMPLETION);
        if (ret) {
            printf("Failure in fi_ep_bind\n");
            goto exit;
        }

        ret = fi_enable(thread_ctx[thread_i].tx_ctx);
        if (ret) {
            printf("Failure in fi_enable for transmit context\n");
            goto exit;
        }

        /* Create the receive context */
        rx_attr = *(prov->rx_attr);
        rx_attr.caps = FI_TAGGED;
        rx_attr.caps |= FI_DIRECTED_RECV;
        rx_attr.caps |= FI_RMA | FI_REMOTE_READ | FI_REMOTE_WRITE;
        rx_attr.caps |= FI_ATOMICS;
        rx_attr.caps |= FI_MSG;
        rx_attr.caps |= FI_MULTI_RECV;

        ret = fi_rx_context(sep, thread_i, &rx_attr, &thread_ctx[thread_i].rx_ctx, NULL);
        if (ret) {
            printf("Failure in fi_rx_context\n");
            goto exit;
        }

        ret = fi_ep_bind(thread_ctx[thread_i].rx_ctx, &thread_ctx[thread_i].cq->fid, FI_RECV);
        if (ret) {
            printf("Failure in fi_ep_bind\n");
            goto exit;
        }

        ret = fi_enable(thread_ctx[thread_i].rx_ctx);
        if (ret) {
            printf("Failure in fi_enable for receive context\n");
            goto exit;
        }
    }

    /* Enable the scalable endpoint */
    ret = fi_enable(sep);
    if (ret) {
        printf("Failure in fi_enable for scalable endpoint\n");
        goto exit;
    }

    /* Exchange information to connect endpoints */
    addrlen = FI_NAME_MAX;
    ret = fi_getname((fid_t) sep, local_addr, &addrlen);
    if (ret) {
        printf("Failure in getting address of scalable endpoint\n");
        goto exit;
    }

    if (rank) {
        /* Receiver */
        int target = rank - 1;
        MPI_Recv(remote_addr, addrlen, MPI_CHAR, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Send(local_addr, addrlen, MPI_CHAR, target, 0, MPI_COMM_WORLD);
    } else {
        /* Sender */
        int target = rank + 1;
        MPI_Send(local_addr, addrlen, MPI_CHAR, target, 0, MPI_COMM_WORLD);
        MPI_Recv(remote_addr, addrlen, MPI_CHAR, target, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    addr_count = 1;     // Each process only exchanges with one other process
    ret = fi_av_insert(av, remote_addr, addr_count, &remote_fi_addr, 0, 0);
    if (ret < addr_count) {
        printf("Failure in fi_av_insert\n");
        goto exit;
    }
    ret = 0;

    for (thread_i = 0; thread_i < num_threads; thread_i++) {
        thread_ctx[thread_i].other_side_ctx_addr =
            fi_rx_addr(remote_fi_addr, thread_i, DEF_RX_CTX_BITS);
    }

    /* Free temporary resources */
    hints->fabric_attr->prov_name = NULL;
    fi_freeinfo(hints);
    
    if (rank == 0) {
        ts_elapsed = calloc(num_threads, sizeof *ts_elapsed);
    }
#pragma omp parallel
    {
        struct fid_ep *tx_ctx, *rx_ctx;
        struct fid_cq *cq;
        fi_addr_t other_side_ctx_addr;
        struct fi_cq_tagged_entry wc[DEF_NUM_CQ_ENTRIES];
        struct fi_context context;
        uint64_t ignore_mask = 0x0000000000000000ULL;
        int tid, my_ret, my_message_size, my_rank;
        int posted_messages, completed_messages, completions;
        int tag;
        int sync_buf;
        void *buffer;

        /* Initialize buffers and variables for the critical path */
        tid = omp_get_thread_num();
        my_message_size = message_size;
        my_rank = rank;
        tx_ctx = thread_ctx[tid].tx_ctx;
        rx_ctx = thread_ctx[tid].rx_ctx;
        cq = thread_ctx[tid].cq;
        other_side_ctx_addr = thread_ctx[tid].other_side_ctx_addr;
        posted_messages = completed_messages = completions = 0;
        posix_memalign(&buffer, PAGE_SIZE, my_message_size);
        tag = tid;

        if (rank) {
            /* Receiver */
            int i;

            *((double *) buffer) = 0;

            /* Pre-post outstanding receives */
            for (i = 0; i < DEF_OUTSTANDING_RECVS; i++) {
                do {
                    my_ret =
                        fi_trecv(rx_ctx, buffer, my_message_size, NULL, other_side_ctx_addr, tag,
                                 ignore_mask, &context);
                    if (likely(my_ret == 0))
                        break;
#ifdef ERROR_CHECK
                    if (my_ret && my_ret != -FI_EAGAIN) {
                        printf("Failure in fi_trecv\n");
                        exit(my_ret);
                    }
#endif
                } while (my_ret == -FI_EAGAIN);
            }
#pragma omp single
            {
                MPI_Barrier(MPI_COMM_WORLD);
            }

            /* Critical path START */
            while (completed_messages < num_messages) {
                /* Replace the completed receives */
                for (i = 0; i < completions; i++) {
                    do {
                        my_ret =
                            fi_trecv(rx_ctx, buffer, my_message_size, NULL, other_side_ctx_addr, tag,
                                     ignore_mask, &context);
                        if (likely(my_ret == 0))
                            break;
#ifdef ERROR_CHECK
                        if (my_ret != -FI_EAGAIN) {
                            printf("Failure in fi_trecv\n");
                            exit(my_ret);
                        }
#endif
                    } while (my_ret == -FI_EAGAIN);
                }
                /* Poll for completion of operations */
                do {
                    completions = fi_cq_read(cq, (void *) wc, DEF_NUM_CQ_ENTRIES);
                } while (completions == -FI_EAGAIN || completions == 0);
#ifdef ERROR_CHECK
                if (completions < 0) {
                    printf("Failure in fi_cq_read\n");
                    my_ret = completions;
                    exit(my_ret);
                }
#endif
                completed_messages += completions;
            }
            /* Sync */
            do {
                my_ret = fi_tinject(tx_ctx, buffer, sizeof(int), other_side_ctx_addr, tag);
                if (likely(my_ret == 0))
                    break;
#ifdef ERROR_CHECK
                if (my_ret != -FI_EAGAIN) {
                    printf("Failure in fi_inject during sync\n");
                    exit(my_ret);
                }
#endif
            } while (my_ret == -FI_EAGAIN);
            /* Critical path END */
        
        } else {
            /* Sender */
            double t_start, t_elapsed;

            *((double *) buffer) = 10;

            /* Initiate sync before critical path */
            do {
                my_ret =
                    fi_trecv(rx_ctx, &sync_buf, sizeof(int), NULL, other_side_ctx_addr, tag,
                             ignore_mask, &context);
                if (likely(my_ret == 0))
                    break;
#ifdef ERROR_CHECK
                if (my_ret != -FI_EAGAIN) {
                    printf("Failure in fi_trecv for sync\n");
                    exit(my_ret);
                }
#endif
            } while (my_ret == -FI_EAGAIN);
#pragma omp single
            {
                MPI_Barrier(MPI_COMM_WORLD);
            }

            /* Critical path START */
            t_start = MPI_Wtime();
            while (posted_messages < num_messages) {
                do {
                    my_ret =
                        fi_tinjectdata(tx_ctx, buffer, my_message_size, my_rank, other_side_ctx_addr,
                                       tag);
                    if (likely(my_ret == 0))
                        break;
#ifdef ERROR_CHECK
                    if (my_ret != -FI_EAGAIN) {
                        printf("Failure in fi_tinjectdata\n");
                        exit(my_ret);
                    }
#endif
                } while (my_ret == -FI_EAGAIN);
                posted_messages++;
            }
            /* Wait for sync to complete */
            do {
                completions = fi_cq_read(cq, (void *) wc, 1);
            } while (completions == -FI_EAGAIN || completions == 0);
#ifdef ERROR_CHECK
            if (completions < 0) {
                printf("Failure in fi_cq_read\n");
                my_ret = completions;
                exit(my_ret);
            }
#endif
            /* Critical path END */
            t_elapsed = MPI_Wtime() - t_start;
            ts_elapsed[tid] = t_elapsed;
        }

        free(buffer);
    }

    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        double thread_msg_rate, msg_rate;

        printf("%-10s\t%-10s\n", "Thread", "Mmsgs/s");
        msg_rate = 0;
        for (thread_i = 0; thread_i < num_threads; thread_i++) {
            thread_msg_rate = num_messages / ts_elapsed[thread_i] / 1e6;
            printf("%-10d\t%-10.2f\n", thread_i, thread_msg_rate);
            msg_rate += thread_msg_rate;
        }
        printf("\n%-10s\t%-10s\t%-10s\n", "Size", "Threads", "Mmsgs/s");
        printf("%-10d\t", message_size);
        printf("%-10d\t", num_threads);
        printf("%-10.2f\n", msg_rate);

        free(ts_elapsed);
    }

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

    struct option long_options[] = {
        {.name = "size",.has_arg = 1,.val = 's'},
        {.name = "messages",.has_arg = 1,.val = 'm'},
        {.name = "threads",.has_arg = 1,.val = 't'},
        {0, 0, 0, 0}
    };

    while (1) {
        op = getopt_long(argc, argv, "h?s:w:m:t:", long_options, NULL);
        if (op == -1)
            break;

        switch (op) {
            case '?':
            case 'h':
                printf("Please see source file.\n");
                return -1;
            case 'm':
                num_messages = atoi(optarg);
                break;
            case 's':
                message_size = atoi(optarg);
                break;
            case 't':
                num_threads = atoi(optarg);
                break;
            default:
                printf("Invalid option\n");
                break;
        }
    }

    if (optind < argc) {
        printf("Please see source file.\n");
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
