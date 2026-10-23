"""Isolated GGUF Tensor Core candidate. Never installs or patches a backend.

Loads original Q4_K/Q6_K bytes. All scale/min reconstruction is FP32 before
the selected tensor-core operand conversion. Unlike the plugin fallback,
one dot spans BLOCK_K values instead of looping over each16/32-value group.
The CPU offset mirror is exhaustively tested separately. GPU compilation,
numerical checks and timing are separate gates; this module claims no speedup.
"""
import torch
import triton
import triton.language as tl


@triton.jit
def _load_half_as_float(w, offsets, mask):
    lo = tl.load(w + offsets, mask=mask, other=0).to(tl.uint16)
    hi = tl.load(w + offsets + 1, mask=mask, other=0).to(tl.uint16)
    return (lo | (hi << 8)).to(tl.float16, bitcast=True).to(tl.float32)


@triton.jit
def _direct_gemm(
    X, W, Y,
    M, N: tl.constexpr, K: tl.constexpr,
    SXM: tl.constexpr, SXK: tl.constexpr, SWN: tl.constexpr,
    QTYPE: tl.constexpr, PRECISION: tl.constexpr,
    BLOCK_M: tl.constexpr, BLOCK_N: tl.constexpr, BLOCK_K: tl.constexpr,
    SPLIT_K: tl.constexpr,
):
    rm = tl.program_id(0) * BLOCK_M + tl.arange(0, BLOCK_M)
    rn = tl.program_id(1) * BLOCK_N + tl.arange(0, BLOCK_N)
    rk = tl.arange(0, BLOCK_K)
    acc = tl.zeros((BLOCK_M, BLOCK_N), tl.float32)
    part = tl.program_id(2)
    for start in range(part, tl.cdiv(K, BLOCK_K), SPLIT_K):
        kk = start * BLOCK_K + rk
        pos = kk % 256
        valid = (rn[:, None] < N) & (kk[None, :] < K)
        x = tl.load(X + rm[:, None] * SXM + kk[None, :] * SXK,
                    mask=(rm[:, None] < M) & (kk[None, :] < K), other=0)
        if QTYPE == 12:
            base = rn[:, None] * SWN + (kk[None, :] // 256) * 144
            group = pos // 32
            a = tl.load(W + base + 4 + tl.where(group < 4, group, group - 4)[None, :], mask=valid, other=0)
            b = tl.load(W + base + 4 + tl.where(group < 4, group + 4, group)[None, :], mask=valid, other=0)
            c = tl.load(W + base + 4 + tl.where(group < 4, group, group + 4)[None, :], mask=valid, other=0)
            sc = tl.where(group[None, :] < 4, a & 63, (c & 15) | ((a >> 6) << 4))
            mn = tl.where(group[None, :] < 4, b & 63, (c >> 4) | ((b >> 6) << 4))
            q = tl.load(W + base + 16 + ((pos // 64) * 32 + pos % 32)[None, :], mask=valid, other=0)
            q = (q >> ((group % 2) * 4)[None, :]) & 15
            d = _load_half_as_float(W, base, valid)
            dm = _load_half_as_float(W, base + 2, valid)
            value = (d * sc.to(tl.float32)) * q.to(tl.float32) - dm * mn.to(tl.float32)
        else:
            base = rn[:, None] * SWN + (kk[None, :] // 256) * 210
            half, group, lane = pos // 128, (pos % 128) // 32, pos % 32
            lo = tl.load(W + base + (half * 64 + (group % 2) * 32 + lane)[None, :], mask=valid, other=0)
            hi = tl.load(W + base + 128 + (half * 32 + lane)[None, :], mask=valid, other=0)
            q = ((lo >> (4 * (group // 2))[None, :]) & 15) | (((hi >> (2 * group)[None, :]) & 3) << 4)
            sc = tl.load(W + base + 192 + (pos // 16)[None, :], mask=valid, other=0).to(tl.int8, bitcast=True)
            d = _load_half_as_float(W, base + 208, valid)
            value = (d * sc.to(tl.float32)) * (q.to(tl.float32) - 32.0)
        if PRECISION == 'bf16':
            acc = tl.dot(x.to(tl.bfloat16), tl.trans(value.to(tl.bfloat16)), acc)
        elif PRECISION == 'bf16_pair':
            high = value.to(tl.bfloat16)
            low = (value - high.to(tl.float32)).to(tl.bfloat16)
            acc = tl.dot(x.to(tl.bfloat16), tl.trans(high), acc)
            acc = tl.dot(x.to(tl.bfloat16), tl.trans(low), acc)
        elif PRECISION == 'tf32x3':
            acc = tl.dot(x.to(tl.float32), tl.trans(value), acc, input_precision='tf32x3')
        else:
            acc = tl.dot(x.to(tl.float32), tl.trans(value), acc, input_precision='ieee')
    tl.store(Y + part * M * N + rm[:, None] * N + rn[None, :], acc,
             mask=(rm[:, None] < M) & (rn[None, :] < N))


@triton.jit
def _reduce_splits(Parts, Y, SIZE, SPLIT_K: tl.constexpr, BLOCK: tl.constexpr):
    offs = tl.program_id(0) * BLOCK + tl.arange(0, BLOCK)
    acc = tl.zeros((BLOCK,), tl.float32)
    for part in range(SPLIT_K):
        acc += tl.load(Parts + part * SIZE + offs, mask=offs < SIZE, other=0)
    tl.store(Y + offs, acc, mask=offs < SIZE)


def direct_gemm(w, x, qtype, *, precision='bf16', block_m=16,
                block_n=64, block_k=128, num_warps=4, num_stages=3, split_k=1):
    if int(qtype) not in (12, 14):
        raise ValueError('only Q4_K and Q6_K')
    if precision not in ('bf16', 'bf16_pair', 'tf32x3', 'ieee'):
        raise ValueError(precision)
    if not x.is_cuda or not w.is_cuda or w.device != x.device:
        raise ValueError('GPU probe only; tensors must share a CUDA device')
    if w.dtype != torch.uint8 or x.ndim != 2 or w.ndim != 2:
        raise ValueError('expected X[M,K] and packed uint8 W[N,rowbytes]')
    if x.dtype != torch.bfloat16:
        raise ValueError('current candidate contract requires BF16 activations')
    if split_k not in (1, 2, 4, 8, 16):
        raise ValueError('split_k must be1,2,4,8 or16')
    m, k = x.shape
    n = w.shape[0]
    width = 144 if int(qtype) == 12 else 210
    if k % 256 or w.shape[1] != (k // 256) * width or w.stride(1) != 1:
        raise ValueError('incompatible or incomplete GGUF block rows')
    y = torch.empty((m, n), device=x.device, dtype=x.dtype)
    if m == 0 or n == 0:
        return y
    target = y if split_k == 1 else torch.empty((split_k, m, n), device=x.device, dtype=torch.float32)
    _direct_gemm[(triton.cdiv(m, block_m), triton.cdiv(n, block_n), split_k)](
        x, w, target, m, n, k, x.stride(0), x.stride(1), w.stride(0),
        int(qtype), precision, block_m, block_n, block_k, split_k,
        num_warps=num_warps, num_stages=num_stages, enable_fp_fusion=False)
    if split_k != 1:
        _reduce_splits[(triton.cdiv(m*n, 256),)](target, y, m*n, split_k, 256)
    return y
