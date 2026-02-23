obj-m := dmaplane.o
dmaplane-objs := main.o dmabuf_rdma.o dmabuf_export.o rdma_engine.o benchmark.o \
                 numa_topology.o flow_control.o dmaplane_trace.o \
                 dmaplane_histogram.o dmaplane_debugfs.o

# -I$(src) is required for TRACE_INCLUDE_PATH = . to resolve
# dmaplane_trace.h during out-of-tree builds.
ccflags-y += -I$(src)

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
