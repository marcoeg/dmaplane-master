# dmaplane Driver

Character device kernel module at `/dev/dmaplane`. Ioctl-driven, with per-channel submission/completion rings and worker threads.

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

## Concurrency Model

| Lock | Type | Protects | Acquired by |
|------|------|----------|-------------|
| `sub_ring.lock` | spinlock | Submission ring entries, head/tail | ioctl submit (producer), worker thread (consumer) |
| `comp_ring.lock` | spinlock | Completion ring entries, head/tail | Worker thread (producer), ioctl complete (consumer) |
| `dev_mutex` | mutex | Channel slot allocation array | ioctl create_channel, release |

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
