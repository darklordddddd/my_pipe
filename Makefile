obj-m += pipe.o
KDIR := /home/heartbeat/Desktop/linux-4.9.10

all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
