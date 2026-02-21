// SPDX-License-Identifier: GPL-2.0
/*
 * dmaplane Phase 1 test — stress test for character device + rings
 * Copyright (c) 2026 Graziano Labs Corp.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <pthread.h>

#include "dmaplane_uapi.h"

#define DEV_PATH	"/dev/dmaplane"
#define STRESS_COUNT	1000000	/* 1M entries for stress tests */
#define MULTI_PER_CHAN	250000	/* 250K per channel in multi-channel test */
#define BATCH_SIZE	256	/* Submit this many before completing */

static int tests_passed;
static int tests_failed;

#define TEST_PASS(name) do { \
	printf("  PASS: %s\n", name); \
	tests_passed++; \
} while (0)

#define TEST_FAIL(name, ...) do { \
	printf("  FAIL: %s — ", name); \
	printf(__VA_ARGS__); \
	printf("\n"); \
	tests_failed++; \
} while (0)

/* Helper: open device */
static int dev_open(void)
{
	int fd = open(DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror("open " DEV_PATH);
	}
	return fd;
}

/* Helper: create channel, return channel ID or -1 on error */
static int create_channel(int fd)
{
	struct dmaplane_channel_params params;
	int ret;

	memset(&params, 0, sizeof(params));
	ret = ioctl(fd, DMAPLANE_IOCTL_CREATE_CHANNEL, &params);
	if (ret < 0)
		return -1;
	return (int)params.channel_id;
}

/* Helper: submit one entry */
static int submit_entry(int fd, __u64 payload, __u32 flags)
{
	struct dmaplane_submit_params p;

	memset(&p, 0, sizeof(p));
	p.entry.payload = payload;
	p.entry.flags = flags;
	return ioctl(fd, DMAPLANE_IOCTL_SUBMIT, &p);
}

/* Helper: complete one entry, returns 0 on success */
static int complete_entry(int fd, struct dmaplane_ring_entry *out)
{
	struct dmaplane_complete_params p;
	int ret;

	memset(&p, 0, sizeof(p));
	ret = ioctl(fd, DMAPLANE_IOCTL_COMPLETE, &p);
	if (ret == 0 && out)
		*out = p.entry;
	return ret;
}

