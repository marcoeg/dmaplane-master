# Zero-Copy Sharing with dma-buf: One Buffer, N Devices

*Part 3 of 9 in a series on building a host-side data path emulator for AI infrastructure*

---

Phase 2 established the zero-copy chain: allocate pages in the kernel, mmap them to userspace, and the NIC reads the same physical memory for transmission. No copies. But that chain has a hard limitation — it connects one process to one device. A disaggregated inference pipeline needs the same buffer visible to a GPU (for prefill computation), a NIC (for RDMA transmission), and possibly a video encoder (for output streaming), all without copying. The kernel's answer to N-device sharing is dma-buf.

**dma-buf is the Linux subsystem for sharing DMA-capable memory between kernel drivers.** One driver (the exporter) wraps its pages in a `struct dma_buf` and hands out file descriptors. Other drivers (importers) attach to the dma-buf and receive scatter-gather tables mapped through their own IOMMU contexts. The physical pages never move. Each device gets DMA addresses valid for its own address space. This is how DRM/KMS shares framebuffers with display controllers, how V4L2 shares video frames with encoders, and how nvidia-peermem exposes GPU BAR pages to RDMA NICs for GPUDirect transfers.

This post covers the full dma-buf implementation in dmaplane: the `dma_buf_ops` vtable and its nine callbacks, the attach/map lifecycle, why the SG table must be built per-importer, why only page-backed buffers can be exported, the refcounting model that prevents use-after-free, and the non-obvious requirements (`O_RDWR`, `MODULE_IMPORT_NS`) that the API imposes without documenting clearly.

---

## The dma-buf Contract: Exporter and Importer

**The dma-buf framework enforces a strict exporter/importer contract through a vtable of callbacks.** The exporter is the driver that owns the physical pages. It implements `struct dma_buf_ops` — a set of function pointers that the dma-buf core invokes at each stage of the sharing lifecycle. Importers never touch the pages directly; they go through the framework, which calls back into the exporter at each step.

The vtable has nine callbacks in the dmaplane implementation:

```c
static const struct dma_buf_ops dmaplane_dmabuf_ops = {
    .attach         = dmaplane_dmabuf_attach,
    .detach         = dmaplane_dmabuf_detach,
    .map_dma_buf    = dmaplane_dmabuf_map,
    .unmap_dma_buf  = dmaplane_dmabuf_unmap,
    .vmap           = dmaplane_dmabuf_vmap,
    .vunmap         = dmaplane_dmabuf_vunmap,
    .release        = dmaplane_dmabuf_release,
    .mmap           = dmaplane_dmabuf_mmap,
    .begin_cpu_access = dmaplane_dmabuf_begin_cpu_access,
    .end_cpu_access   = dmaplane_dmabuf_end_cpu_access,
};
```

Nine callbacks exist because the framework separates concerns that simpler APIs collapse together. Attach is not map. Map is not mmap. CPU access requires explicit bracketing on non-coherent architectures. Each callback handles one responsibility, and the framework calls them in a strict order that the exporter cannot violate.

---

## The Attachment Lifecycle: Strict Ordering

**The dma-buf lifecycle proceeds through five stages: attach, map, use, unmap, detach.** Skipping a stage or reordering them produces undefined behavior — the framework does not enforce ordering at runtime, so the exporter's correctness depends on importers following the protocol.

1. **Attach** — The importer calls `dma_buf_attach`, passing its `struct device *`. The exporter's `attach` callback fires. dmaplane accepts all devices unconditionally (no IOMMU group filtering) and increments an attachment counter:

```c
static int dmaplane_dmabuf_attach(struct dma_buf *dmabuf,
                                  struct dma_buf_attachment *attach)
{
    struct dmaplane_dmabuf_ctx *ctx = dmabuf->priv;

    atomic_inc(&ctx->attach_count);
    atomic64_inc(&ctx->dev->stats.dmabuf_attachments);
    pr_debug("dmabuf attach: buf %u, device %s, count %d\n",
             ctx->buf->id, dev_name(attach->dev),
             atomic_read(&ctx->attach_count));
    return 0;
}
```

