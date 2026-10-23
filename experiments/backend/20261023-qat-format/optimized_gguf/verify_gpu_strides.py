"""GPU regression for mixed-QKV padded rows and noncontiguous activations."""
import argparse,json
from pathlib import Path
import gguf,numpy as np,torch
from direct_gemm import direct_gemm
from dequant_cublas import dequant_cublas
from exact_repack import relative_rms

p=argparse.ArgumentParser();p.add_argument('--run-gpu-probe',action='store_true',required=True);p.parse_args()
torch.manual_seed(20261023);torch.set_num_threads(1)
source='/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-mtp-gguf/Gemma4-31B-QAT-Uncensored-HauhauCS-Balanced-Q4_K_M.gguf'
reader=gguf.GGUFReader(source,'r');rows=[]
for name in ('blk.0.ffn_down.weight','blk.10.ffn_down.weight'):
    t=next(t for t in reader.tensors if t.name==name);qt=int(t.tensor_type);width=210 if qt==14 else 144
    raw=np.ascontiguousarray(t.data[:129,:3*width]);n,k=129,768
    storage=torch.zeros((2*n+1,len(raw[0])+17),device='cuda',dtype=torch.uint8)
    w=storage[1:1+2*n:2,:len(raw[0])];w.copy_(torch.from_numpy(raw))
    x=torch.randn((33,k*2),device='cuda',dtype=torch.bfloat16)[:,1::2]
    ref=x.float().cpu().numpy().astype(np.float64)@gguf.dequantize(raw,t.tensor_type).astype(np.float64).T
    for mode in ('bf16','bf16_pair'):
        for method in ('direct','dequant_cublas'):
            y=(direct_gemm(w,x,qt,precision=mode,split_k=8) if method=='direct'
               else dequant_cublas(w,x,qt,precision=mode))
            error=relative_rms(y.float().cpu().numpy(),ref)
            assert error < (0.006 if mode=='bf16' else 0.003),error
            rows.append({'name':name,'method':method,'precision':mode,'w_stride':list(w.stride()),
                         'x_stride':list(x.stride()),'nonzero_storage_offsets':True,'relative_rms':error})
Path(__file__).with_name('gpu-stride-results.json').write_text(json.dumps(rows,indent=2)+'\n')
print(json.dumps(rows,indent=2))
