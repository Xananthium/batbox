"""Standalone, opt-in GPU probe. Run only in the backend owner's test window.

No server requests, service operations, model writes or package patches.
Uses the existing plugin for baselines and the original checkpoint for all
candidate inputs. Small CPU FP64 reference slices gate candidate correctness.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time

ROOT = Path(__file__).resolve().parent
os.environ.setdefault('TRITON_CACHE_DIR', str(ROOT / '.triton-cache'))
os.environ.setdefault('OMP_NUM_THREADS', '1')
os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')

import gguf
import numpy as np
import torch

from direct_gemm import direct_gemm
from exact_repack import relative_rms

SOURCE = '/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-mtp-gguf/Gemma4-31B-QAT-Uncensored-HauhauCS-Balanced-Q4_K_M.gguf'
SHARDS = {'blk.0.ffn_gate.weight': 'output', 'blk.0.ffn_down.weight': 'input',
          'blk.10.ffn_down.weight': 'input', 'blk.10.attn_q.weight': 'output'}


def duration(fn, repeats):
    for _ in range(3):
        fn()
    torch.cuda.synchronize()
    graph = torch.cuda.CUDAGraph()
    with torch.cuda.graph(graph):
        y = fn()
    start, end = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
    start.record()
    for _ in range(repeats):
        graph.replay()
    end.record()
    end.synchronize()
    return start.elapsed_time(end)/repeats, y


def reference_input_rows(m):
    if m <= 8:
        return np.arange(m)
    return np.unique(np.array([0,1,7,m//4,m//2,3*m//4,m-2,m-1]))


def reference(x, raw, qtype, rows):
    a = x[reference_input_rows(len(x))].cpu().float().numpy().astype(np.float64)
    w = gguf.dequantize(np.ascontiguousarray(raw[rows]), qtype).astype(np.float64)
    return a @ w.T


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--run-gpu-probe', action='store_true', required=True)
    p.add_argument('--source', default=SOURCE)
    p.add_argument('--device', default='cuda:0')
    p.add_argument('--tensors', default=','.join(SHARDS))
    p.add_argument('--batches', default='1,4,12,16,256,4096')
    p.add_argument('--repeats', type=int, default=20)
    p.add_argument('--sweep', action='store_true', help='include split-K and extra tiles for tuning')
    p.add_argument('--cublas', action='store_true', help='include decode-once plus cuBLAS candidates')
    p.add_argument('--output', type=Path, default=ROOT/'gpu-results.json')
    a = p.parse_args()
    from vllm_gguf_plugin import ops
    from vllm_gguf_plugin.triton.gemm.interface import ggml_mul_mat_a8_triton
    if a.cublas:
        from dequant_cublas import dequant_cublas
    torch.manual_seed(20261023)
    torch.set_num_threads(1)
    torch.cuda.set_device(a.device)
    reader = gguf.GGUFReader(a.source, 'r')
    tensors = {t.name: t for t in reader.tensors if t.name in a.tensors.split(',')}
    assert set(tensors) == set(a.tensors.split(',')), 'unknown tensor requested'
    result = {'scope': 'standalone kernel probe; not end-to-end serving',
              'source': a.source, 'device': torch.cuda.get_device_name(),
              'utc_unix_started': time.time(), 'cuda_baseline_enabled': ops._CUDA_ENABLED,
              'candidate_sha256': hashlib.sha256((ROOT/'direct_gemm.py').read_bytes()).hexdigest(),
              'torch_float32_matmul_precision': torch.get_float32_matmul_precision(),
              'shared_host_conditions': 'exclusive GPU window; other agents may run CPU-only graph recovery and tiny-model experiments',
              'small_checks': [], 'rows': []}
    a.output.write_text(json.dumps(result, indent=2)+'\n')
    for name, t in tensors.items():
        raw = t.data
        if SHARDS[name] == 'output':
            raw = raw[:raw.shape[0]//2]
        else:
            raw = raw[:, :raw.shape[1]//2]
        raw = np.ascontiguousarray(raw)
        qt = int(t.tensor_type)
        block_bytes = 144 if qt == 12 else 210
        k = raw.shape[1]//block_bytes*256
        # Odd N/M and a short K exercise masks, two superblocks, all subgroups.
        small_raw = np.ascontiguousarray(raw[:37, :2*block_bytes])
        small_w = torch.from_numpy(small_raw).to(a.device)
        sx = torch.randn((7, 512), device=a.device, dtype=torch.bfloat16)
        sr = reference(sx, small_raw, t.tensor_type, np.arange(37))
        for mode in ('bf16', 'bf16_pair', 'tf32x3'):
            sy = direct_gemm(small_w, sx, qt, precision=mode).float().cpu().numpy()
            err = relative_rms(sy, sr)
            limit = 0.006 if mode == 'bf16' else 0.003
            result['small_checks'].append({'name': name, 'precision': mode,
                                           'relative_rms_to_cpu_fp64': err, 'limit': limit})
            a.output.write_text(json.dumps(result, indent=2)+'\n')
            if not np.isfinite(sy).all() or err > limit:
                raise AssertionError(f'{name}/{mode} correctness failed: {err} > {limit}')
        if a.cublas:
            for mode in ('bf16','bf16_pair','fp32'):
                sy=dequant_cublas(small_w,sx,qt,precision=mode).float().cpu().numpy()
                err=relative_rms(sy,sr)
                limit=0.006 if mode=='bf16' else 0.003
                result['small_checks'].append({'name':name,'precision':'cublas_'+mode,
                                              'relative_rms_to_cpu_fp64':err,'limit':limit})
                a.output.write_text(json.dumps(result,indent=2)+'\n')
                if not np.isfinite(sy).all() or err>limit:
                    raise AssertionError(f'{name}/cublas_{mode}: {err} > {limit}')
        del small_w, sx
        w = torch.from_numpy(raw).to(a.device)
        rng = np.random.default_rng(20261023)
        selected = np.unique(np.concatenate(([0, len(raw)-1], rng.integers(0, len(raw), 62))))
        for m in map(int, a.batches.split(',')):
            x = torch.randn((m, k), device=a.device, dtype=torch.bfloat16)
            ref = reference(x, raw, t.tensor_type, selected)
            choices = [('cuda_mmq', lambda: ops.ggml_mul_mat_a8(w, x, qt, len(raw))),
                       ('plugin_triton', lambda: ggml_mul_mat_a8_triton(w, x, qt, len(raw)))]
            if m <= 16:
                choices.insert(0, ('cuda_mmvq', lambda: ops.ggml_mul_mat_vec_a8(w, x, qt, len(raw))))
            for bm, bn, bk, nw in ((16,64,128,4), (32,64,128,4), (16,128,128,4), (16,64,256,4)):
                choices.append((f'direct_bf16_{bm}_{bn}_{bk}',
                                lambda bm=bm,bn=bn,bk=bk,nw=nw: direct_gemm(
                                    w,x,qt,block_m=bm,block_n=bn,block_k=bk,num_warps=nw)))
            for mode in ('bf16_pair', 'tf32x3'):
                choices.append((f'direct_{mode}', lambda mode=mode: direct_gemm(w,x,qt,precision=mode)))
            if a.sweep:
                for mode in ('bf16','bf16_pair','tf32x3'):
                    for sk in ((2,4,8,16) if m<=48 else (2,4)):
                        choices.append((f'direct_{mode}_split{sk}',lambda mode=mode,sk=sk:
                                        direct_gemm(w,x,qt,precision=mode,split_k=sk)))
                    for bm,bn,bk,nw in ((16,32,128,4),(32,128,128,4),(32,128,256,8),(64,64,128,4)):
                        choices.append((f'direct_{mode}_{bm}_{bn}_{bk}_w{nw}',
                                        lambda mode=mode,bm=bm,bn=bn,bk=bk,nw=nw: direct_gemm(
                                            w,x,qt,precision=mode,block_m=bm,block_n=bn,block_k=bk,num_warps=nw)))
            if a.cublas:
                choices.append(('cuda_dequant_cublas_bf16',lambda: torch.mm(
                    x,ops.ggml_dequantize(w,qt,len(raw),k,torch.bfloat16).T)))
                for mode in ('bf16','bf16_pair','fp32'):
                    choices.append((f'dequant_cublas_{mode}',lambda mode=mode:
                                    dequant_cublas(w,x,qt,precision=mode)))
            for label, fn in choices:
                row = {'name': name, 'qtype': qt, 'shape': [m, len(raw), k], 'kernel': label,
                       'reference_input_rows': reference_input_rows(m).tolist()}
                try:
                    ms, y = duration(fn, a.repeats)
                    subset = y[reference_input_rows(m)][:,selected].float().cpu().numpy()
                    row.update(milliseconds=ms, relative_rms_to_cpu_fp64=relative_rms(subset, ref),
                               max_abs_error_to_cpu_fp64=float(np.max(np.abs(subset-ref))),
                               finite=bool(np.isfinite(subset).all()))
                    bf16_mode=(label.startswith('direct_bf16_') and 'pair' not in label) or label.endswith('cublas_bf16')
                    row['correctness_gate'] = row['finite'] and row['relative_rms_to_cpu_fp64'] < (0.006 if bf16_mode else 0.003)
                    if not (label.startswith('direct_') or 'cublas' in label):
                        # Baselines are evidence, not a required reference implementation.
                        row.pop('correctness_gate')
                    del y
                except Exception as e:
                    row['error'] = repr(e)
                result['rows'].append(row)
                a.output.write_text(json.dumps(result, indent=2)+'\n')
                print(json.dumps(row), flush=True)
            del x
        del w, raw
    result['utc_unix_finished'] = time.time()
    a.output.write_text(json.dumps(result, indent=2)+'\n')


if __name__ == '__main__':
    main()
