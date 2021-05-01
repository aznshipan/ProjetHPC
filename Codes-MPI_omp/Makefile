exact_cover : 
	mpicc -fopenmp -o exact_cover exact_cover.c

exec : 
	mpirun exact_cover --in  ../Instances-20210423/bell12.ec print-solutions

clean :
	rm exact_cover