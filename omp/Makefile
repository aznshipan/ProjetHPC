exact_cover : 
	gcc -O3 -fopenmp -o exact_cover exact_cover.c

exec : 
	./exact_cover --in  ../Instances-20210423/bell12.ec print-solutions

exact_cover2 : 
	gcc -O3 -fopenmp -o exact_cover2 exact_cover_omp2.c

clean :
	-rm exact_cover
	-rm exact_cover2