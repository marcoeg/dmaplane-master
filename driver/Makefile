obj-m := dmaplane.o
dmaplane-objs := main.o dmabuf_rdma.o dmabuf_export.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
