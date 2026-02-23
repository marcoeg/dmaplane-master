// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 Graziano Labs Corp.
 *
 * kvcache_receiver.c — Chunked WRITEIMM KVCache receiver
 *
 * Loopback mode: minimal self-test — creates one MR, posts a few recvs,
 * and directs the user to kvcache_sender for the full loopback test.
 *
 * Peer mode (--listen): stubbed for future EC2 two-machine deployment.
 * Structure is written now; only TCP setup needs un-stubbing.
 *
 * Build:  make -C examples/kvcache
 * Run:    sudo ./kvcache_receiver [--loopback] [--listen 0.0.0.0:9876]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>

#include "kvcache_common.h"

/* ──────────────────────────────────────────────────────────────────────────
 *  Configuration
 * ────────────────────────────────────────────────────────────────────────── */

struct config {
	int      loopback;        /* 1=loopback (default), 0=peer */
	int      use_gpu;
	int      verify;
	uint64_t buf_size;
	int      credit_window;
	char     listen_addr[128];
	char    *ib_dev;
};

static void config_defaults(struct config *c)
{
	memset(c, 0, sizeof(*c));
	c->loopback = 1;
	c->buf_size = 256ULL * 1024 * 1024;
	c->credit_window = 16;
	snprintf(c->listen_addr, sizeof(c->listen_addr), "0.0.0.0:9876");
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"  --loopback            Minimal loopback self-test (default)\n"
		"  --listen <ip:port>    Listen for remote kvcache_sender\n"
		"  --gpu                 Use GPU-backed receive MR (requires CUDA)\n"
		"  --ib-dev <name>       IB device name (auto-detect if omitted)\n"
		"  --buf-size N          Receive buffer size (default: 256MB)\n"
		"  --credit-window N     Max in-flight WRITEIMMs (default: 16)\n"
		"  --verify              Verify data integrity\n",
		prog);
}

static int parse_args(int argc, char *argv[], struct config *c)
{
	static struct option opts[] = {
		{"loopback",       no_argument,       NULL, 'l'},
		{"listen",         required_argument, NULL, 'L'},
		{"gpu",            no_argument,       NULL, 'g'},
		{"ib-dev",         required_argument, NULL, 'd'},
		{"buf-size",       required_argument, NULL, 'b'},
		{"credit-window",  required_argument, NULL, 'w'},
		{"verify",         no_argument,       NULL, 'v'},
		{"help",           no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "lL:gd:b:w:vh",
				  opts, NULL)) != -1) {
		switch (opt) {
		case 'l': c->loopback = 1; break;
		case 'L':
			c->loopback = 0;
			snprintf(c->listen_addr, sizeof(c->listen_addr),
				 "%s", optarg);
			break;
		case 'g': c->use_gpu = 1; break;
		case 'd': c->ib_dev = optarg; break;
		case 'b': c->buf_size = (uint64_t)atoll(optarg); break;
		case 'w': c->credit_window = atoi(optarg); break;
		case 'v': c->verify = 1; break;
		case 'h': usage(argv[0]); return -1;
		default:  usage(argv[0]); return -1;
		}
	}
	return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Peer mode (stubbed)
 * ────────────────────────────────────────────────────────────────────────── */

/*
 * run_peer — two-machine receiver mode (TCP connection stubbed).
 *
 * Creates all RDMA resources (buffer, MR with REMOTE_WRITE) that a remote
 * sender would target.  In the un-stubbed version:
 *   1. TCP listen → accept → exchange QP metadata via IOCTL_RDMA_INIT_PEER
 *   2. Exchange MR metadata (addr, rkey, size) over TCP
 *   3. IOCTL_RDMA_CONNECT_PEER with the sender's peer info
 *   4. Recv loop using shared helpers from kvcache_common.h
 *
 * The recv loop body is identical to the sender's loopback recv-poll loop —
 * both use recv_loop_poll_and_track() + recv_loop_replenish().  Only the
 * TCP setup code needs writing when peer mode is un-stubbed for EC2.
 */
