"""Audit actual GPU decoding on samples from every quantized source tensor."""
import argparse
import json
from pathlib import Path
import time
import gguf
import numpy as np
import torch
from dequant_cublas import decode_weights
from exact_repack import relative_rms

ROOT=Path(__file__).resolve().parent
SOURCE='/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-mtp-gguf/Gemma4-31B-QAT-Uncensored-HauhauCS-Balanced-Q4_K_M.gguf'


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--run-gpu-probe',action='store_true',required=True)
    a=p.parse_args()
    from vllm_gguf_plugin import ops
    torch.set_num_threads(1)
    started=time.monotonic()
    reader=gguf.GGUFReader(SOURCE,'r')
    rows=[]
    rng=np.random.default_rng(20261023)
    for t in reader.tensors:
        if int(t.tensor_type) not in (12,14):continue
        chosen=np.unique([0,t.data.shape[0]//2,t.data.shape[0]-1,int(rng.integers(t.data.shape[0]))])
        raw=np.ascontiguousarray(t.data[chosen])
        ref=gguf.dequantize(raw,t.tensor_type)
        w=torch.from_numpy(raw).cuda()
        fp32,_=decode_weights(w,ref.shape[1],int(t.tensor_type),'fp32')
        got=fp32.cpu().numpy()
        mismatch=int(np.count_nonzero(got.view(np.uint32)!=ref.view(np.uint32)))
        if mismatch:raise AssertionError((t.name,'FP32',mismatch))
        bf16,_=decode_weights(w,ref.shape[1],int(t.tensor_type),'bf16')
        ref_bf16=torch.from_numpy(ref).to(torch.bfloat16)
        mismatch_bf16=int(torch.count_nonzero(bf16.cpu().view(torch.int16)!=ref_bf16.view(torch.int16)))
        if mismatch_bf16:raise AssertionError((t.name,'BF16',mismatch_bf16))
        legacy=ops.ggml_dequantize(w,int(t.tensor_type),len(chosen),ref.shape[1],torch.float32).cpu().numpy()
        rows.append({'name':t.name,'qtype':int(t.tensor_type),'values':int(ref.size),
                     'fp32_bit_mismatches':mismatch,'bf16_bit_mismatches':mismatch_bf16,
                     'legacy_fp32_relative_rms':relative_rms(legacy,ref),
                     'legacy_fp32_bit_mismatches':int(np.count_nonzero(legacy.view(np.uint32)!=ref.view(np.uint32)))})
    result={'device':torch.cuda.get_device_name(),'source':SOURCE,'scope':'GPU decode correctness; sampled rows from every quantized tensor',
            'rows':rows,'summary':{'tensors':len(rows),'values':sum(r['values'] for r in rows),
            'fp32_bit_mismatches':sum(r['fp32_bit_mismatches'] for r in rows),
            'bf16_bit_mismatches':sum(r['bf16_bit_mismatches'] for r in rows),
            'legacy_fp32_bit_mismatches':sum(r['legacy_fp32_bit_mismatches'] for r in rows),
            'max_legacy_fp32_relative_rms':max(r['legacy_fp32_relative_rms'] for r in rows),
            'elapsed_seconds':time.monotonic()-started}}
    (ROOT/'gpu-decode-audit.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['summary'],indent=2))


if __name__=='__main__':main()