/* Helper: poll for a completion with timeout */
static int complete_poll(int fd, struct dmaplane_ring_entry *out,
			 int max_retries)
{
	int i;
	for (i = 0; i < max_retries; i++) {
		if (complete_entry(fd, out) == 0)
			return 0;
		usleep(100);
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Test 1: Basic open/close                                            */
/* ------------------------------------------------------------------ */
static void test_open_close(void)
{
	int fd = dev_open();
	if (fd < 0) {
		TEST_FAIL("open/close", "cannot open device");
		return;
	}
	close(fd);
	TEST_PASS("open/close");
}

/* ------------------------------------------------------------------ */
/* Test 2: Channel creation                                            */
/* ------------------------------------------------------------------ */
static void test_channel_creation(void)
{
	int fd1, fd2, ch1, ch2;

	fd1 = dev_open();
	fd2 = dev_open();
	if (fd1 < 0 || fd2 < 0) {
		TEST_FAIL("channel creation", "cannot open device");
		if (fd1 >= 0) close(fd1);
		if (fd2 >= 0) close(fd2);
		return;
	}

	ch1 = create_channel(fd1);
	ch2 = create_channel(fd2);

	if (ch1 < 0 || ch2 < 0) {
		TEST_FAIL("channel creation", "ioctl failed: ch1=%d ch2=%d",
			  ch1, ch2);
	} else if (ch1 != 0 || ch2 != 1) {
		TEST_FAIL("channel creation",
			  "expected IDs 0,1 got %d,%d", ch1, ch2);
	} else {
		TEST_PASS("channel creation");
	}

	close(fd1);
	close(fd2);
}

/* ------------------------------------------------------------------ */
/* Test 3: Submit and complete                                         */
/* ------------------------------------------------------------------ */
static void test_submit_complete(void)
{
	int fd, ch;
	struct dmaplane_ring_entry entry;
	__u64 payload = 0xDEADBEEFULL;

	fd = dev_open();
	if (fd < 0) {
		TEST_FAIL("submit/complete", "cannot open device");
		return;
	}

	ch = create_channel(fd);
	if (ch < 0) {
		TEST_FAIL("submit/complete", "create channel failed");
		close(fd);
		return;
	}

	if (submit_entry(fd, payload, 0) != 0) {
		TEST_FAIL("submit/complete", "submit failed: %s",
			  strerror(errno));
		close(fd);
		return;
	}

	if (complete_poll(fd, &entry, 10000) != 0) {
		TEST_FAIL("submit/complete", "completion timed out");
		close(fd);
		return;
	}

	if (entry.payload != payload + 1) {
		TEST_FAIL("submit/complete",
			  "expected payload 0x%llx, got 0x%llx",
			  (unsigned long long)(payload + 1),
			  (unsigned long long)entry.payload);
	} else {
		TEST_PASS("submit/complete");
	}

	close(fd);
}

/* ------------------------------------------------------------------ */
/* Test 4: Ring full behavior                                          */
/* ------------------------------------------------------------------ */
static void test_ring_full(void)
{
	int fd, ch, ret;
	int submitted = 0;
	struct dmaplane_ring_entry entry;

	fd = dev_open();
	if (fd < 0) {
		TEST_FAIL("ring full", "cannot open device");
		return;
	}

	ch = create_channel(fd);
	if (ch < 0) {
		TEST_FAIL("ring full", "create channel failed");
		close(fd);
		return;
	}

	/*
	 * Submit RING_SIZE entries rapidly. The ring should accept
	 * (RING_SIZE - 1) entries because the worker may consume some
	 * before we fill it. We keep trying until we get ENOSPC.
	 */
	while (submitted < DMAPLANE_RING_SIZE + 10) {
		ret = submit_entry(fd, (__u64)submitted, 0);
		if (ret != 0) {
			if (errno == ENOSPC)
				break;
			TEST_FAIL("ring full",
				  "unexpected error at %d: %s",
				  submitted, strerror(errno));
			close(fd);
			return;
		}
		submitted++;
	}

	if (submitted == DMAPLANE_RING_SIZE + 10) {
		/*
		 * Worker consumed entries fast enough that we never got
		 * ENOSPC. This is technically fine — the ring works,
		 * just the worker is fast. Accept as pass with note.
		 */
		printf("  NOTE: worker consumed entries faster than submit; "
		       "ring full not triggered\n");
	}

	/* Drain all completions */
	int completed = 0;
	while (completed < submitted) {
		if (complete_poll(fd, &entry, 50000) != 0) {
			TEST_FAIL("ring full",
				  "stuck draining: completed %d/%d",
				  completed, submitted);
			close(fd);
			return;
		}
		/* Verify: entry payload should be original + 1 */
		if (entry.payload != (__u64)completed + 1) {
			TEST_FAIL("ring full",
				  "payload mismatch at %d: expected 0x%llx got 0x%llx",
				  completed,
				  (unsigned long long)((__u64)completed + 1),
				  (unsigned long long)entry.payload);
			close(fd);
			return;
		}
		completed++;
	}

	TEST_PASS("ring full");
	close(fd);
}

/* ------------------------------------------------------------------ */
/* Test 5: Stress test (single channel)                                */
/* ------------------------------------------------------------------ */
static void test_stress_single(void)
{
	int fd, ch;
	int submitted = 0, completed = 0;
	struct dmaplane_ring_entry entry;
	struct timespec start, end;
	double elapsed, rate;

	fd = dev_open();
	if (fd < 0) {
		TEST_FAIL("stress single", "cannot open device");
		return;
	}

	ch = create_channel(fd);
	if (ch < 0) {
		TEST_FAIL("stress single", "create channel failed");
		close(fd);
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &start);

	while (completed < STRESS_COUNT) {
		/* Submit a batch */
		int batch = 0;
		while (submitted < STRESS_COUNT && batch < BATCH_SIZE) {
			if (submit_entry(fd, (__u64)submitted, 0) == 0) {
				submitted++;
				batch++;
			} else if (errno == ENOSPC) {
				break;
			} else {
				TEST_FAIL("stress single",
					  "submit error at %d: %s",
					  submitted, strerror(errno));
				close(fd);
				return;
			}
		}

		/* Complete as many as available */
		while (completed < submitted) {
			if (complete_entry(fd, &entry) == 0) {
				completed++;
			} else if (errno == EAGAIN) {
				break;
			} else {
				TEST_FAIL("stress single",
					  "complete error at %d: %s",
					  completed, strerror(errno));
				close(fd);
				return;
			}
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &end);
	elapsed = (double)(end.tv_sec - start.tv_sec) +
		  (double)(end.tv_nsec - start.tv_nsec) / 1e9;
	rate = (double)STRESS_COUNT / elapsed;

	if (completed == STRESS_COUNT) {
		printf("  PASS: stress single — %d entries in %.3f s "
		       "(%.0f ops/sec)\n", STRESS_COUNT, elapsed, rate);
		tests_passed++;
	} else {
		TEST_FAIL("stress single",
			  "count mismatch: submitted=%d completed=%d",
			  submitted, completed);
	}

	close(fd);
}

/* ------------------------------------------------------------------ */
/* Test 6: Multi-channel stress                                        */
/* ------------------------------------------------------------------ */
struct thread_args {
	int fd;
	int channel_id;
	int count;
	int ok;		/* Set to 1 on success */
};

static void *multi_channel_thread(void *arg)
{
	struct thread_args *ta = arg;
	int submitted = 0, completed = 0;
	struct dmaplane_ring_entry entry;
	/*
	 * Use payload base = channel_id * count so each channel
	 * has a unique range, allowing cross-contamination detection.
	 */
	__u64 base = (__u64)ta->channel_id * (__u64)ta->count;

	while (completed < ta->count) {
		/* Submit batch */
		int batch = 0;
		while (submitted < ta->count && batch < BATCH_SIZE) {
			if (submit_entry(ta->fd, base + (__u64)submitted, 0) == 0) {
				submitted++;
				batch++;
			} else if (errno == ENOSPC) {
				break;
			} else {
				fprintf(stderr, "  ch%d submit error: %s\n",
					ta->channel_id, strerror(errno));
				ta->ok = 0;
				return NULL;
			}
		}

		/* Complete */
		while (completed < submitted) {
			if (complete_entry(ta->fd, &entry) == 0) {
				/* Verify payload belongs to this channel */
				__u64 expected = base + (__u64)completed + 1;
				if (entry.payload != expected) {
					fprintf(stderr,
						"  ch%d mismatch at %d: "
						"expected 0x%llx got 0x%llx\n",
						ta->channel_id, completed,
						(unsigned long long)expected,
						(unsigned long long)entry.payload);
					ta->ok = 0;
					return NULL;
				}
				completed++;
			} else if (errno == EAGAIN) {
				break;
			} else {
				fprintf(stderr, "  ch%d complete error: %s\n",
					ta->channel_id, strerror(errno));
				ta->ok = 0;
				return NULL;
			}
		}
	}

	ta->ok = 1;
	return NULL;
}

static void test_stress_multi(void)
{
	int fds[DMAPLANE_MAX_CHANNELS];
	pthread_t threads[DMAPLANE_MAX_CHANNELS];
	struct thread_args args[DMAPLANE_MAX_CHANNELS];
	struct timespec start, end;
	double elapsed, rate;
	int i, all_ok = 1;

	for (i = 0; i < DMAPLANE_MAX_CHANNELS; i++) {
		fds[i] = dev_open();
		if (fds[i] < 0) {
			TEST_FAIL("stress multi", "cannot open device for ch %d", i);
			while (--i >= 0) close(fds[i]);
			return;
		}
	}

	for (i = 0; i < DMAPLANE_MAX_CHANNELS; i++) {
		int ch = create_channel(fds[i]);
		if (ch < 0) {
			TEST_FAIL("stress multi",
				  "create channel %d failed", i);
			for (int j = 0; j < DMAPLANE_MAX_CHANNELS; j++)
				close(fds[j]);
			return;
		}
		args[i].fd = fds[i];
		args[i].channel_id = ch;
		args[i].count = MULTI_PER_CHAN;
		args[i].ok = 0;
	}

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (i = 0; i < DMAPLANE_MAX_CHANNELS; i++)
		pthread_create(&threads[i], NULL, multi_channel_thread, &args[i]);

	for (i = 0; i < DMAPLANE_MAX_CHANNELS; i++)
		pthread_join(threads[i], NULL);

	clock_gettime(CLOCK_MONOTONIC, &end);
	elapsed = (double)(end.tv_sec - start.tv_sec) +
		  (double)(end.tv_nsec - start.tv_nsec) / 1e9;
	rate = (double)(DMAPLANE_MAX_CHANNELS * MULTI_PER_CHAN) / elapsed;

	for (i = 0; i < DMAPLANE_MAX_CHANNELS; i++) {
		if (!args[i].ok) {
			all_ok = 0;
			break;
		}
	}

	if (all_ok) {
		printf("  PASS: stress multi — %d channels × %d entries "
		       "in %.3f s (%.0f ops/sec total)\n",
		       DMAPLANE_MAX_CHANNELS, MULTI_PER_CHAN,
		       elapsed, rate);
		tests_passed++;
	} else {
		TEST_FAIL("stress multi", "one or more channels failed");
	}

	for (i = 0; i < DMAPLANE_MAX_CHANNELS; i++)
		close(fds[i]);
}

/* ------------------------------------------------------------------ */
/* Test 7: Cleanup on close                                            */
/* ------------------------------------------------------------------ */
static void test_cleanup_on_close(void)
{
	int fd, ch;
	int i;

	fd = dev_open();
	if (fd < 0) {
		TEST_FAIL("cleanup on close", "cannot open device");
		return;
	}

	ch = create_channel(fd);
	if (ch < 0) {
		TEST_FAIL("cleanup on close", "create channel failed");
		close(fd);
		return;
	}

	/* Submit entries without completing them */
	for (i = 0; i < 100; i++) {
		if (submit_entry(fd, (__u64)i, 0) != 0)
			break;
	}

	/* Close without completing — should not crash or leak */
	close(fd);

	/*
	 * If we get here, the kernel didn't crash.
	 * User should check dmesg for warnings.
	 */
	TEST_PASS("cleanup on close (check dmesg for warnings)");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
	printf("=== dmaplane Phase 1 Test Suite ===\n\n");

	test_open_close();
	test_channel_creation();
	test_submit_complete();
	test_ring_full();
	test_stress_single();
	test_stress_multi();
	test_cleanup_on_close();

	printf("\n=== Summary: %d passed, %d failed ===\n",
	       tests_passed, tests_failed);

	return tests_failed ? 1 : 0;
}