2. **Map** — The importer calls `dma_buf_map_attachment`, which invokes the exporter's `map_dma_buf`. This is the expensive step (see next section).

3. **Use** — The importer programs its DMA engine with the addresses from the SG table. Transfers happen without framework involvement.

4. **Unmap** — The importer calls `dma_buf_unmap_attachment`. The exporter tears down the per-device IOMMU mappings and frees the SG table.

5. **Detach** — The importer calls `dma_buf_detach`. The exporter decrements its attachment counter. When the last fd reference drops and all attachments are gone, the framework calls `release`.

The reason attach and map are separate: attach is cheap (bookkeeping only), and a device may need to attach once but map/unmap repeatedly as it starts and stops DMA operations. Collapsing them would force IOMMU reprogramming on every attach, which is wasteful when the device just needs to hold a reference.

---

## map_dma_buf Is the Expensive Callback

**`map_dma_buf` builds a scatter-gather table for the importing device, programming that device's IOMMU in the process.** This is the core of dma-buf's value proposition — and the reason the SG table cannot be built once at export time.

Each importing device may sit behind a different IOMMU. A NIC on PCIe bus 0 and a GPU on PCIe bus 1 have separate IOMMU translation contexts. The same physical page at address 0x1_0000_0000 maps to different DMA addresses in each device's view. Building one SG table at export time would assume a single IOMMU — wrong for any real cross-device sharing scenario.

The implementation:

```c
static struct sg_table *dmaplane_dmabuf_map(struct dma_buf_attachment *attach,
                                            enum dma_data_direction dir)
{
    struct dmaplane_dmabuf_ctx *ctx = attach->dmabuf->priv;
    struct sg_table *sgt;
    struct scatterlist *sg;
    unsigned int i;
    int ret;

    sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
    if (!sgt)
        return ERR_PTR(-ENOMEM);

    ret = sg_alloc_table(sgt, ctx->nr_pages, GFP_KERNEL);
    if (ret) {
        kfree(sgt);
        return ERR_PTR(ret);
    }

    /* Fill SG entries — one per page */
    for_each_sgtable_sg(sgt, sg, i)
        sg_set_page(sg, ctx->pages[i], PAGE_SIZE, 0);

    /*
     * dma_map_sgtable programs IOMMU entries for the importing device.
     * After this call, each SG entry's dma_address is valid for the
     * importer's DMA context.  On x86 without IOMMU, DMA addresses
     * equal physical addresses.
     */
    ret = dma_map_sgtable(attach->dev, sgt, dir, 0);
    if (ret) {
        sg_free_table(sgt);
        kfree(sgt);
        return ERR_PTR(ret);
    }

    atomic64_inc(&ctx->dev->stats.dmabuf_maps);
    return sgt;
}
```

Three steps: allocate the SG table structure, fill each entry with `sg_set_page` (one entry per physical page), then `dma_map_sgtable` walks the table and programs the importer's IOMMU. After this call, each scatterlist entry's `dma_address` field holds an address valid for that specific device's DMA context. On x86 without an IOMMU, DMA addresses equal physical addresses and the mapping is a no-op. With an IOMMU (standard on any server), the mapping allocates IOVA ranges and writes page table entries — the expensive part.

The `unmap_dma_buf` callback reverses this exactly: `dma_unmap_sgtable` tears down the IOMMU entries, then the SG table is freed:

```c
static void dmaplane_dmabuf_unmap(struct dma_buf_attachment *attach,
                                  struct sg_table *sgt,
                                  enum dma_data_direction dir)
{
    dma_unmap_sgtable(attach->dev, sgt, dir, 0);
    sg_free_table(sgt);
    kfree(sgt);
}
```

---

## Only Page-Backed Buffers Can Be Exported

**Coherent allocations from `dma_alloc_coherent` cannot be exported as dma-buf objects because they have no `struct page` array.** The SG table construction in `map_dma_buf` calls `sg_set_page`, which requires a `struct page *` for each entry. Coherent memory is a physically contiguous allocation with a kernel virtual address and a DMA handle, but `virt_to_page` on coherent memory is unreliable across architectures — especially when SWIOTLB bounce buffers or CMA regions are involved.

