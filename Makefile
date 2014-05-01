
obj-m += ramdisk.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	insmod ramdisk.ko
	lsmod | grep ramdisk
	
	gcc -o test test.c ramdisk_ioctl.c

	rm *.o *.order *.symvers *.mod.c .ramdisk*
	rm -r .tmp_versions

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm test
	rmmod ramdisk
