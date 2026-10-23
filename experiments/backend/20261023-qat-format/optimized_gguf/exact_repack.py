"""Lossless Q4_K/Q6_K coefficient repacking; CPU-only research code.

The represented float32 weights are preserved. This does NOT claim that a
different matmul kernel has identical accumulation or activation rounding.
No model, server, or installed package is modified by this module.
"""
from dataclasses import dataclass

import numpy as np


@dataclass
class Repacked:
    qtype: str
    shape: tuple[int, int]
    low: np.ndarray
    high: np.ndarray | None
    scale: np.ndarray
    minimum: np.ndarray | None

    @property
    def nbytes(self):
        return sum(a.nbytes for a in (self.low, self.high, self.scale,
                                     self.minimum) if a is not None)


def _blocks(data, block_bytes):
    data = np.asarray(data)
    if data.dtype != np.uint8 or data.ndim != 2:
        raise ValueError("expected two-dimensional packed uint8 rows")
    if data.shape[1] % block_bytes:
        raise ValueError("incomplete GGUF block")
    return data.reshape(-1, block_bytes), data.shape[0]


def unpack_q4k(data):
    b, rows = _blocks(data, 144)
    d = b[:, :2].copy().view('<f2').astype(np.float32)
    dm = b[:, 2:4].copy().view('<f2').astype(np.float32)
    raw = b[:, 4:16]
    scale_codes = np.empty((len(b), 8), np.uint8)
    min_codes = np.empty_like(scale_codes)
    for j in range(8):
        if j < 4:
            scale_codes[:, j] = raw[:, j] & 63
            min_codes[:, j] = raw[:, j + 4] & 63
        else:
            scale_codes[:, j] = (raw[:, j + 4] & 15) | ((raw[:, j - 4] >> 6) << 4)
            min_codes[:, j] = (raw[:, j + 4] >> 4) | ((raw[:, j] >> 6) << 4)
    pos = np.arange(256)
    codes = ((b[:, 16 + (pos // 64) * 32 + pos % 32]
              >> (4 * ((pos // 32) % 2))) & 15).astype(np.uint8)
    return (codes.reshape(rows, -1),
            (d * scale_codes.astype(np.float32)).reshape(rows, -1),
            (dm * min_codes.astype(np.float32)).reshape(rows, -1))


def unpack_q6k(data):
    b, rows = _blocks(data, 210)
    pos = np.arange(256)
    half, group, lane = pos // 128, (pos % 128) // 32, pos % 32
    lo = b[:, 64 * half + 32 * (group % 2) + lane]
    hi = b[:, 128 + 32 * half + lane]
    codes = (((lo >> (4 * (group // 2))) & 15)
             | (((hi >> (2 * group)) & 3) << 4)).astype(np.uint8)
    d = b[:, 208:].copy().view('<f2').astype(np.float32)
    scales = b[:, 192:208].copy().view(np.int8).astype(np.float32)
    return codes.reshape(rows, -1), (d * scales).reshape(rows, -1)


def repack(data, qtype):
    """Expand only group metadata to FP32; keep every quantized code.

    Q4_K: 4-bit codes + FP32 scale/min per32 = 6 bits/weight.
    Q6_K: 4-bit low + 2-bit high + FP32 signed scale per16 = 8 bits/weight.
    No division, quantizer fit, BF16 metadata rounding, or group merging.
    """
    if qtype == 'Q4_K':
        q, scale, minimum = unpack_q4k(data)
        high = None
    elif qtype == 'Q6_K':
        q, scale = unpack_q6k(data)
        minimum = None
        high = np.zeros((q.shape[0], q.shape[1] // 4), np.uint8)
        for j in range(4):
            high |= (q[:, j::4] >> 4) << (2 * j)
    else:
        raise ValueError(qtype)
    low = ((q[:, 0::2] & 15) | ((q[:, 1::2] & 15) << 4)).copy()
    return Repacked(qtype, q.shape, low, high, scale.copy(),
                    None if minimum is None else minimum.copy())


def repacked_codes(r):
    q = np.empty(r.shape, np.uint8)
    q[:, 0::2], q[:, 1::2] = r.low & 15, r.low >> 4
    if r.high is not None:
        for j in range(4):
            q[:, j::4] |= ((r.high >> (2 * j)) & 3) << 4
    return q


def decode(r):
    q = repacked_codes(r).astype(np.float32)
    if r.qtype == 'Q4_K':
        return (q.reshape(r.shape[0], -1, 32) * r.scale[:, :, None]
                - r.minimum[:, :, None]).reshape(r.shape)
    return ((q - 32).reshape(r.shape[0], -1, 16)
            * r.scale[:, :, None]).reshape(r.shape)


def decode_direct_offsets(data, qtype):
    """Vectorized CPU model of the direct-GGUF GPU address calculation."""
    b, rows = _blocks(data, 144 if qtype == 'Q4_K' else 210)
    k = np.arange(256)
    if qtype == 'Q4_K':
        g = k // 32
        j0 = b[:, 4 + np.where(g < 4, g, g - 4)]
        j1 = b[:, 4 + np.where(g < 4, g + 4, g)]
        j2 = b[:, 4 + np.where(g < 4, g, g + 4)]
        sc = np.where(g < 4, j0 & 63, (j2 & 15) | ((j0 >> 6) << 4))
        mn = np.where(g < 4, j1 & 63, (j2 >> 4) | ((j1 >> 6) << 4))
        q = (b[:, 16 + (k // 64) * 32 + k % 32] >> ((g % 2) * 4)) & 15
        d = b[:, :2].copy().view('<f2').astype(np.float32)
        dm = b[:, 2:4].copy().view('<f2').astype(np.float32)
        return ((d * sc.astype(np.float32)) * q.astype(np.float32)
                - dm * mn.astype(np.float32)).reshape(rows, -1)
    if qtype != 'Q6_K':
        raise ValueError(qtype)
    h, g, lane = k // 128, (k % 128) // 32, k % 32
    lo = b[:, 64 * h + 32 * (g % 2) + lane]
    hi = b[:, 128 + 32 * h + lane]
    q = ((lo >> (4 * (g // 2))) & 15) | (((hi >> (2 * g)) & 3) << 4)
    sc = b[:, 192 + k // 16].copy().view(np.int8).astype(np.float32)
    d = b[:, 208:].copy().view('<f2').astype(np.float32)
    return ((d * sc) * (q.astype(np.float32) - 32)).reshape(rows, -1)


def bf16_round(x):
    """Round finite FP32 to BF16 then widen to FP32; nearest ties to even."""
    x = np.asarray(x, np.float32)
    bits = x.view(np.uint32)
    return ((bits + np.uint32(0x7fff) + ((bits >> 16) & 1))
            & np.uint32(0xffff0000)).view(np.float32)


def bf16_pair(x):
    """Two BF16 operands whose FP32 sum recovers substantially more precision.

    This is measured, not assumed lossless: tiny residuals can underflow and
    full-range FP32 generally needs more than two BF16 mantissas.
    """
    hi = bf16_round(x)
    lo = bf16_round(np.asarray(x, np.float32) - hi)
    return hi, lo


def relative_rms(a, b):
    a, b = np.asarray(a, np.float64), np.asarray(b, np.float64)
    return float(np.sqrt(np.sum((a-b)**2) / max(np.sum(b*b), 1e-300)))
