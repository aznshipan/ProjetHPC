echo "" > test.txt

echo " ./exact_cover --in  ../Instances-20210423/bell12.ec"
echo " ./exact_cover --in  ../Instances-20210423/bell12.ec" >> test.txt
./exact_cover --in  ../Instances-20210423/bell12.ec | grep "FINI" >> test.txt

echo " ./exact_cover --in  ../Instances-20210423/bell13.ec"
echo " ./exact_cover --in  ../Instances-20210423/bell13.ec" >> test.txt
./exact_cover --in  ../Instances-20210423/bell13.ec | grep "FINI" >> test.txt

echo " ./exact_cover --in  ../Instances-20210423/bell14.ec"
echo " ./exact_cover --in  ../Instances-20210423/bell14.ec" >> test.txt
./exact_cover --in  ../Instances-20210423/bell14.ec | grep "FINI" >> test.txt