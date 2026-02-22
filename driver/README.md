# dmaplane Driver

Character device kernel module at `/dev/dmaplane`. Ioctl-driven, with per-channel submission/completion rings, worker threads, and DMA buffer management with mmap support.

## Architecture

```
  Userspace                          Kernel
  ────────                          ──────
  open(/dev/dmaplane)  ──────────→  dmaplane_open()
                                      → allocate file_ctx
                                      → store in filp->private_data

  ioctl(CREATE_CHANNEL) ─────────→  dmaplane_ioctl_create_channel()
                                      → find free channel slot (mutex)
                                      → init submission + completion rings
                                      → create kthread "dmaplane/<id>"
                                      → return channel ID

  ioctl(SUBMIT, &entry) ─────────→  dmaplane_ioctl_submit()
                                      → copy_from_user
                                      → spinlock submission ring
                                      → write entry, advance head
                                      → wake worker thread

  [worker thread]                     dmaplane_worker_fn()
                                      → drain submission ring
                                      → process: payload += 1
                                      → push to completion ring

  ioctl(COMPLETE, &entry) ────────→  dmaplane_ioctl_complete()
                                      → spinlock completion ring
                                      → read entry, advance tail
                                      → copy_to_user

  ioctl(CREATE_BUFFER, &p) ─────→  dmaplane_ioctl_create_buffer()
                                      → dmabuf_rdma_create_buffer()
                                      → coherent: dma_alloc_coherent
                                      → pages: alloc_page loop + vmap
                                      → return buffer handle

  ioctl(GET_MMAP_INFO, &info) ──→  dmaplane_ioctl_get_mmap_info()
                                      → return offset + size for mmap

  mmap(fd, offset)  ─────────────→  dmaplane_mmap()
                                      → coherent: dma_mmap_coherent
                                      → pages: vm_insert_page loop
                                      → increment mmap_count

  ioctl(DESTROY_BUFFER, &id)  ──→  dmaplane_ioctl_destroy_buffer()
                                      → check mmap_count == 0
                                      → reverse-order cleanup
                                      → free pages / dma_free_coherent

  close(fd)  ─────────────────────→  dmaplane_release()
                                      → signal worker shutdown
                                      → kthread_stop (blocking)
                                      → free channel + file_ctx
```

## Ioctl Reference

| Command | Direction | Struct | Description |
|---------|-----------|--------|-------------|
| `DMAPLANE_IOCTL_CREATE_CHANNEL` | `_IOR` | `dmaplane_channel_params` | Allocate a channel and assign to this fd |
| `DMAPLANE_IOCTL_SUBMIT` | `_IOW` | `dmaplane_submit_params` | Push a work item onto the submission ring |
| `DMAPLANE_IOCTL_COMPLETE` | `_IOWR` | `dmaplane_complete_params` | Pop a completion from the completion ring |
| `DMAPLANE_IOCTL_GET_STATS` | `_IOR` | `dmaplane_stats` | Return channel statistics |
| `DMAPLANE_IOCTL_CREATE_BUFFER` | `_IOWR` | `dmaplane_buf_params` | Allocate a DMA buffer (coherent or page-backed) |
| `DMAPLANE_IOCTL_DESTROY_BUFFER` | `_IOW` | `__u32` | Destroy a buffer by handle |
| `DMAPLANE_IOCTL_GET_MMAP_INFO` | `_IOWR` | `dmaplane_mmap_info` | Get mmap offset and size for a buffer |
| `DMAPLANE_IOCTL_GET_BUF_STATS` | `_IOR` | `dmaplane_buf_stats` | Return buffer allocation statistics |
| `DMAPLANE_IOCTL_EXPORT_DMABUF` | `_IOWR` | `dmaplane_export_dmabuf_arg` | Export a page-backed buffer as a dma-buf fd |
| `DMAPLANE_IOCTL_GET_DMABUF_STATS` | `_IOR` | `dmaplane_dmabuf_stats` | Return dma-buf export statistics |
| `DMAPLANE_IOCTL_SETUP_RDMA` | `_IOWR` | `dmaplane_rdma_setup` | Initialize RDMA: IB device → PD → CQs → QPs |
| `DMAPLANE_IOCTL_TEARDOWN_RDMA` | `_IO` | — | Tear down RDMA subsystem |
| `DMAPLANE_IOCTL_REGISTER_MR` | `_IOWR` | `dmaplane_mr_params` | Register buffer pages as RDMA MR |
| `DMAPLANE_IOCTL_DEREGISTER_MR` | `_IOW` | `__u32` | Deregister an MR by ID |
| `DMAPLANE_IOCTL_LOOPBACK_TEST` | `_IOWR` | `dmaplane_loopback_params` | Single send/recv loopback test |
| `DMAPLANE_IOCTL_PINGPONG_BENCH` | `_IOWR` | `dmaplane_bench_params` | Ping-pong latency benchmark |
| `DMAPLANE_IOCTL_STREAMING_BENCH` | `_IOWR` | `dmaplane_bench_params` | Streaming throughput benchmark |
| `DMAPLANE_IOCTL_GET_RDMA_STATS` | `_IOR` | `dmaplane_rdma_stats` | Return RDMA statistics |

## Concurrency Model

| Lock | Type | Protects | Acquired by |
|------|------|----------|-------------|
| `sub_ring.lock` | spinlock | Submission ring entries, head/tail | ioctl submit (producer), worker thread (consumer) |
| `comp_ring.lock` | spinlock | Completion ring entries, head/tail | Worker thread (producer), ioctl complete (consumer) |
| `channel->lock` | mutex | Channel state (shutdown flag, worker pointer) | dmaplane_release |
| `dev_mutex` | mutex | Channel slot allocation array | ioctl create_channel, release |
| `buf_lock` | mutex | Buffer array (slots, lookup, mmap) | buffer ioctls, dmaplane_mmap |
| `rdma_sem` | rw_semaphore | RDMA context (PD, CQs, QPs) | read: MR/benchmark ops; write: setup/teardown |
| `mr_lock` | mutex | MR array (slots, lookup) | register/deregister MR, benchmarks |

**Lock ordering:** `dev_mutex` and `buf_lock` are independent (never nested). `rdma_sem` (read) → `buf_lock` → `mr_lock`. `dev_mutex` → `channel->lock` → ring spinlocks.

**Ring buffer design:** Monotonically increasing head/tail with modulo indexing. Full when `(head - tail) == RING_SIZE`, empty when `head == tail`. Head and tail on separate cache lines (`____cacheline_aligned_in_smp`) to avoid false sharing.

**Worker shutdown protocol:** Set `channel->shutdown = true`, wake the worker, then `kthread_stop()` (blocking wait for thread exit).

## Loading / Unloading

```bash
sudo insmod dmaplane.ko          # Load
ls -la /dev/dmaplane              # Verify device node
dmesg | grep dmaplane             # Check init message
sudo rmmod dmaplane               # Unload
dmesg | grep dmaplane             # Check exit message
```
