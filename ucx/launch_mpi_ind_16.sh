export OMP_PLACES=cores
export OMP_PROC_BIND=close
export UCX_NET_DEVICES=mlx5_0:1
export UCX_TLS=rc
mpiexec -n 2 -ppn 1 -hosts gomez02,gomez03 -bind-to core:16 ./mpi_omp_ucp_tagged_sr -T 16 -C 1 -W 1
