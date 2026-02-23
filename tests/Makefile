CC := gcc
CFLAGS := -Wall -Wextra -O2 -I../include

TESTS := test_phase1_driver test_phase2_dma test_phase3_dmabuf test_phase4_rdma test_phase5_numa test_phase6_backpressure test_phase7_instrumentation

all: $(TESTS)

test_phase1_driver: test_phase1_driver.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

test_phase2_dma: test_phase2_dma.c
	$(CC) $(CFLAGS) -o $@ $<

test_phase3_dmabuf: test_phase3_dmabuf.c
	$(CC) $(CFLAGS) -o $@ $<

test_phase4_rdma: test_phase4_rdma.c
	$(CC) $(CFLAGS) -o $@ $<

test_phase5_numa: test_phase5_numa.c
	$(CC) $(CFLAGS) -o $@ $<

test_phase6_backpressure: test_phase6_backpressure.c
	$(CC) $(CFLAGS) -o $@ $<

test_phase7_instrumentation: test_phase7_instrumentation.c
	$(CC) $(CFLAGS) -o $@ $<

# Phase 8: GPU test — conditional on CUDA availability.
# Skip with a warning if nvcc/cuda headers are not found.
CUDA_AVAILABLE := $(shell command -v nvcc >/dev/null 2>&1 && echo yes)
ifeq ($(CUDA_AVAILABLE),yes)
TESTS += test_phase8_gpu

test_phase8_gpu: test_phase8_gpu.c
	$(CC) $(CFLAGS) -o $@ $< -lcuda -lcudart -lm
else
test_phase8_gpu:
	@echo "SKIP: test_phase8_gpu — CUDA not available (nvcc not found)"
endif

clean:
	rm -f $(TESTS) test_phase8_gpu
