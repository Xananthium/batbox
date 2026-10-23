#!/usr/bin/env python3
"""CPU-only independent sample audit of the saved conversion against GGUF.

This measures weight representation and synthetic linear outputs, not full-model
quality or actual GPU-kernel error. It never imports vLLM or initializes CUDA.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path

import gguf
import numpy as np
from safetensors import safe_open
import torch


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def relative_rms(actual, expected):
    delta = actual.astype(np.float64) - expected.astype(np.float64)
    return float(np.sqrt(np.sum(delta * delta) /
                         max(np.sum(expected.astype(np.float64) ** 2), 1e-30)))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=Path('/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-mtp-gguf/Gemma4-31B-QAT-Uncensored-HauhauCS-Balanced-Q4_K_M.gguf'))
    p.add_argument('--converted', type=Path, default=Path('/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-native-mixed-w4w8a16'))
    p.add_argument('--output', type=Path, default=Path(__file__).with_name('export-fidelity-samples.json'))
    args = p.parse_args()
    torch.set_num_threads(2)
    reader = gguf.GGUFReader(str(args.source))
    sources = {t.name: t for t in reader.tensors}
    index_path = args.converted / 'model.safetensors.index.json'
    index = json.loads(index_path.read_text())['weight_map']
    receipts = json.loads((args.converted / 'export-progress.json').read_text())['tensor_receipts']
    mapping = {'attn_q': 'self_attn.q_proj', 'attn_k': 'self_attn.k_proj',
               'attn_v': 'self_attn.v_proj', 'attn_output': 'self_attn.o_proj',
               'ffn_down': 'mlp.down_proj', 'ffn_gate': 'mlp.gate_proj',
               'ffn_up': 'mlp.up_proj'}
    rng = np.random.default_rng(20261023)
    rows = []
    # Cover early/middle/late layers and the largest-error layer in the receipt.
    for layer in (0, 14, 28, 41, 59):
        for source_part, target_part in mapping.items():
            source_name = f'blk.{layer}.{source_part}.weight'
            if source_name not in sources:
                continue
            target = f'model.language_model.layers.{layer}.{target_part}.'
            tensor = sources[source_name]
            nrows, ncols = map(int, tensor.shape[::-1])
            selected = np.unique(np.concatenate([
                np.arange(4), np.arange(nrows // 2, nrows // 2 + 4),
                np.arange(nrows - 4, nrows), rng.integers(0, nrows, 20)]))
            raw = tensor.data[selected]
            original = gguf.dequantize(raw, tensor.tensor_type)
            bits = receipts[target + 'weight']['native_bits']

            def saved(suffix):
                key = target + suffix
                with safe_open(str(args.converted / index[key]), framework='pt', device='cpu') as f:
                    view = f.get_slice(key)
                    return torch.cat([view[int(i):int(i)+1] for i in selected]).float().numpy()

            # Read packed integer words without a float conversion.
            key = target + 'weight_packed'
            with safe_open(str(args.converted / index[key]), framework='pt', device='cpu') as f:
                view = f.get_slice(key)
                packed = torch.cat([view[int(i):int(i)+1] for i in selected]).numpy().astype(np.uint32)
            factor = 32 // bits
            codes = ((packed[..., None] >> (np.arange(factor, dtype=np.uint32) * bits))
                     & ((1 << bits) - 1)).reshape(len(selected), ncols)
            signed = codes.astype(np.float32) - 2 ** (bits - 1)
            scale = saved('weight_scale')
            zero = saved('weight_zero_point') if target + 'weight_zero_point' in index else 0.0
            represented = ((signed.reshape(len(selected), -1, 32)
                            - (zero[..., None] if isinstance(zero, np.ndarray) else zero))
                           * scale[..., None]).reshape(original.shape)
            code_mismatches = None
            if bits == 4 and tensor.tensor_type == gguf.GGMLQuantizationType.Q4_K:
                blocks = raw.reshape(-1, 144)
                packed_nibbles = blocks[:, 16:].reshape(-1, 4, 32)
                original_codes = np.stack((packed_nibbles & 15, packed_nibbles >> 4), axis=2).reshape(original.shape)
                code_mismatches = int(np.count_nonzero(codes != original_codes))
            activations = rng.standard_normal((16, ncols)).astype(np.float64)
            original_output = activations @ original.astype(np.float64).T
            converted_output = activations @ represented.astype(np.float64).T
            delta = represented - original
            rows.append({'source_tensor': source_name, 'converted_tensor': target + 'weight',
                         'source_type': tensor.tensor_type.name, 'target_bits': bits,
                         'sample_rows': selected.tolist(), 'sample_values': int(original.size),
                         'retained_code_mismatches': code_mismatches,
                         'weight_relative_rms': relative_rms(represented, original),
                         'weight_max_abs_error': float(np.abs(delta).max()),
                         'weight_mean_signed_error': float(delta.mean()),
                         'synthetic_linear_relative_rms': relative_rms(converted_output, original_output),
                         'reference': 'GGUF FP32 decoded source; CPU FP64 matmul for synthetic linear comparison'})
    result = {'utc': datetime.now(timezone.utc).isoformat(), 'seed': 20261023,
              'script_sha256': sha256(Path(__file__)), 'converted_index_sha256': sha256(index_path),
              'source': str(args.source), 'converted': str(args.converted),
              'scope': 'Sampled saved matrix rows, not full-model outputs, GPU kernels, or an accuracy score.',
              'summary': {'matrices': len(rows), 'sample_values': sum(r['sample_values'] for r in rows),
                          'types': dict(Counter(f"{r['source_type']}->INT{r['target_bits']}" for r in rows)),
                          'retained_code_mismatches': sum(r['retained_code_mismatches'] or 0 for r in rows),
                          'max_sample_weight_relative_rms': max(r['weight_relative_rms'] for r in rows),
                          'max_synthetic_linear_relative_rms': max(r['synthetic_linear_relative_rms'] for r in rows)},
              'matrices': rows}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['summary'], indent=2))
    if result['summary']['retained_code_mismatches']:
        raise SystemExit('Saved INT4 codes differ from source codes')


if __name__ == '__main__':
    main()
