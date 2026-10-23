"""CPU correctness audit against the independent installed GGUF decoder."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time

os.environ.setdefault('CUDA_VISIBLE_DEVICES', '')
os.environ.setdefault('OMP_NUM_THREADS', '1')
os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')

import gguf
import numpy as np

from exact_repack import (bf16_pair, bf16_round, decode, decode_direct_offsets,
                          relative_rms, repack, repacked_codes, unpack_q4k,
                          unpack_q6k)

DEFAULT_SOURCE = '/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-mtp-gguf/Gemma4-31B-QAT-Uncensored-HauhauCS-Balanced-Q4_K_M.gguf'


def check(data, qtype):
    reference = gguf.dequantize(data, getattr(gguf.GGMLQuantizationType, qtype))
    packed = repack(data, qtype)
    value = decode(packed)
    direct = decode_direct_offsets(data, qtype)
    # Check float bit patterns, not an approximate tolerance.
    np.testing.assert_array_equal(value.view(np.uint32), reference.view(np.uint32))
    np.testing.assert_array_equal(direct.view(np.uint32), reference.view(np.uint32))
    original_codes = (unpack_q4k(data) if qtype == 'Q4_K' else unpack_q6k(data))[0]
    np.testing.assert_array_equal(repacked_codes(packed), original_codes)
    hi, lo = bf16_pair(reference)
    return {
        'values': int(reference.size), 'source_bytes': int(data.nbytes),
        'repacked_bytes': int(packed.nbytes),
        'repacked_bits_per_value': 8 * packed.nbytes / reference.size,
        'fp32_repack_bit_mismatches': 0, 'direct_offset_bit_mismatches': 0,
        'code_mismatches': 0,
        'bf16_weight_relative_rms': relative_rms(hi, reference),
        'bf16_pair_relative_rms': relative_rms(hi + lo, reference),
        'bf16_pair_max_abs_error': float(np.max(np.abs(hi + lo - reference))),
    }


def synthetic(rng, qtype, nblocks=8192):
    width = 144 if qtype == 'Q4_K' else 210
    b = rng.integers(0, 256, (nblocks, width), dtype=np.uint8)
    # All integer codes/scales remain random, including signed Q6_K extremes.
    # Random finite half bit patterns cover both signs, subnormals and zeros.
    half = rng.integers(0, 65536, (nblocks, 2), dtype=np.uint16)
    half[(half & 0x7c00) == 0x7c00] &= np.uint16(0xfbff)
    half[:8, 0] = [0, 0x8000, 1, 0x8001, 0x7bff, 0xfbff, 0x0400, 0x8400]
    half[:8, 1] = half[:8, 0][::-1]
    if qtype == 'Q4_K':
        b[:, :4] = half.astype('<u2').view(np.uint8)
    else:
        b[:, 208:] = half[:, :1].copy().astype('<u2').view(np.uint8)
    return b.reshape(8, -1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', default=DEFAULT_SOURCE)
    ap.add_argument('--output', type=Path, default=Path(__file__).with_name('cpu-results.json'))
    ap.add_argument('--rows', type=int, default=8)
    args = ap.parse_args()
    rng = np.random.default_rng(20261023)
    started = time.monotonic()
    result = {'scope': 'CPU represented-weight audit; no GPU execution or end-to-end quality claim',
              'seed': 20261023, 'source': args.source, 'synthetic': [], 'tensors': []}
    for qtype in ('Q4_K', 'Q6_K'):
        result['synthetic'].append({'type': qtype, **check(synthetic(rng, qtype), qtype)})
    reader = gguf.GGUFReader(args.source, 'r')
    totals = {}
    for t in reader.tensors:
        qtype = t.tensor_type.name
        row = totals.setdefault(qtype, {'tensors': 0, 'values': 0, 'bytes': 0})
        row['tensors'] += 1
        row['values'] += int(t.n_elements)
        row['bytes'] += int(t.n_bytes)
        if qtype not in ('Q4_K', 'Q6_K'):
            continue
        n = t.data.shape[0]
        picks = np.unique(np.concatenate(([0, n//2, n-1],
                           rng.integers(0, n, max(0, args.rows-3)))))
        data = np.ascontiguousarray(t.data[picks])
        measured = check(data, qtype)
        if qtype == 'Q4_K':
            q, scale, minimum = unpack_q4k(data)
            s = bf16_round(scale)
            zp = bf16_round(minimum / np.where(s != 0, s, 1) - 8)
            zp = np.where(s != 0, zp, np.float32(0))
            qs = q.reshape(*scale.shape, 32).astype(np.float32) - 8
            source_w = decode(repack(data, qtype))
            receipt_w = ((qs - zp[..., None]) * s[..., None]).reshape(source_w.shape)
            offset = bf16_round(-zp * s)
            offset_w = (qs * s[..., None] + offset[..., None]).reshape(source_w.shape)
            measured['emulated_int4_export_receipt_relative_rms'] = relative_rms(receipt_w, source_w)
            measured['emulated_runtime_offset_coeff_relative_rms'] = relative_rms(offset_w, source_w)
            measured['runtime_offset_vs_receipt_relative_rms'] = relative_rms(offset_w, receipt_w)
            measured['offset_audit_scope'] = 'hypothetical retained-code INT4 for this source tensor; coefficient rounding only; excludes activation/group-sum/GEMM/add rounding'
        result['tensors'].append({'name': t.name, 'type': qtype, 'rows': picks.tolist(), **measured})
    result['source_totals'] = totals
    result['elapsed_seconds'] = time.monotonic() - started
    result['summary'] = {
        'real_tensors': len(result['tensors']),
        'real_values': sum(t['values'] for t in result['tensors']),
        'synthetic_values': sum(t['values'] for t in result['synthetic']),
        'fp32_repack_bit_mismatches': 0, 'direct_offset_bit_mismatches': 0,
        'code_mismatches': 0,
        'max_real_bf16_weight_relative_rms': max(t['bf16_weight_relative_rms'] for t in result['tensors']),
        'max_real_bf16_pair_relative_rms': max(t['bf16_pair_relative_rms'] for t in result['tensors']),
        'max_runtime_offset_vs_receipt_relative_rms': max(t.get('runtime_offset_vs_receipt_relative_rms', 0) for t in result['tensors']),
    }
    result['script_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in (Path(__file__), Path(__file__).with_name('exact_repack.py'))}
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result['summary'], indent=2))


if __name__ == '__main__':
    main()
