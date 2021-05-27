exact_cover_omp_mpi :
	mpicc -O3 -fopenmp -o exact_cover_omp_mpi exact_cover_openMP+MPI_version3.c

A :
	mpicc -O3 -fopenmp -o exact_cover exact_cover.c

clean :
	-rm exact_cover_omp_mpi
	-rm exact_cover
