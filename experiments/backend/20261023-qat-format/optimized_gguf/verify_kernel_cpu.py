"""Run the actual Triton address/masking code through its CPU interpreter.

IEEE FP32 dot mode isolates layout correctness. GPU BF16/TF32 numerics,
code-generation correctness and performance still require probe_gpu.py.
"""
import json
import os
from pathlib import Path

os.environ['CUDA_VISIBLE_DEVICES'] = ''
os.environ['TRITON_INTERPRET'] = '1'
os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')
os.environ.setdefault('OMP_NUM_THREADS', '1')

import gguf
import numpy as np
import torch
from direct_gemm import _direct_gemm, _reduce_splits
from exact_repack import bf16_round, relative_rms


def main():
    rng = np.random.default_rng(20261023)
    results = []
    for qt in (12, 14):
        width = 144 if qt == 12 else 210
        for m,n,k,bk,sk in ((1,1,256,128,1),(7,37,512,128,1),(17,65,768,256,1),
                            (7,37,768,128,4),(17,65,1024,256,8)):
            raw = rng.integers(0,256,(n,k//256*width),dtype=np.uint8)
            b = raw.reshape(-1,width)
            if qt == 12:
                b[:,:4] = rng.uniform(0.0001,0.003,(len(b),2)).astype('<f2').view(np.uint8)
            else:
                b[:,208:] = rng.uniform(0.0001,0.003,(len(b),1)).astype('<f2').view(np.uint8)
            # Both operands have nontrivial row stride; X also has stride2 K.
            xs = bf16_round(rng.standard_normal((m,k*2)).astype(np.float32))
            x = torch.from_numpy(xs)[:,::2]
            storage = torch.zeros((n,raw.shape[1]+17),dtype=torch.uint8)
            storage[:,:raw.shape[1]] = torch.from_numpy(raw)
            w = storage[:,:raw.shape[1]]
            y = torch.empty((m,n),dtype=torch.float32)
            target = y if sk==1 else torch.empty((sk,m,n),dtype=torch.float32)
            _direct_gemm[((m+15)//16,(n+63)//64,sk)](
                x,w,target,m,n,k,x.stride(0),x.stride(1),w.stride(0),
                qt,'ieee',16,64,bk,sk,num_warps=4,num_stages=3,enable_fp_fusion=False)
            if sk != 1:
                _reduce_splits[((m*n+255)//256,)](target,y,m*n,sk,256)
            ref = x.numpy().astype(np.float64) @ gguf.dequantize(raw,gguf.GGMLQuantizationType(qt)).astype(np.float64).T
            err = relative_rms(y.numpy(),ref)
            np.testing.assert_allclose(y.numpy(),ref,rtol=3e-5,atol=1e-4)
            assert err < 1e-6, err
            results.append({'qtype':qt,'shape':[m,n,k],'block_k':bk,'split_k':sk,
                            'strided_inputs':True,'relative_rms_to_fp64':err})
    out = {'scope':'actual Triton code run on CPU interpreter in IEEE mode; no GPU',
           'cases':results,'passed':len(results)}
    Path(__file__).with_name('kernel-cpu-results.json').write_text(json.dumps(out,indent=2)+'\n')
    print(json.dumps(out,indent=2))


if __name__ == '__main__':
    main()
