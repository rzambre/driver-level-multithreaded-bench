CC = gcc
MPICC = mpicc
CFLAGS = -Wall -std=gnu99 -O3 #-g3
MACROS = #-DERRCHK
IFLAGS = -I/home/rzambre/base-rdma-core/build/include
LFLAGS = -L/home/rzambre/base-rdma-core/build/lib
LNAME = -libverbs
OMPFLAGS = -fopenmp
DEPS = shared.c

TARGETS = mpi_omp_rc_multiQP_resProcDriveTh_resSharing_write_bench

mpi_omp_rc_multiQP_resProcDriveTh_resSharing_write_bench: mpi_omp_rc_multiQP_resProcDriveTh_resSharing_write_bench.c $(DEPS)
	$(MPICC) $(OMPFLAGS) $(CFLAGS) $(MACROS) $^ -o $@ $(IFLAGS) $(LFLAGS) $(LNAME)

clean:
	rm -f $(TARGETS)
