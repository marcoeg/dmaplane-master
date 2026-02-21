# DMA Memory: Two Allocation Paths and the Zero-Copy Chain

*Part 2 of 9 in a series on building a host-side data path emulator for AI infrastructure*

---

Phase 1 gave us a character device, ioctl dispatch, ring buffers, and worker threads. The worker's "processing" was incrementing a payload by one — a placeholder. Phase 2 replaces that placeholder with the first real resource: DMA-mapped memory. Buffers that can be written from userspace and read by a NIC, with no copies in between.

The core additions are two new ioctls — `IOCTL_CREATE_BUFFER` and `IOCTL_DESTROY_BUFFER` — backed by two allocation paths and an mmap implementation that exposes the same physical pages to userspace. This post covers why two paths exist, how the DMA subsystem constrains the driver architecture, and what the mmap implementation must get right to be safe under concurrency.

---

## Why Two Allocation Paths

AI data movement has two fundamentally different memory needs.

Small, hot control structures — completion queue entries, doorbells, metadata headers — need cache-coherent memory where the CPU and device see the same data without explicit flush barriers. These are typically 4 KB to 64 KB. The DMA subsystem provides `dma_alloc_coherent` for this: it returns physically contiguous memory with a single DMA address and handles the cache coherency protocol (snooping on x86, explicit sync on ARM).

Large streaming buffers — gradient tensors, model weights, KV-cache slabs — are megabytes to gigabytes. They do not need coherency (the CPU fills them, then hands them to the NIC; no concurrent access). What they need is NUMA-aware placement (Phase 3), the ability to build scatter-gather lists for the NIC (Phase 4), and eventually dma-buf export for zero-copy device sharing (Phase 5). These are allocated as individual pages via `alloc_page` and stitched together with `vmap`.

The UAPI reflects this:

```c
#define DMAPLANE_BUF_TYPE_COHERENT  0
#define DMAPLANE_BUF_TYPE_PAGES     1
```

And the kernel-side buffer struct tracks both paths:

```c
struct dmaplane_buffer {
    unsigned int id;        /* Handle returned to userspace */
    int alloc_type;         /* BUF_TYPE_COHERENT or BUF_TYPE_PAGES */
    size_t size;            /* Requested size in bytes */
    bool in_use;            /* Slot is occupied. Protected by buf_lock. */
    void *vaddr;            /* Kernel VA — from dma_alloc_coherent or vmap */
    dma_addr_t dma_handle;  /* Coherent path only */
    struct page **pages;    /* Page-backed path only */
    unsigned int nr_pages;  /* Number of pages allocated */
    atomic_t mmap_count;    /* Prevents destroy while mmapped */
};
```

The coherent path produces a single contiguous allocation with both a kernel virtual address and a DMA address the device can use. The page-backed path produces an array of scattered physical pages with a vmapped kernel address. Both support mmap to userspace. The difference shows up in Phase 4: the page array from the page-backed path feeds directly into `ib_dma_map_sg` to build the scatter-gather table the RDMA NIC needs. The coherent path cannot do that — no page array, no SG table.

---

## The Platform Device: Why device_create Is Not Enough for DMA

The DMA API requires a device that is registered with a bus and carries a valid DMA mask. The char device created by `device_create` in Phase 1 is a userspace interface — it produces a `/dev/` node for open/ioctl/mmap, but it has no bus attachment and no IOMMU context. Calling `dma_alloc_coherent` on it returns NULL with no useful diagnostic.

A `platform_device` provides what the DMA subsystem needs:

```c
dev->pdev = platform_device_alloc("dmaplane_dma", -1);
if (!dev->pdev) {
    ret = -ENOMEM;
    goto err_free_dev;
}

ret = platform_device_add(dev->pdev);
if (ret)
    goto err_put_pdev;

ret = dma_set_mask_and_coherent(&dev->pdev->dev, DMA_BIT_MASK(64));
if (ret) {
    ret = dma_set_mask_and_coherent(&dev->pdev->dev, DMA_BIT_MASK(32));
    if (ret)
        goto err_unreg_pdev;
}
```

