export OMP_PLACES=cores
export OMP_PROC_BIND=close
export UCX_NET_DEVICES=mlx5_0:1
export UCX_TLS=rc
mpiexec -n 2 -ppn 1 -hosts gomez02,gomez03 -bind-to core:8 ./mpi_omp_ucp_tagged_sr -T 8 -C 8 -W 1