The export function enforces this at the top:

```c
/* Only page-backed buffers can be exported — coherent has no
 * page array for SG table construction */
if (buf->alloc_type != DMAPLANE_BUF_TYPE_PAGES) {
    mutex_unlock(&dev->buf_lock);
    return -EINVAL;
}
```

This is a fundamental architectural constraint, not a missing feature. The page-backed path (`alloc_page` per page, `vmap` for kernel access) produces the `struct page **` array that both dma-buf and RDMA MR registration (Phase 4) consume. The coherent path exists for small control structures that need hardware cache coherency — they do not need cross-device sharing.

---

## Refcounting: The Ownership Graph

**Three layers of references keep a dma-buf alive: the exporter's flag, the dma-buf file descriptor, and per-device attachments.** Understanding when each is acquired and released prevents use-after-free and leaked exports.

The export context borrows references to the backing buffer's page array and vaddr:

```c
struct dmaplane_dmabuf_ctx {
    struct dmaplane_dev *dev;       /* Back-pointer to device singleton */
    struct dmaplane_buffer *buf;    /* Backing buffer (borrowed) */
    struct page **pages;            /* Page array (borrowed from buf) */
    unsigned int nr_pages;          /* Number of pages */
    size_t size;                    /* Buffer size in bytes */
    void *vaddr;                    /* Kernel VA (borrowed from buf->vaddr) */
    atomic_t attach_count;          /* Active attachment count */
};
```

The `dmabuf_exported` flag on the backing `dmaplane_buffer` is the guard that prevents the buffer from being destroyed while its pages are shared. It is set under `buf_lock` when `dma_buf_export` succeeds and cleared under `buf_lock` in the release callback. The dma-buf holds a logical reference on the buffer — not via `kref`, but through this flag. The buffer destruction path checks it:

```c
/* Refuse if buffer is exported as dma-buf — importers may still
 * hold references.  The release callback clears this flag when
 * the last dma-buf reference drops. */
if (buf->dmabuf_exported) {
    mutex_unlock(&dev->buf_lock);
    return -EBUSY;
}
```

The dma-buf's own lifetime is managed by the VFS file reference count. `dma_buf_fd` installs a file descriptor in the process's fd table. Every `dma_buf_get` (kernel) or `dup` (userspace) increments the file refcount. When the last reference drops — all fds closed, all kernel `dma_buf_put` calls complete — the framework calls the exporter's `release` callback, which clears the flag:

```c
static void dmaplane_dmabuf_release(struct dma_buf *dmabuf)
{
    struct dmaplane_dmabuf_ctx *ctx = dmabuf->priv;
    struct dmaplane_dev *dev = ctx->dev;
    struct dmaplane_buffer *buf = ctx->buf;

    mutex_lock(&dev->buf_lock);
    buf->dmabuf_exported = false;
    buf->dmabuf = NULL;
    mutex_unlock(&dev->buf_lock);

    kfree(ctx);
}
```

The `mutex_lock` in `release` and the `mutex_lock` in `destroy_buffer` serialize on the same lock. A concurrent destroy either sees `dmabuf_exported == true` and returns `-EBUSY`, or sees `dmabuf_exported == false` and proceeds. There is no window where the flag is inconsistent.

---

## The Destroy Guard: Same Pattern as mmap_count

**The `dmabuf_exported` flag is a destroy guard identical in purpose to the `mmap_count` guard from Phase 2.** Both prevent the buffer's backing pages from being freed while another subsystem holds references to them.

Phase 2 introduced `mmap_count`: the mmap handler increments it, `vma_close` decrements it, and `destroy_buffer` refuses with `-EBUSY` if the count is positive. Phase 3 adds a second guard on the same path:

```c
if (atomic_read(&buf->mmap_count) > 0) {
    mutex_unlock(&dev->buf_lock);
    return -EBUSY;
}

if (buf->dmabuf_exported) {
    mutex_unlock(&dev->buf_lock);
    return -EBUSY;
}
```