static int run_peer(int fd, struct config *cfg)
{
	const char *role = "RECV";
	uint64_t start_ns = now_ns();
	int ret = 0;

	uint32_t buf_id = 0;
	int buf_ok = 0, rdma_up = 0, mr_ok = 0;
	struct dmaplane_mr_params mr = {0};
	struct latency_stats ls;
	struct credit_tracker ct;
	/* In real peer mode, layer/chunk counts come from the sender's
	 * manifest exchanged over TCP.  Defaults here for struct sizing. */
	int num_layers = 32, chunks_per_layer = 4;
	struct layer_bitmap bm;

	if (latency_stats_init(&ls, num_layers * chunks_per_layer + 16) < 0) {
		fprintf(stderr, "  Failed to allocate latency stats\n");
		return 1;
	}
	credit_init(&ct, cfg->credit_window);
	bitmap_init(&bm, num_layers, chunks_per_layer);

	print_trace(role, start_ns,
		    "Peer mode: buf_size=%llu MB, credit_window=%d",
		    (unsigned long long)(cfg->buf_size >> 20),
		    cfg->credit_window);

	/* [1] Setup RDMA */
	if (dmaplane_setup_rdma(fd, cfg->ib_dev, 256,
				cfg->credit_window + 16,
				cfg->credit_window + 16) < 0) {
		perror("  SETUP_RDMA");
		ret = 1; goto out;
	}
	rdma_up = 1;

	/* [2] Allocate receive buffer */
	if (dmaplane_create_buffer(fd, cfg->buf_size, DMAPLANE_NUMA_ANY,
				   &buf_id) < 0) {
		perror("  CREATE_BUFFER");
		ret = 1; goto out;
	}
	buf_ok = 1;

	/* [3] Register MR (LOCAL_WRITE | REMOTE_WRITE) */
	if (dmaplane_register_mr(fd, buf_id,
				 DMAPLANE_IB_ACCESS_LOCAL_WRITE |
				 DMAPLANE_IB_ACCESS_REMOTE_WRITE,
				 &mr) < 0) {
		perror("  REGISTER_MR");
		ret = 1; goto out;
	}
	mr_ok = 1;

	print_trace(role, start_ns,
		    "MR id=%u rkey=0x%x addr=0x%llx",
		    mr.mr_id, mr.rkey, (unsigned long long)mr.addr);

	/* TODO: TCP listen + accept on cfg->listen_addr */
	/* TODO: Exchange QP info via IOCTL_RDMA_INIT_PEER + TCP */
	/* TODO: Exchange MR metadata (addr, rkey, size) over TCP */
	/* TODO: IOCTL_RDMA_CONNECT_PEER with sender's peer info */
	fprintf(stderr,
		"\n"
		"  ┌────────────────────────────────────────────┐\n"
		"  │  Peer mode TCP connection: NOT YET IMPLEMENTED │\n"
		"  │  Use --loopback for now, or kvcache_sender     │\n"
		"  │  --loopback for the full self-contained test.  │\n"
		"  └────────────────────────────────────────────┘\n"
		"\n");
	ret = 1;

	/*
	 * When TCP is un-stubbed, the recv loop will be:
	 *
	 * Pre-post initial recvs:
	 *   for (i = 0; i < initial_recvs; i++)
	 *       dmaplane_post_recv(fd, mr.mr_id, chunk_size, 1);
	 *
	 * Recv loop:
	 *   while (!bitmap_all_complete(&bm)) {
	 *       uint64_t cs = now_ns();
	 *       int rc = recv_loop_poll_and_track(fd, 1, 10000, &bm, &ls, &cs);
	 *       if (rc < 0) break;
	 *       if (rc == 1) break;   // SENTINEL
	 *       recv_loop_replenish(fd, mr.mr_id, chunk_size, 1, &ct);
	 *   }
	 *
	 * Print summary:
	 *   print_trace(role, start_ns, "Received %d/%d chunks",
	 *               bitmap_total_received(&bm), total_chunks);
	 *   latency_stats_print(&ls, role, start_ns);
	 */

out:
	dmaplane_drain_recv_cq(fd, 0);
	dmaplane_drain_recv_cq(fd, 1);
	if (rdma_up)
		dmaplane_teardown_rdma(fd);
	if (mr_ok)
		dmaplane_deregister_mr(fd, mr.mr_id);
	if (buf_ok)
		dmaplane_destroy_buffer(fd, buf_id);
	latency_stats_free(&ls);
	return ret;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Loopback self-test (minimal)
 * ────────────────────────────────────────────────────────────────────────── */

/*
 * run_loopback — minimal self-test for the receiver path.
 *
 * Verifies that the receiver can: create a fast-reg MR (REMOTE_WRITE),
 * post recv WRs, and mmap the buffer for read/write.  Does NOT send
 * any data — use kvcache_sender --loopback for the full round-trip test.
 *
 * This exists so 'make test' can exercise the receiver binary independently.
 */
static int run_loopback(int fd, struct config *cfg)
{
	const char *role = "RECV";
	uint64_t start_ns = now_ns();
	int ret = 0;

	uint32_t buf_id = 0;
	int buf_ok = 0, rdma_up = 0, mr_ok = 0;
	struct dmaplane_mr_params mr = {0};
	uint64_t test_size = 4ULL * 1024 * 1024; /* 4 MB test buffer */

	print_trace(role, start_ns,
		    "Loopback self-test (minimal): creating MR + posting recvs");

	/* [1] Setup RDMA */
	if (dmaplane_setup_rdma(fd, cfg->ib_dev, 64, 32, 32) < 0) {
		perror("  SETUP_RDMA");
		ret = 1; goto out;
	}
	rdma_up = 1;

	/* [2] Allocate + register MR */
	if (dmaplane_create_buffer(fd, test_size, DMAPLANE_NUMA_ANY,
				   &buf_id) < 0) {
		perror("  CREATE_BUFFER");
		ret = 1; goto out;
	}
	buf_ok = 1;

	if (dmaplane_register_mr(fd, buf_id,
				 DMAPLANE_IB_ACCESS_LOCAL_WRITE |
				 DMAPLANE_IB_ACCESS_REMOTE_WRITE,
				 &mr) < 0) {
		perror("  REGISTER_MR");
		ret = 1; goto out;
	}
	mr_ok = 1;

	print_trace(role, start_ns,
		    "MR id=%u rkey=0x%x addr=0x%llx size=%llu KB",
		    mr.mr_id, mr.rkey, (unsigned long long)mr.addr,
		    (unsigned long long)(test_size >> 10));

	/* [3] Post a few test recvs to verify RDMA plumbing */
	int test_recvs = 4;

	for (int i = 0; i < test_recvs; i++) {
		if (dmaplane_post_recv(fd, mr.mr_id, 1024 * 1024, 0) < 0) {
			perror("  POST_RECV");
			ret = 1; goto out;
		}
	}
	print_trace(role, start_ns, "Posted %d test recvs — PASS", test_recvs);

	/* [4] Verify mmap works */
	{
		uint64_t msz;
		void *ptr = dmaplane_mmap_buffer(fd, buf_id,
						 PROT_READ | PROT_WRITE,
						 &msz);

		if (ptr == MAP_FAILED) {
			perror("  mmap");
			ret = 1; goto out;
		}
		/* Write a marker, read it back */
		memset(ptr, 0xAB, 4096);
		uint8_t *p = (uint8_t *)ptr;

		if (p[0] == 0xAB && p[4095] == 0xAB)
			print_trace(role, start_ns, "mmap read/write — PASS");
		else {
			print_trace(role, start_ns, "mmap read/write — FAIL");
			ret = 1;
		}
		munmap(ptr, msz);
	}

	print_trace(role, start_ns,
		    "Loopback self-test complete. For full KVCache test, run:");
	print_trace(role, start_ns,
		    "  sudo ./kvcache_sender --loopback --verify");

out:
	dmaplane_drain_recv_cq(fd, 0);
	if (rdma_up)
		dmaplane_teardown_rdma(fd);
	if (mr_ok)
		dmaplane_deregister_mr(fd, mr.mr_id);
	if (buf_ok)
		dmaplane_destroy_buffer(fd, buf_id);
	return ret;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Main
 * ────────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	struct config cfg;
	int fd, ret;

	config_defaults(&cfg);
	if (parse_args(argc, argv, &cfg) < 0)
		return 1;

	fd = open(DMAPLANE_DEVICE, O_RDWR);
	if (fd < 0) {
		perror("open " DMAPLANE_DEVICE);
		return 1;
	}

	if (cfg.loopback)
		ret = run_loopback(fd, &cfg);
	else
		ret = run_peer(fd, &cfg);

	close(fd);
	return ret;
}
