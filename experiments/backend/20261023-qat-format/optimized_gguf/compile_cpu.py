"""Compile to SM86 cubins without opening a CUDA context or running kernels."""
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent
os.environ['CUDA_VISIBLE_DEVICES'] = ''
os.environ.setdefault('TRITON_CACHE_DIR', str(ROOT / '.triton-cache'))

import triton
from triton.backends.compiler import GPUTarget
from triton.compiler import ASTSource

from direct_gemm import _direct_gemm


def main():
    results = []
    for qtype in (12, 14):
        for precision in ('bf16', 'bf16_pair', 'tf32x3'):
            const = dict(N=64, K=512, SXM=512, SXK=1,
                         SWN=2*(144 if qtype == 12 else 210), QTYPE=qtype,
                         PRECISION=precision, BLOCK_M=16, BLOCK_N=64, BLOCK_K=128,
                         SPLIT_K=1)
            signature = {'X': '*bf16', 'W': '*u8', 'Y': '*bf16', 'M': 'i32'}
            signature.update({k: 'constexpr' for k in const})
            kernel = triton.compile(ASTSource(_direct_gemm, signature=signature,
                                             constexprs=const),
                                    target=GPUTarget('cuda', 86, 32),
                                    options={'num_warps': 4, 'num_stages': 3,
                                             'enable_fp_fusion': False})
            ptx = kernel.asm['ptx']
            path = ROOT / f'direct-q{qtype}-{precision}-sm86.ptx'
            path.write_text(ptx)
            row = {'qtype': qtype, 'precision': precision, 'target': 'sm86',
                   'ptx_sha256': hashlib.sha256(ptx.encode()).hexdigest(),
                   'shared_memory_bytes': kernel.metadata.shared,
                   'mma_instructions': sorted({line.strip().split(' {')[0] for line in ptx.splitlines() if 'mma.sync' in line}),
                   'scope': 'offline compilation only; not executed'}
            if not row['mma_instructions']:
                raise AssertionError('candidate did not compile to tensor-core MMA')
            results.append(row)
            print(f"compiled q{qtype}/{precision}: {row['mma_instructions']}", flush=True)
    (ROOT / 'compile-results.json').write_text(json.dumps(results, indent=2)+'\n')


if __name__ == '__main__':
    main()