Both guards enforce the same invariant: physical pages must not be freed while any consumer — userspace mapping or kernel dma-buf importer — still references them. Without the dma-buf guard, destroying the buffer while an RDMA NIC holds an attached SG table means the NIC's next DMA read fetches from recycled pages that now belong to another process.

Module exit checks for leaked exports the same way it checks for leaked mmaps:

```c
for (i = 0; i < DMAPLANE_MAX_BUFFERS; i++) {
    if (dma_dev->buffers[i].in_use) {
        if (dma_dev->buffers[i].dmabuf_exported)
            pr_warn("buffer %u still has active dma-buf export at exit\n",
                    dma_dev->buffers[i].id);
        if (atomic_read(&dma_dev->buffers[i].mmap_count) > 0)
            pr_warn("buffer %u still has %d active mmaps at exit\n",
                    dma_dev->buffers[i].id,
                    atomic_read(&dma_dev->buffers[i].mmap_count));
        dmabuf_rdma_destroy_buffer(dma_dev, dma_dev->buffers[i].id);
    }
}
```

---

## begin_cpu_access and end_cpu_access: Architecture-Dependent Stubs

**The `begin_cpu_access` and `end_cpu_access` callbacks bracket CPU access to dma-buf memory, but on x86 they are no-ops.** x86 provides hardware cache coherency — the CPU snoops the bus, and device writes are visible to CPU reads without explicit cache flushes. The callbacks exist because ARM and RISC-V have weakly ordered memory models where the CPU cache and the device's view of memory can diverge.

```c
static int dmaplane_dmabuf_begin_cpu_access(struct dma_buf *dmabuf,
                                            enum dma_data_direction dir)
{
    /* x86: hardware cache coherency — no sync needed */
    return 0;
}

static int dmaplane_dmabuf_end_cpu_access(struct dma_buf *dmabuf,
                                          enum dma_data_direction dir)
{
    /* x86: hardware cache coherency — no sync needed */
    return 0;
}
```

On ARM, `begin_cpu_access` would call `dma_sync_sg_for_cpu()` to invalidate CPU caches so reads see data written by the device. `end_cpu_access` would call `dma_sync_sg_for_device()` to flush CPU writes to the point of coherency. Implementing the callbacks as stubs is correct for x86 and preserves the interface for portability — an ARM port would fill in the sync calls without changing the vtable structure.

---

## The O_RDWR Requirement

**`dma_buf_export` requires `O_RDWR` in the export info flags for any dma-buf that will be mmapped with `PROT_WRITE`.** The dma-buf framework's internal mmap handler checks the file's access mode against the VMA protection flags. If the file was opened (created, in this case) with `O_RDONLY`, an mmap with `PROT_WRITE` fails with `-EACCES`.

```c
exp_info.flags = O_RDWR;    /* O_RDWR required for mmap with PROT_WRITE;
                             * O_CLOEXEC is set via dma_buf_fd below */
```

The `O_CLOEXEC` flag is set separately on the `dma_buf_fd` call, not in `exp_info.flags`. Setting `O_CLOEXEC` in `exp_info.flags` has no effect because `dma_buf_export` only inspects access mode flags. This split between where access mode and close-on-exec are configured is not documented in the dma-buf API headers — it is only visible in the `dma_buf_export` and `dma_buf_fd` implementations.

---

## MODULE_IMPORT_NS(DMA_BUF): The Symbol Namespace Gate

**Since kernel 5.x, the dma-buf framework exports its symbols under a named namespace, and modules that call those symbols must declare the import.** Without `MODULE_IMPORT_NS(DMA_BUF)`, the module compiles successfully but fails to load with "Unknown symbol" errors for `dma_buf_export`, `dma_buf_fd`, `dma_buf_put`, and every other dma-buf API function.

```c
/*
 * Import the DMA_BUF symbol namespace.  Since kernel 5.x, the dma-buf
 * framework exports its symbols (dma_buf_export, dma_buf_fd, dma_buf_put,
 * etc.) under this namespace.  Without this declaration, the module fails
 * to load with "Unknown symbol" errors for all dma-buf API calls.
 */
MODULE_IMPORT_NS(DMA_BUF);
```

