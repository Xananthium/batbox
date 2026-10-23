# SPDX-License-Identifier: Apache-2.0
"""Immutable experimental dispatch policy; default preserves legacy behavior.

Pure Python so policy validation can be tested without importing torch/vLLM.
Explicit policies are loaded once before CUDA graph capture. No autotuning,
tensor inspection or device synchronization occurs in the selection path.
"""
from dataclasses import dataclass
from functools import lru_cache
import json
import os
from pathlib import Path


@dataclass(frozen=True, slots=True)
class Decision:
    op: str = 'legacy'
    precision: str = 'bf16_pair'
    block_m: int = 16
    block_n: int = 64
    block_k: int = 128
    num_warps: int = 4
    num_stages: int = 3
    split_k: int = 1

    def kwargs(self):
        return {k: getattr(self, k) for k in self.__dataclass_fields__ if k != 'op'}


class KernelPolicy:
    def __init__(self, mode='legacy', rules=(), trace_path=None):
        self.mode = mode
        self.trace_path = trace_path
        if mode not in ('legacy', 'rules'):
            raise ValueError(f'unknown GGUF kernel policy mode: {mode}')
        self.rules = tuple(self._rule(r) for r in rules)

    @staticmethod
    def _rule(raw):
        raw = dict(raw)
        selectors = {'qtype', 'n', 'k', 'min_m', 'max_m'}
        choices = set(Decision.__dataclass_fields__)
        extra = set(raw) - selectors - choices - {'evidence'}
        if extra:
            raise ValueError(f'unknown GGUF rule keys: {sorted(extra)}')
        select = {k: raw[k] for k in selectors if k in raw}
        for k,v in select.items():
            if isinstance(v,bool) or not isinstance(v,int) or v < 1:
                raise ValueError(f'{k} must be a positive integer')
        if select.get('qtype') not in (None,12,14):
            raise ValueError('only Q4_K/Q6_K candidate rules are supported')
        if select.get('min_m',1)>select.get('max_m',2**31-1):
            raise ValueError('min_m exceeds max_m')
        decision = Decision(**{k:raw[k] for k in choices if k in raw})
        if decision.op not in ('legacy','cuda_mmvq','cuda_mmq','plugin_triton','direct','dequant_cublas','cuda_dequant_cublas'):
            raise ValueError(f'unknown GGUF kernel: {decision.op}')
        if decision.precision not in ('bf16','bf16_pair','tf32x3','ieee'):
            raise ValueError('unknown precision mode')
        if decision.op=='dequant_cublas' and decision.precision not in ('bf16','bf16_pair'):
            raise ValueError('cuBLAS policy supports explicit BF16 or BF16-pair precision')
        if decision.block_m not in (16,32,64) or decision.block_n not in (32,64,128,256):
            raise ValueError('unsupported tile M/N')
        if decision.block_k not in (32,64,128,256) or decision.num_warps not in (2,4,8):
            raise ValueError('unsupported tile K/warp count')
        if decision.num_stages not in (1,2,3,4,5) or decision.split_k not in (1,2,4,8,16):
            raise ValueError('unsupported stage/split count')
        return select,decision

    @classmethod
    def from_env(cls):
        value = os.environ.get('VLLM_GGUF_KERNEL_POLICY','legacy')
        trace = os.environ.get('VLLM_GGUF_TRACE_SHAPES')
        if value == 'legacy':
            return cls(trace_path=trace)
        raw = json.loads(Path(value).read_text())
        if raw.get('version') != 1 or not isinstance(raw.get('rules'),list):
            raise ValueError('GGUF policy must contain version1 and a rules array')
        return cls('rules',raw['rules'],trace)

    @lru_cache(maxsize=8192)
    def select(self,qtype,m,n,k,dtype):
        decision = Decision()
        if self.mode != 'legacy' and qtype in (12,14) and dtype == 'torch.bfloat16' and m > 0:
            for selectors, candidate in self.rules:
                if any(selectors.get(key,value) != value for key,value in (('qtype',qtype),('n',n),('k',k))):
                    continue
                if not selectors.get('min_m',1) <= m <= selectors.get('max_m',2**31-1):
                    continue
                decision = candidate
                break
        if self.trace_path:
            path = Path(f'{self.trace_path}.{os.getpid()}.jsonl')
            path.parent.mkdir(parents=True,exist_ok=True)
            with path.open('a') as f:
                f.write(json.dumps({'qtype':qtype,'m':m,'n':n,'k':k,'dtype':dtype,
                                    'op':decision.op,**decision.kwargs()})+'\n')
        return decision
