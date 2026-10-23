"""Decode original weights once per GEMM, then use cuBLAS Tensor Cores.

For prefill, amortize decoding over all M rows rather than repeating it in
every M tile. Temporary weights are released each call. No saved conversion.
"""
import torch
import triton
import triton.language as tl


@triton.jit
def _half(W, offset, valid):
    lo=tl.load(W+offset,mask=valid,other=0).to(tl.uint16)
    hi=tl.load(W+offset+1,mask=valid,other=0).to(tl.uint16)
    return (lo|(hi<<8)).to(tl.float16,bitcast=True).to(tl.float32)


@triton.jit
def _decode(W, Hi, Lo, N:tl.constexpr,K:tl.constexpr,STRIDE:tl.constexpr,
            QTYPE:tl.constexpr,PRECISION:tl.constexpr,BLOCK:tl.constexpr):
    z=tl.program_id(0)*BLOCK+tl.arange(0,BLOCK)
    valid=z<N*K
    row,k=z//K,z%K
    pos=k%256
    if QTYPE==12:
        base=row*STRIDE+(k//256)*144
        g=pos//32
        a=tl.load(W+base+4+tl.where(g<4,g,g-4),mask=valid,other=0)
        b=tl.load(W+base+4+tl.where(g<4,g+4,g),mask=valid,other=0)
        c=tl.load(W+base+4+tl.where(g<4,g,g+4),mask=valid,other=0)
        sc=tl.where(g<4,a&63,(c&15)|((a>>6)<<4))
        mn=tl.where(g<4,b&63,(c>>4)|((b>>6)<<4))
        q=tl.load(W+base+16+(pos//64)*32+pos%32,mask=valid,other=0)
        q=(q>>((g%2)*4))&15
        value=(_half(W,base,valid)*sc.to(tl.float32))*q.to(tl.float32)-_half(W,base+2,valid)*mn.to(tl.float32)
    else:
        base=row*STRIDE+(k//256)*210
        h,g,lane=pos//128,(pos%128)//32,pos%32
        low=tl.load(W+base+h*64+(g%2)*32+lane,mask=valid,other=0)
        high=tl.load(W+base+128+h*32+lane,mask=valid,other=0)
        q=((low>>(4*(g//2)))&15)|(((high>>(2*g))&3)<<4)
        sc=tl.load(W+base+192+pos//16,mask=valid,other=0).to(tl.int8,bitcast=True)
        value=(_half(W,base+208,valid)*sc.to(tl.float32))*(q.to(tl.float32)-32.0)
    if PRECISION=='bf16_pair':
        hi=value.to(tl.bfloat16)
        lo=(value-hi.to(tl.float32)).to(tl.bfloat16)
        tl.store(Hi+z,hi,mask=valid)
        tl.store(Lo+z,lo,mask=valid)
    else:
        tl.store(Hi+z,value,mask=valid)


def decode_weights(w,k,qtype,precision='bf16'):
    if int(qtype) not in (12,14) or precision not in ('bf16','bf16_pair','fp32'):
        raise ValueError('unsupported type/precision')
    width=144 if int(qtype)==12 else 210
    if w.dtype!=torch.uint8 or w.ndim!=2 or w.stride(1)!=1 or k%256 or w.shape[1]!=k//256*width:
        raise ValueError('invalid GGUF block layout')
    hi=torch.empty((w.shape[0],k),device=w.device,dtype=torch.float32 if precision=='fp32' else torch.bfloat16)
    lo=torch.empty_like(hi) if precision=='bf16_pair' else hi
    _decode[(triton.cdiv(w.shape[0]*k,512),)](w,hi,lo,w.shape[0],k,w.stride(0),int(qtype),precision,512,
                                           num_warps=4,enable_fp_fusion=False)
    return hi,lo


def dequant_cublas(w,x,qtype,precision='bf16',**unused):
    if x.dtype!=torch.bfloat16 or x.ndim!=2:
        raise ValueError('expected BF16 X[M,K]')
    hi,lo=decode_weights(w,x.shape[1],qtype,precision)
    if precision=='bf16':
        return torch.mm(x,hi.T)
    if precision=='bf16_pair':
        # Preserve FP32 partial outputs; round only the final combined result.
        y=torch.mm(x,hi.T,out_dtype=torch.float32)
        y.add_(torch.mm(x,lo.T,out_dtype=torch.float32))
        return y.to(torch.bfloat16)
    # This reference arm requires the caller to choose/check torch's FP32
    # precision policy; production dispatch policies use the explicit BF16 arms.
    return torch.mm(x.float(),hi.T).to(torch.bfloat16)
