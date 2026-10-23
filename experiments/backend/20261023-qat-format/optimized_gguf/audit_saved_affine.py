"""Compare actual saved INT4 tensors with runtime-rounded affine coefficients.

This is a CPU coefficient audit. It does not run Marlin or pretend to include
its scale multiplication, grouped activation sum, GEMM and final-add rounding.
"""
import json
import os
from pathlib import Path

os.environ['CUDA_VISIBLE_DEVICES'] = ''
os.environ.setdefault('OMP_NUM_THREADS', '1')
os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')

import gguf
import numpy as np
from safetensors import safe_open

from exact_repack import bf16_round, relative_rms

ROOT = Path(__file__).resolve().parent
SOURCE = Path('/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-mtp-gguf/Gemma4-31B-QAT-Uncensored-HauhauCS-Balanced-Q4_K_M.gguf')
NATIVE = Path('/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-native-mixed-w4w8a16')


def main():
    index = json.loads((NATIVE/'model.safetensors.index.json').read_text())['weight_map']
    prior = json.loads((ROOT.parent/'export-fidelity-samples.json').read_text())
    source = {t.name:t for t in gguf.GGUFReader(str(SOURCE),'r').tensors}
    result = {'scope': 'CPU actual saved coefficient audit; not GPU or complete runtime rounding',
              'source': str(SOURCE), 'native': str(NATIVE), 'matrices': []}
    for item in prior['matrices']:
        if item['target_bits'] != 4:
            continue
        name = item['converted_tensor'].removesuffix('.weight')
        rows = item['sample_rows']
        arrays = {}
        for suffix in ('weight_packed','weight_scale','weight_zero_point'):
            key = name+'.'+suffix
            with safe_open(str(NATIVE/index[key]),framework='pt',device='cpu') as sf:
                slices = sf.get_slice(key)
                arrays[suffix] = np.concatenate([slices[r:r+1].float().numpy() if suffix != 'weight_packed'
                                                else slices[r:r+1].numpy() for r in rows],axis=0)
        packed = arrays['weight_packed'].view(np.uint32)
        codes = ((packed[:,:,None] >> (np.arange(8,dtype=np.uint32)*4))&15).reshape(len(rows),-1)
        scale,zp = arrays['weight_scale'],arrays['weight_zero_point']
        qs = codes.astype(np.float32).reshape(*scale.shape,32)-8
        receipt = ((qs-zp[:,:,None])*scale[:,:,None]).reshape(len(rows),-1)
        offset = bf16_round(-zp*scale)
        runtime_coeff = (qs*scale[:,:,None]+offset[:,:,None]).reshape(len(rows),-1)
        tensor = source[item['source_tensor']]
        reference = gguf.dequantize(np.ascontiguousarray(tensor.data[rows]),tensor.tensor_type)
        result['matrices'].append({
            'source_tensor': tensor.name, 'converted_tensor': name,
            'sample_values': int(reference.size),
            'receipt_relative_rms_to_source': relative_rms(receipt,reference),
            'runtime_offset_coeff_relative_rms_to_source': relative_rms(runtime_coeff,reference),
            'runtime_offset_coeff_relative_rms_to_receipt': relative_rms(runtime_coeff,receipt),
            'runtime_offset_changed_groups': int(np.count_nonzero(offset != -zp*scale)),
            'groups': int(offset.size),
        })
    result['summary'] = {
        'matrices': len(result['matrices']),
        'sample_values': sum(r['sample_values'] for r in result['matrices']),
        'runtime_offset_changed_groups': sum(r['runtime_offset_changed_groups'] for r in result['matrices']),
        'groups': sum(r['groups'] for r in result['matrices']),
        'max_additional_coefficient_relative_rms': max(r['runtime_offset_coeff_relative_rms_to_receipt'] for r in result['matrices']),
        'max_receipt_relative_rms_to_source': max(r['receipt_relative_rms_to_source'] for r in result['matrices']),
        'max_runtime_offset_coeff_relative_rms_to_source': max(r['runtime_offset_coeff_relative_rms_to_source'] for r in result['matrices']),
    }
    (ROOT/'saved-affine-results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['summary'],indent=2))


if __name__ == '__main__':
    main()