`platform_device_alloc` creates a virtual device. `platform_device_add` registers it with the platform bus so the DMA subsystem can program IOMMU entries for it. `dma_set_mask_and_coherent` tells the allocator what address widths the device supports — 64-bit first (all physical memory accessible), with a 32-bit fallback (4 GB limit). Without the mask, coherent allocation fails silently.

Every DMA call in the module uses `&dev->pdev->dev`, not the char device:

```c
buf->vaddr = dma_alloc_coherent(&dev->pdev->dev, buf->size,
                                &buf->dma_handle, GFP_KERNEL);
```

This separation — DMA device and userspace interface device as different objects — is standard in real drivers. The platform device owns the DMA context; the char device owns the file operations.

The error cleanup labels must distinguish between two failure modes: `platform_device_put` for a device that was allocated but never added to the bus, and `platform_device_unregister` for a device that was both allocated and added. Calling `unregister` on a device that was never `add`-ed corrupts the platform bus's internal list.

---

## The Page-Backed Path: alloc_page, vmap, and __GFP_ZERO

For large buffers, the driver allocates individual order-0 pages:

```c
nr_pages = DIV_ROUND_UP(buf->size, PAGE_SIZE);
buf->pages = kvcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);

for (i = 0; i < nr_pages; i++) {
    buf->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!buf->pages[i]) {
        while (i > 0)
            __free_page(buf->pages[--i]);
        kvfree(buf->pages);
        buf->pages = NULL;
        mutex_unlock(&dev->buf_lock);
        return -ENOMEM;
    }
}
```

`__GFP_ZERO` is not optional. Without it, each page contains whatever the previous owner left behind — potentially passwords, encryption keys, or private data from another process. Since these pages are mmapped to userspace, a missing `__GFP_ZERO` lets any process that opens `/dev/dmaplane` read stale kernel memory. For the coherent path, `dma_alloc_coherent` does not guarantee zeroing, so the driver explicitly calls `memset(buf->vaddr, 0, buf->size)` after allocation.

The error path unwinds in reverse order — if the 500th page allocation fails, pages 499 through 0 are freed along with the pointer array. This goto-style cleanup prevents memory leaks on partial allocation failure.

After the page array is populated, `vmap` creates a contiguous kernel virtual address:

```c
buf->vaddr = vmap(buf->pages, nr_pages, VM_MAP, PAGE_KERNEL);
```

`vmap` takes scattered physical pages and maps them into a contiguous range in the kernel's vmalloc address space. Without it, the kernel would need to `kmap` each page individually to access the buffer — impractical for multi-megabyte allocations. The vmapped address is also what the RDMA engine uses in Phase 4 as the SGE virtual address for rxe's memcpy-based send path.

---

## The mmap Zero-Copy Chain

The critical data path: allocate buffer in the kernel, mmap it to userspace, userspace writes data, the same physical pages are available to the NIC for transmission. No copies. This is what makes RDMA-based systems fast.

Userspace obtains the mmap parameters through a separate ioctl:

```c
info.mmap_offset = (__u64)buf->id << PAGE_SHIFT;
info.mmap_size = buf->size;
```

Then calls `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset)`. The kernel's mmap handler decodes the buffer ID from `vma->vm_pgoff` and maps the pages:

```c
case DMAPLANE_BUF_TYPE_PAGES:
    for (i = 0; i < buf->nr_pages && i * PAGE_SIZE < size; i++) {
        ret = vm_insert_page(vma, vma->vm_start + i * PAGE_SIZE,
                             buf->pages[i]);
        if (ret) {
            mutex_unlock(&dev->buf_lock);
            return ret;
        }
    }
    break;
```

For page-backed buffers, `vm_insert_page` maps each page individually — the pages are physically scattered, so `remap_pfn_range` (which requires contiguous PFNs) cannot be used. For coherent buffers, `dma_mmap_coherent` handles the mapping and sets the correct cache attributes automatically.

---

## The dma_mmap_coherent pgoff Interaction