Symbol namespaces are the kernel's mechanism for controlling which modules can use which subsystem APIs. Before namespaces, any module could call any exported symbol. Namespaces add an explicit opt-in — a module must declare which subsystem it depends on. The DMA_BUF namespace was one of the first to be introduced, and it catches every out-of-tree module that links against dma-buf without the declaration.

The error is insmod-time, not compile-time. The module object builds cleanly because the symbols are resolved against Module.symvers during the Kbuild link stage. The namespace check happens at module load when the kernel's symbol resolver verifies that the calling module has declared the namespace import. The resulting "Unknown symbol" error message does not mention namespaces, which makes the root cause non-obvious.

---

## The Export Function: Putting It Together

**The export path validates, allocates, wraps, and installs in a single critical section.** The entire operation — buffer lookup, type check, context allocation, `dma_buf_export`, `dma_buf_fd` — runs under `buf_lock`. Coarse granularity is acceptable because exports are rare operations (once per buffer lifetime), and the mutex permits sleeping through the allocation calls:

```c
int dmaplane_dmabuf_export(struct dmaplane_dev *dev,
                           struct dmaplane_export_dmabuf_arg *arg)
{
    struct dmaplane_buffer *buf;
    struct dmaplane_dmabuf_ctx *ctx;
    struct dma_buf *dmabuf;
    DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
    int fd;

    mutex_lock(&dev->buf_lock);

    buf = dmabuf_rdma_find_buffer(dev, arg->buf_id);
    if (!buf) { /* ... unlock, return -EINVAL */ }

    if (buf->alloc_type != DMAPLANE_BUF_TYPE_PAGES) { /* ... -EINVAL */ }
    if (buf->dmabuf_exported) { /* ... -EBUSY */ }

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    /* ... populate ctx fields from buf ... */

    exp_info.ops = &dmaplane_dmabuf_ops;
    exp_info.size = buf->size;
    exp_info.flags = O_RDWR;
    exp_info.priv = ctx;

    dmabuf = dma_buf_export(&exp_info);
    /* ... error handling ... */

    fd = dma_buf_fd(dmabuf, O_CLOEXEC);
    /* ... error handling ... */

    buf->dmabuf_exported = true;
    buf->dmabuf = dmabuf;
    mutex_unlock(&dev->buf_lock);

    arg->fd = fd;
    return 0;
}
```

The UAPI struct that userspace passes:

```c
struct dmaplane_export_dmabuf_arg {
    __u32 buf_id;       /* in  — buffer handle to export */
    __s32 fd;           /* out — dma-buf file descriptor */
};
```

Holding `buf_lock` across both `dma_buf_export` and `dma_buf_fd` prevents a race where two threads export the same buffer simultaneously — both would pass the `dmabuf_exported` check, and the buffer would end up with two dma-buf objects sharing the same pages with independent lifetimes.

---

## Connection to Phase 4: The Page Array Feeds RDMA

**The same `struct page **` array that `map_dma_buf` uses for SG table construction feeds directly into RDMA Memory Region registration.** In Phase 4, `ib_reg_mr` takes an SG table built from the page array and registers it with the NIC's protection domain. The NIC then has a Memory Key (rkey/lkey) that allows it to DMA directly from those pages.

The chain is: `alloc_page` (Phase 2) produces `struct page *` entries. The page array is stored on the buffer. dma-buf export (this phase) borrows the array and builds per-importer SG tables from it. RDMA MR registration (Phase 4) builds its own SG table from the same array and hands it to the NIC. The physical pages never move — every consumer gets its own mapping (SG table, VMA, IOMMU entries) into the same memory.

This is why dmaplane allocates pages individually rather than using compound pages or `vmalloc`: individual `struct page` pointers are the universal currency that dma-buf, RDMA, and `vm_insert_page` all accept. Every later phase extends the chain without replacing it.

---

*Next: [Part 4 — Kernel-Space RDMA from First Principles](/docs/blog_04_rdma_engine.md)*
