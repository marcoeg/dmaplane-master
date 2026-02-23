#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
dmaplane_py.py — Python wrapper for /dev/dmaplane ioctls

Uses struct.pack / fcntl.ioctl for zero-dependency access to the kernel
driver. All struct format strings use '=' prefix (native byte order,
standard alignment) and match the kernel UAPI structs exactly.

Struct sizes verified against dmaplane_uapi.h:
  buf_params:       24 bytes  =IIQii
  rdma_setup:       52 bytes  =32sIIIII
  mr_params:        32 bytes  =IIIIIIQ
  mmap_info:        24 bytes  =IIQQ
  gpu_pin_params:   40 bytes  =QQIiIIQ
  gpu_unpin_params:  4 bytes  =I
  gpu_mr_params:    16 bytes  =IIII
  write_imm_params: 48 bytes  =IIQQIIIIQ
  post_recv_params: 16 bytes  =IIII
  poll_recv_params: 32 bytes  =IIIIIIQ
"""

import os
import struct
import fcntl
import mmap
import gc

# ── ioctl encoding (matches <linux/ioctl.h>) ──────────────────────────────
#
# Linux ioctl command numbers encode direction, type (magic), number, and
# struct size into a 32-bit integer.  The layout is:
#   [31:30] direction (0=none, 1=write, 2=read, 3=read+write)
#   [29:16] struct size in bytes
#   [15:8]  magic number (0xE4 for dmaplane)
#   [7:0]   command number
#
# _IOWR means the kernel both reads and writes the struct (in-place update).

DMAPLANE_MAGIC = 0xE4

def _IOC(d, t, nr, sz):
    return (d << 30) | (sz << 16) | (t << 8) | nr

def _IO(t, nr):
    return _IOC(0, t, nr, 0)

def _IOR(t, nr, sz):
    return _IOC(2, t, nr, sz)

def _IOW(t, nr, sz):
    return _IOC(1, t, nr, sz)

def _IOWR(t, nr, sz):
    return _IOC(3, t, nr, sz)


# ── Struct format strings ─────────────────────────────────────────────────
#
# All format strings use '=' prefix: native byte order, standard sizes
# (no compiler padding).  These MUST match the kernel UAPI structs in
# include/dmaplane_uapi.h byte-for-byte.  The self-test at the bottom
# of this file validates sizes; test_dmaplane_py.py tests round-trips.

# struct dmaplane_buf_params { u32, u32, u64, s32, s32 } = 24
FMT_BUF_PARAMS = "=IIQii"
SZ_BUF_PARAMS = struct.calcsize(FMT_BUF_PARAMS)

# struct dmaplane_rdma_setup { char[32], u32, u32, u32, u32, u32 } = 52
FMT_RDMA_SETUP = "=32sIIIII"
SZ_RDMA_SETUP = struct.calcsize(FMT_RDMA_SETUP)

# struct dmaplane_mr_params { u32, u32, u32, u32, u32, u32, u64 } = 32
FMT_MR_PARAMS = "=IIIIIIQ"
SZ_MR_PARAMS = struct.calcsize(FMT_MR_PARAMS)

# struct dmaplane_mmap_info { u32, u32, u64, u64 } = 24
FMT_MMAP_INFO = "=IIQQ"
SZ_MMAP_INFO = struct.calcsize(FMT_MMAP_INFO)

# struct dmaplane_gpu_pin_params { u64, u64, u32, s32, u32, u32, u64 } = 40
FMT_GPU_PIN = "=QQIiIIQ"
SZ_GPU_PIN = struct.calcsize(FMT_GPU_PIN)

# struct dmaplane_gpu_unpin_params { u32 } = 4
FMT_GPU_UNPIN = "=I"
SZ_GPU_UNPIN = struct.calcsize(FMT_GPU_UNPIN)

# struct dmaplane_gpu_mr_params { u32, u32, u32, u32 } = 16
FMT_GPU_MR = "=IIII"
SZ_GPU_MR = struct.calcsize(FMT_GPU_MR)

# struct dmaplane_write_imm_params { u32, u32, u64, u64, u32, u32, u32, u32, u64 } = 48
FMT_WRITE_IMM = "=IIQQIIIIQ"
SZ_WRITE_IMM = struct.calcsize(FMT_WRITE_IMM)

# struct dmaplane_post_recv_params { u32, u32, u32, u32 } = 16
FMT_POST_RECV = "=IIII"
SZ_POST_RECV = struct.calcsize(FMT_POST_RECV)

# struct dmaplane_poll_recv_params { u32, u32, u32, u32, u32, u32, u64 } = 32
FMT_POLL_RECV = "=IIIIIIQ"
SZ_POLL_RECV = struct.calcsize(FMT_POLL_RECV)

# struct dmaplane_remote_op_params { u32, u32, u64, u32, u32, u32, u32 } = 32
FMT_REMOTE_OP = "=IIQIIII"
SZ_REMOTE_OP = struct.calcsize(FMT_REMOTE_OP)

# ── ioctl command numbers ─────────────────────────────────────────────────

IOCTL_CREATE_BUFFER     = _IOWR(DMAPLANE_MAGIC, 0x05, SZ_BUF_PARAMS)
IOCTL_DESTROY_BUFFER    = _IOW (DMAPLANE_MAGIC, 0x06, 4)
IOCTL_GET_MMAP_INFO     = _IOWR(DMAPLANE_MAGIC, 0x08, SZ_MMAP_INFO)

IOCTL_SETUP_RDMA        = _IOWR(DMAPLANE_MAGIC, 0x10, SZ_RDMA_SETUP)
IOCTL_TEARDOWN_RDMA     = _IO  (DMAPLANE_MAGIC, 0x11)

IOCTL_REGISTER_MR       = _IOWR(DMAPLANE_MAGIC, 0x20, SZ_MR_PARAMS)
IOCTL_DEREGISTER_MR     = _IOW (DMAPLANE_MAGIC, 0x21, 4)

IOCTL_GPU_PIN           = _IOWR(DMAPLANE_MAGIC, 0x60, SZ_GPU_PIN)
IOCTL_GPU_UNPIN         = _IOW (DMAPLANE_MAGIC, 0x61, SZ_GPU_UNPIN)
IOCTL_GPU_REGISTER_MR   = _IOWR(DMAPLANE_MAGIC, 0x65, SZ_GPU_MR)

IOCTL_RDMA_WRITE_IMM    = _IOWR(DMAPLANE_MAGIC, 0x80, SZ_WRITE_IMM)
IOCTL_RDMA_POST_RECV    = _IOWR(DMAPLANE_MAGIC, 0x81, SZ_POST_RECV)
IOCTL_RDMA_POLL_RECV    = _IOWR(DMAPLANE_MAGIC, 0x82, SZ_POLL_RECV)

# ── IB access flags (mirror kernel values from <rdma/ib_verbs.h>) ─────────
#
# LOCAL_WRITE:  HCA can write to this MR (needed for recv buffers)
# REMOTE_WRITE: remote HCA can RDMA WRITE into this MR (needs fast-reg path)
# REMOTE_READ:  remote HCA can RDMA READ from this MR

IB_ACCESS_LOCAL_WRITE  = 1
IB_ACCESS_REMOTE_WRITE = 1 << 1
IB_ACCESS_REMOTE_READ  = 1 << 2

# ── Buffer types ──────────────────────────────────────────────────────────

BUF_TYPE_COHERENT = 0
BUF_TYPE_PAGES    = 1
NUMA_ANY          = -1

# ── KVCache IMM encoding (mirrors kvcache_proto.h) ───────────────────────
#
# The 32-bit immediate value delivered via RDMA WRITE WITH IMMEDIATE encodes
# a (layer_index, chunk_index) pair.  The receiver decodes it from the CQ
# completion to track which chunk arrived — no memory polling needed.
# Sentinel value 0xFFFFFFFF signals end-of-transfer.

KVCACHE_SENTINEL = 0xFFFFFFFF

def kvcache_imm_encode(layer, chunk):
    """Encode (layer, chunk) into a 32-bit immediate: upper 16 = layer, lower 16 = chunk."""
    return ((layer & 0xFFFF) << 16) | (chunk & 0xFFFF)

def kvcache_imm_layer(imm):
    """Extract layer index from 32-bit immediate."""
    return (imm >> 16) & 0xFFFF

def kvcache_imm_chunk(imm):
    """Extract chunk index from 32-bit immediate."""
    return imm & 0xFFFF

# ── RxE device detection ─────────────────────────────────────────────────

def find_rxe_device():
    """Scan /sys/class/infiniband for an rxe_* device. Returns name or None."""
    ib_dir = "/sys/class/infiniband"
    if not os.path.isdir(ib_dir):
        return None
    for name in os.listdir(ib_dir):
        if name.startswith("rxe_"):
            return name
    return None


# ── DmaplaneClient class ─────────────────────────────────────────────────

class DmaplaneClient:
    """Python ioctl interface to /dev/dmaplane.

    Each method packs a C struct via struct.pack, calls fcntl.ioctl (which
    mutates the bytearray in-place), then unpacks the result.  This mirrors
    what C code does with a stack-allocated struct + ioctl(fd, cmd, &s).

    Use as a context manager to ensure the fd and mmaps are cleaned up:
        with DmaplaneClient() as cl:
            cl.setup_rdma()
            ...
            cl.teardown_rdma()
    """

    def __init__(self, device="/dev/dmaplane"):
        self.fd = os.open(device, os.O_RDWR)
        self._mmaps = {}  # buf_id -> (mmap_obj, size) — tracked for cleanup

    def close(self):
        for buf_id in list(self._mmaps.keys()):
            self._munmap(buf_id)
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def _ioctl(self, name, cmd, buf):
        """Thin wrapper that includes ioctl name in exceptions."""
        try:
            fcntl.ioctl(self.fd, cmd, buf)
        except OSError as e:
            raise OSError(e.errno,
                          f"dmaplane {name}: {os.strerror(e.errno)}") from e

    # ── Buffer management ─────────────────────────────────────────────

    def create_buffer(self, size, numa_node=NUMA_ANY, alloc_type=BUF_TYPE_PAGES):
        """Create a DMA buffer. Returns (buf_id, actual_numa_node)."""
        buf = bytearray(struct.pack(FMT_BUF_PARAMS,
                                    0, alloc_type, size, numa_node, 0))
        self._ioctl("CREATE_BUFFER", IOCTL_CREATE_BUFFER, buf)
        fields = struct.unpack(FMT_BUF_PARAMS, buf)
        return {"buf_id": fields[0], "actual_numa_node": fields[4]}

    def destroy_buffer(self, buf_id):
        """Destroy a DMA buffer."""
        buf = bytearray(struct.pack("=I", buf_id))
        self._ioctl("DESTROY_BUFFER", IOCTL_DESTROY_BUFFER, buf)

    def get_mmap_info(self, buf_id):
        """Get mmap offset and size for a buffer. Returns (offset, size)."""
        buf = bytearray(struct.pack(FMT_MMAP_INFO, buf_id, 0, 0, 0))
        self._ioctl("GET_MMAP_INFO", IOCTL_GET_MMAP_INFO, buf)
        fields = struct.unpack(FMT_MMAP_INFO, buf)
        return {"mmap_offset": fields[2], "mmap_size": fields[3]}

    def mmap_buffer(self, buf_id, prot=mmap.PROT_READ | mmap.PROT_WRITE):
        """mmap a buffer into this process. Returns mmap object."""
        info = self.get_mmap_info(buf_id)
        m = mmap.mmap(self.fd, info["mmap_size"],
                      prot=prot, flags=mmap.MAP_SHARED,
                      offset=info["mmap_offset"])
        self._mmaps[buf_id] = (m, info["mmap_size"])
        return m

    def _munmap(self, buf_id):
        if buf_id in self._mmaps:
            self._mmaps[buf_id][0].close()
            del self._mmaps[buf_id]

    # ── RDMA setup/teardown ───────────────────────────────────────────

    def setup_rdma(self, ib_dev=None, cq_depth=256, max_send_wr=64,
                   max_recv_wr=64):
        """Setup RDMA resources (PD, CQ, QP pair). Auto-detects rxe if ib_dev is None.

        Creates: protection domain, send + recv completion queues, and a
        loopback QP pair (QP-A for sends, QP-B for receives).  The port field
        (second u32) is set to 1 — IB port numbering starts at 1.
        """
        if ib_dev is None:
            ib_dev = find_rxe_device()
            if ib_dev is None:
                raise RuntimeError("No rxe device found")
        dev_bytes = ib_dev.encode("utf-8")[:32].ljust(32, b"\x00")
        buf = bytearray(struct.pack(FMT_RDMA_SETUP,
                                    dev_bytes, 1, cq_depth,
                                    max_send_wr, max_recv_wr, 0))
        self._ioctl("SETUP_RDMA", IOCTL_SETUP_RDMA, buf)

    def teardown_rdma(self):
        """Teardown RDMA resources."""
        fcntl.ioctl(self.fd, IOCTL_TEARDOWN_RDMA, 0)

    # ── MR management ─────────────────────────────────────────────────

    def register_mr(self, buf_id, access_flags=IB_ACCESS_LOCAL_WRITE):
        """Register an MR. Returns dict with mr_id, lkey, rkey, addr.

        If access_flags includes REMOTE_WRITE, the kernel uses the fast-reg
        path (ib_alloc_mr + IB_WR_REG_MR) and returns nonzero rkey + addr
        that the remote HCA needs for RDMA WRITE targeting.
        """
        buf = bytearray(struct.pack(FMT_MR_PARAMS,
                                    0, buf_id, access_flags, 0, 0, 0, 0))
        self._ioctl("REGISTER_MR", IOCTL_REGISTER_MR, buf)
        fields = struct.unpack(FMT_MR_PARAMS, buf)
        return {
            "mr_id": fields[0],
            "buf_id": fields[1],
            "access_flags": fields[2],
            "lkey": fields[3],
            "rkey": fields[4],
            "addr": fields[6],
        }

    def deregister_mr(self, mr_id):
        """Deregister an MR."""
        buf = bytearray(struct.pack("=I", mr_id))
        self._ioctl("DEREGISTER_MR", IOCTL_DEREGISTER_MR, buf)

    # ── GPU P2P ───────────────────────────────────────────────────────

    def gpu_pin(self, gpu_va, size):
        """Pin GPU memory. Returns dict with handle, gpu_numa_node, etc."""
        buf = bytearray(struct.pack(FMT_GPU_PIN,
                                    gpu_va, size, 0, 0, 0, 0, 0))
        self._ioctl("GPU_PIN", IOCTL_GPU_PIN, buf)
        fields = struct.unpack(FMT_GPU_PIN, buf)
        return {
            "gpu_va": fields[0],
            "size": fields[1],
            "handle": fields[2],
            "gpu_numa_node": fields[3],
            "num_pages": fields[4],
            "bar1_consumed": fields[6],
        }

    def gpu_unpin(self, handle):
        """Unpin GPU memory."""
        buf = bytearray(struct.pack(FMT_GPU_UNPIN, handle))
        self._ioctl("GPU_UNPIN", IOCTL_GPU_UNPIN, buf)

    def gpu_register_mr(self, gpu_handle):
        """Register a GPU MR. Returns dict with mr_id, lkey, rkey."""
        buf = bytearray(struct.pack(FMT_GPU_MR, gpu_handle, 0, 0, 0))
        self._ioctl("GPU_REGISTER_MR", IOCTL_GPU_REGISTER_MR, buf)
        fields = struct.unpack(FMT_GPU_MR, buf)
        return {
            "gpu_handle": fields[0],
            "mr_id": fields[1],
            "lkey": fields[2],
            "rkey": fields[3],
        }

    # ── WRITEIMM operations ───────────────────────────────────────────

    def write_imm(self, local_mr_id, local_offset, remote_addr, remote_rkey,
                  length, imm_data, use_peer_qp=0):
        """RDMA WRITE with immediate data. Returns elapsed_ns.

        Posts IB_WR_RDMA_WRITE_WITH_IMM: copies 'length' bytes from
        local_mr[local_offset] to remote_addr, and delivers imm_data
        through the receiver's completion queue.  A matching recv WR
        must be posted before calling this — otherwise the HCA returns
        RNR (Receiver Not Ready) and the send times out.
        """
        buf = bytearray(struct.pack(FMT_WRITE_IMM,
                                    local_mr_id, length, local_offset,
                                    remote_addr, remote_rkey, imm_data,
                                    use_peer_qp, 0, 0))
        self._ioctl("WRITE_IMM", IOCTL_RDMA_WRITE_IMM, buf)
        fields = struct.unpack(FMT_WRITE_IMM, buf)
        status = fields[7]
        if status != 0:
            raise OSError(5, f"dmaplane WRITE_IMM: WC status {status}")
        return fields[8]  # elapsed_ns

    def post_recv(self, mr_id, size, use_peer_qp=0):
        """Post a recv WR — required before each write_imm from the sender.

        The recv WR tells the HCA where to deliver the immediate data
        completion.  Without a posted recv, RDMA WRITE WITH IMM triggers
        RNR on the receiving QP.
        """
        buf = bytearray(struct.pack(FMT_POST_RECV,
                                    mr_id, size, use_peer_qp, 0))
        self._ioctl("POST_RECV", IOCTL_RDMA_POST_RECV, buf)
        fields = struct.unpack(FMT_POST_RECV, buf)
        if fields[3] != 0:
            raise OSError(5, f"dmaplane POST_RECV: status {fields[3]}")

    def poll_recv(self, use_peer_qp=0, timeout_ms=10000):
        """Poll recv CQ for a WRITEIMM completion.

        Blocks up to timeout_ms.  On success, returns imm_data (the 32-bit
        immediate from the sender) and byte_len (RDMA payload size).
        Raises OSError(ETIMEDOUT) if no completion arrives.
        """
        buf = bytearray(struct.pack(FMT_POLL_RECV,
                                    use_peer_qp, timeout_ms, 0, 0, 0, 0, 0))
        self._ioctl("POLL_RECV", IOCTL_RDMA_POLL_RECV, buf)
        fields = struct.unpack(FMT_POLL_RECV, buf)
        return {
            "status": fields[2],
            "wc_flags": fields[3],
            "imm_data": fields[4],
            "byte_len": fields[5],
            "elapsed_ns": fields[6],
        }


# ── GPU tensor alignment helper (for PyTorch) ────────────────────────────

def allocate_aligned_gpu_tensor(size_bytes, alignment=65536):
    """
    Allocate a GPU tensor with guaranteed 64KB-aligned data_ptr.

    NVIDIA P2P API requires 64KB-aligned VA and size for gpu_pin.
    cudaMalloc usually returns 256B-aligned addresses, so we
    over-allocate and slice to the aligned boundary.

    Returns (aligned_tensor, raw_storage_ref) — keep raw_storage_ref
    alive to prevent GC from freeing the underlying storage.
    """
    try:
        import torch
    except ImportError:
        raise RuntimeError("PyTorch required for allocate_aligned_gpu_tensor")

    if not torch.cuda.is_available():
        raise RuntimeError("CUDA not available")

    # Round up size to alignment boundary
    aligned_size = (size_bytes + alignment - 1) & ~(alignment - 1)

    # Over-allocate to guarantee we can find an aligned region
    raw = torch.empty(aligned_size + alignment, dtype=torch.uint8,
                      device="cuda")
    raw_ptr = raw.data_ptr()

    # Find the next aligned address
    offset = (alignment - (raw_ptr % alignment)) % alignment
    aligned = raw[offset:offset + aligned_size]

    # Verify alignment
    assert aligned.data_ptr() % alignment == 0, \
        f"Alignment failed: data_ptr=0x{aligned.data_ptr():x}"

    # Keep a reference to the raw storage so GC doesn't free it
    aligned._raw_storage_ref = raw

    return aligned


# ── Self-test (run as script) ─────────────────────────────────────────────

if __name__ == "__main__":
    print("dmaplane_py.py — struct size validation")
    expected = {
        "buf_params": (FMT_BUF_PARAMS, 24),
        "rdma_setup": (FMT_RDMA_SETUP, 52),
        "mr_params": (FMT_MR_PARAMS, 32),
        "mmap_info": (FMT_MMAP_INFO, 24),
        "gpu_pin_params": (FMT_GPU_PIN, 40),
        "gpu_unpin_params": (FMT_GPU_UNPIN, 4),
        "gpu_mr_params": (FMT_GPU_MR, 16),
        "write_imm_params": (FMT_WRITE_IMM, 48),
        "post_recv_params": (FMT_POST_RECV, 16),
        "poll_recv_params": (FMT_POLL_RECV, 32),
        "remote_op_params": (FMT_REMOTE_OP, 32),
    }
    all_ok = True
    for name, (fmt, exp_sz) in expected.items():
        actual = struct.calcsize(fmt)
        status = "OK" if actual == exp_sz else "FAIL"
        if actual != exp_sz:
            all_ok = False
        print(f"  {name:20s} fmt={fmt:12s}  size={actual:3d}  expected={exp_sz:3d}  [{status}]")

    print(f"\nAll struct sizes correct: {'YES' if all_ok else 'NO'}")

    print("\nIoctl command numbers:")
    for name, val in [
        ("CREATE_BUFFER", IOCTL_CREATE_BUFFER),
        ("DESTROY_BUFFER", IOCTL_DESTROY_BUFFER),
        ("GET_MMAP_INFO", IOCTL_GET_MMAP_INFO),
        ("SETUP_RDMA", IOCTL_SETUP_RDMA),
        ("TEARDOWN_RDMA", IOCTL_TEARDOWN_RDMA),
        ("REGISTER_MR", IOCTL_REGISTER_MR),
        ("DEREGISTER_MR", IOCTL_DEREGISTER_MR),
        ("WRITE_IMM", IOCTL_RDMA_WRITE_IMM),
        ("POST_RECV", IOCTL_RDMA_POST_RECV),
        ("POLL_RECV", IOCTL_RDMA_POLL_RECV),
    ]:
        print(f"  {name:20s} 0x{val:08x}")
