CC = gcc
MPICC = mpicc
CFLAGS = -Wall -std=gnu99 -O3 -g3
MACROS =# -DERRCHK
UCSLNAME = -lucs
UCTLNAME = -luct
UCPLNAME = -lucp
OMPFLAGS = -fopenmp
DEPS = ucx_shared.c

TARGETS = mpi_omp_uct_put_short mpi_omp_ucp_tagged_sr

mpi_omp_uct_put_short: mpi_omp_uct_put_short.c $(DEPS)
	$(MPICC) $(OMPFLAGS) $(CFLAGS) $(MACROS) $^ -o $@ $(UCTLNAME) $(UCSLNAME)

mpi_omp_ucp_tagged_sr: mpi_omp_ucp_tagged_sr.c $(DEPS)
	$(MPICC) $(OMPFLAGS) $(CFLAGS) $(MACROS) $^ -o $@ $(UCPLNAME) $(UCSLNAME)

clean:
	rm -f $(TARGETS)