`dma_mmap_coherent` interprets `vma->vm_pgoff` as a page offset into the coherent allocation. Since the driver encodes the buffer ID in `vm_pgoff`, any buffer with ID > 0 causes `dma_mmap_coherent` to seek past the end of the allocation and return `-ENXIO`. The fix is to reset `vm_pgoff` before the call:

```c
case DMAPLANE_BUF_TYPE_COHERENT:
    /*
     * Reset vm_pgoff to 0: we used it to encode the buffer ID,
     * but dma_mmap_coherent interprets it as a page offset into
     * the coherent allocation. A non-zero pgoff would cause
     * ENXIO because the offset exceeds the allocation size.
     */
    vma->vm_pgoff = 0;
    ret = dma_mmap_coherent(&dev->pdev->dev, vma, buf->vaddr,
                            buf->dma_handle, size);
```

This interaction is not documented in the `dma_mmap_coherent` API. The function signature accepts a VMA and a coherent allocation, but the implicit dependency on `vm_pgoff` is only visible in the DMA subsystem source. The `-ENXIO` error code does not suggest "page offset is wrong." It is the kind of bug that requires reading the implementation.

---

## VMA Flags and the mmap_count Lifecycle

Every DMA mmap sets three VMA flags:

```c
vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP);
```

**VM_DONTCOPY**: Prevents the mapping from being inherited by forked children. If a child process inherits a mapping to DMA pages, and the parent (or NIC) writes to those pages, the child sees data corruption or triggers a use-after-free if the parent destroys the buffer. In the AI context, Python's `multiprocessing` forks — without `VM_DONTCOPY`, every subprocess inherits DMA buffer mappings.

**VM_DONTEXPAND**: Prevents `mremap` from expanding the mapping beyond the buffer bounds. Without it, userspace could access memory past the allocated region.

**VM_DONTDUMP**: Excludes DMA pages from core dumps. Core dumps would race with ongoing DMA, and the dumped data might contain sensitive model weights or inference inputs.

The mmap_count tracking protects against use-after-free on destroy. An important detail of the Linux VM: `vma_open` is NOT called for the initial mmap — it fires only on fork (blocked by `VM_DONTCOPY`) and mremap (blocked by `VM_DONTEXPAND`). The mmap_count increment must happen in the mmap handler itself:

```c
/*
 * Increment mmap_count on success. NOT done via vma_open —
 * Linux does NOT call vma_open for the initial mmap.
 * vma_close IS called for the initial mapping's munmap.
 */
atomic_inc(&buf->mmap_count);
```

The decrement lives in `vma_close`, which is called for every munmap — including the initial mapping. So the count tracks all active mappings correctly: increment in the mmap handler, decrement in `vma_close`.

---

## The copy_to_user Undo Pattern

Buffer creation has a subtle failure mode. The buffer is allocated and assigned an ID, then the driver copies the result to userspace:

```c
ret = dmabuf_rdma_create_buffer(dev, &params);
if (ret)
    return ret;

if (copy_to_user((void __user *)arg, &params, sizeof(params))) {
    /*
     * Buffer was allocated but handle can't be returned —
     * destroy it to prevent a leaked buffer that userspace
     * can never reference.
     */
    dmabuf_rdma_destroy_buffer(dev, params.buf_id);
    return -EFAULT;
}
```

If `copy_to_user` fails (bad userspace pointer, unresolvable page fault), the buffer exists in the kernel but userspace never learned its handle. Without the undo, that buffer is leaked until module unload — it occupies a slot, consumes memory, and nobody can destroy it because nobody knows its ID. The destroy call on the error path prevents the orphan. This pattern is standard in any ioctl that creates a kernel resource and returns a handle.

---

## Allocation Sweep Results

The test suite includes a sweep across buffer sizes from 4 KB to 64 MB, measuring the full create/mmap/write/verify/munmap/destroy lifecycle:

| Size | Pages | Create (µs) | Map (µs) | Write (µs) | Total (µs) |
|------|-------|-------------|----------|------------|------------|
| 4 KB | 1 | 4 | 3 | 0 | 12 |
| 64 KB | 16 | 8 | 4 | 1 | 18 |
| 256 KB | 64 | 20 | 12 | 5 | 44 |
| 1 MB | 256 | 63 | 39 | 17 | 131 |
| 4 MB | 1024 | 223 | 98 | 63 | 404 |
| 16 MB | 4096 | 935 | 282 | 1421 | 2779 |
| 64 MB | 16384 | 5113 | 730 | 6617 | 13742 |

The dominant cost at large sizes is the page-by-page allocation loop — 16,384 individual `alloc_page` calls for a 64 MB buffer. Each call enters the page allocator, potentially triggers zone rebalancing, and returns a single 4 KB page. The write cost reflects the TLB miss cascade: the first touch of each page triggers a page fault that installs the PTE, and the TLB can only cache a limited number of entries before eviction.

These numbers establish the baseline. Phase 3 replaces `alloc_page` with `alloc_pages_node`, which costs roughly the same per page but ensures the pages land on the NUMA node local to the CPU that uses them — avoiding the 40–50% bandwidth penalty from cross-node memory access on a dual-socket machine.

---

## Destruction: Reverse-Order Cleanup with a Safety Guard

Buffer destruction reverses the allocation with a safety check:

```c
if (atomic_read(&buf->mmap_count) > 0) {
    mutex_unlock(&dev->buf_lock);
    return -EBUSY;
}

switch (buf->alloc_type) {
case DMAPLANE_BUF_TYPE_PAGES:
    if (buf->vaddr)
        vunmap(buf->vaddr);
    if (buf->pages) {
        for (i = 0; i < buf->nr_pages; i++)
            __free_page(buf->pages[i]);
        kvfree(buf->pages);
    }
    break;

case DMAPLANE_BUF_TYPE_COHERENT:
    if (buf->vaddr)
        dma_free_coherent(&dev->pdev->dev, buf->size,
                          buf->vaddr, buf->dma_handle);
    break;
}
```

The `mmap_count` guard prevents the use-after-free that would otherwise occur when an RDMA NIC (Phase 4) is still DMA-reading from pages that have been freed and recycled by the page allocator — another process's data ends up in the NIC's transmit buffer. With the guard, userspace must munmap before destroy. The test suite verifies this:

```c
/* Destroy must fail with EBUSY while mmap is active */
if (destroy_buffer(fd, buf_id) == 0) {
    TEST_FAIL(name, "destroy succeeded while mmapped (expected EBUSY)");
    munmap(ptr, size);
    return;
}
```

Module exit also checks for leaked mmaps:

```c
for (i = 0; i < DMAPLANE_MAX_BUFFERS; i++) {
    if (dev->buffers[i].in_use) {
        if (atomic_read(&dev->buffers[i].mmap_count) > 0)
            pr_warn("buffer %u still has %d active mmaps at exit\n",
                    dev->buffers[i].id,
                    atomic_read(&dev->buffers[i].mmap_count));
        dmabuf_rdma_destroy_buffer(dev, dev->buffers[i].id);
    }
}
```

The exit ordering matters: destroy the char device first (prevents new ioctls), then destroy buffers (which need the platform device for `dma_free_coherent`), then unregister the platform device. Reversing the last two steps — tearing down the platform device before freeing coherent buffers — calls `dma_free_coherent` on a device that no longer exists.

---

## What Comes Next

Phase 2 establishes the zero-copy chain: allocate in kernel, mmap to userspace, same physical pages visible to the NIC. Phase 3 makes it NUMA-aware — `alloc_pages_node` replaces `alloc_page`, and topology queries let userspace request buffers on a specific node. Phase 4 builds scatter-gather tables from the page array and registers RDMA memory regions. Phase 5 wraps the buffers as dma-buf objects for cross-device sharing without going through userspace.

Every later phase extends the chain without replacing it.

---

*Next: [Part 3 — NUMA-Aware Allocation and Topology Queries](/docs/blog_03_numa_topology.md)*
