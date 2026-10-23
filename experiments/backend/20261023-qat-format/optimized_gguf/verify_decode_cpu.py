"""Execute the once-per-matrix decoder on the CPU Triton interpreter."""
import os
os.environ['CUDA_VISIBLE_DEVICES']=''
os.environ['TRITON_INTERPRET']='1'
os.environ.setdefault('OPENBLAS_NUM_THREADS','1')
import json
from pathlib import Path
import gguf
import numpy as np
import torch
from dequant_cublas import _decode

rng=np.random.default_rng(20261023)
results=[]
for qt,width in ((12,144),(14,210)):
    for n,k in ((1,256),(37,768)):
        raw=rng.integers(0,256,(n,k//256*width),dtype=np.uint8)
        b=raw.reshape(-1,width)
        if qt==12:
            b[:,:4]=rng.uniform(0.00001,1,(len(b),2)).astype('<f2').view(np.uint8)
        else:
            b[:,208:]=rng.uniform(0.00001,1,(len(b),1)).astype('<f2').view(np.uint8)
        w=torch.from_numpy(raw)
        hi=torch.empty((n,k),dtype=torch.float32)
        _decode[((n*k+511)//512,)](w,hi,hi,n,k,w.stride(0),qt,'fp32',512,num_warps=4,enable_fp_fusion=False)
        reference=gguf.dequantize(raw,gguf.GGMLQuantizationType(qt))
        np.testing.assert_array_equal(hi.numpy().view(np.uint32),reference.view(np.uint32))
        results.append({'qtype':qt,'shape':[n,k],'fp32_bit_mismatches':0})
Path(__file__).with_name('decode-cpu-results.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
