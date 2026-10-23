"""Policy guards and first-match semantics, without loading a GPU runtime."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('isolated_policy',ROOT/'kernel_policy.py')
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
KernelPolicy = module.KernelPolicy


class TestPolicy(unittest.TestCase):
    def test_legacy_default(self):
        self.assertEqual(KernelPolicy().select(12,4096,5376,10752,'torch.bfloat16').op,'legacy')

    def test_exact_shape_and_batch_ranges(self):
        p=KernelPolicy('rules',[{'qtype':12,'n':5376,'k':10752,'min_m':128,'op':'direct'},
                                {'qtype':12,'min_m':3,'max_m':16,'op':'cuda_mmq'}])
        self.assertEqual(p.select(12,4096,5376,10752,'torch.bfloat16').op,'direct')
        self.assertEqual(p.select(12,4,5376,10752,'torch.bfloat16').op,'cuda_mmq')
        for args in [(12,1,5376,10752,'torch.bfloat16'),(14,4096,5376,10752,'torch.bfloat16'),
                     (12,4096,4096,10752,'torch.bfloat16'),(12,4096,5376,10752,'torch.float16'),
                     (12,0,5376,10752,'torch.bfloat16')]:
            self.assertEqual(p.select(*args).op,'legacy')

    def test_first_match_and_parameter_preservation(self):
        p=KernelPolicy('rules',[{'qtype':14,'op':'direct','precision':'tf32x3','split_k':4,'block_k':256},
                                {'op':'cuda_mmq'}])
        d=p.select(14,4,5376,10752,'torch.bfloat16')
        self.assertEqual((d.op,d.precision,d.split_k,d.block_k),('direct','tf32x3',4,256))
        self.assertNotIn('op',d.kwargs())

    def test_invalid_rules_rejected(self):
        for rule in ({'op':'magic'},{'qtype':99},{'split_k':3},{'block_m':8},
                     {'min_m':64,'max_m':1},{'precision':'tf32'},{'num_warps':3},
                     {'unknown':123},{'qtype':True}):
            with self.subTest(rule=rule),self.assertRaises(ValueError):
                KernelPolicy('rules',[rule])

    def test_trace_once_per_key(self):
        with tempfile.TemporaryDirectory() as d:
            p=KernelPolicy(trace_path=f'{d}/shapes')
            p.select(12,4,5376,10752,'torch.bfloat16')
            p.select(12,4,5376,10752,'torch.bfloat16')
            path=Path(f'{d}/shapes.{os.getpid()}.jsonl')
            rows=path.read_text().splitlines()
            self.assertEqual(len(rows),1)
            self.assertEqual(json.loads(rows[0])['op'],'legacy')


if __name__=='__main__':
    unittest.main()
