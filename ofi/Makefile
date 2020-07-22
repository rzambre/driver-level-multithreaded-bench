MPICC = mpicc
LIBFABRIC = /home/rzambre/libfabric/debug-psm2-install
CFLAGS = -Wall -O0 -g3
MACROS = -DERROR_CHECK
LFLAGS = -L$(LIBFABRIC)/lib
LNAME = -lfabric
IFLAGS = -I$(LIBFABRIC)/include
#OMPFLAGS = -fopenmp
OMPFLAGS = -qopenmp
DEPS = shared.h

TARGETS = mpi_omp_ofi_tagged_sr mpi_omp_rdm_multi_ep_bw_cntr_manual

mpi_omp_rdm_multi_ep_bw_cntr_manual: mpi_omp_rdm_multi_ep_bw_cntr_manual.c $(DEPS)
	$(MPICC) $(OMPFLAGS) $(CFLAGS) $^ -o $@ $(IFLAGS) $(LFLAGS) $(LNAME)

mpi_omp_ofi_tagged_sr: mpi_omp_ofi_tagged_sr.c
	$(MPICC) $(OMPFLAGS) $(CFLAGS) $(MACROS) $^ -o $@ $(IFLAGS) $(LFLAGS) $(LNAME)

clean:
	rm -f $(TARGETS)
