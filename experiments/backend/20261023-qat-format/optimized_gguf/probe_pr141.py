"""One upstream PR141 comparison; no custom-kernel policy or model writes."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess

os.environ.setdefault('OMP_NUM_THREADS', '1')
os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')
import gguf
import numpy as np
import torch
from vllm_gguf_plugin import ops
from vllm_gguf_plugin.kernel_support import upstream_storage_padding_bytes

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'pr141-kernel-results.json'
SOURCE = Path('/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-mtp-gguf/Gemma4-31B-QAT-Uncensored-HauhauCS-Balanced-Q4_K_M.gguf')


def padded_weight(raw, qt):
    storage = torch.zeros(raw.size + upstream_storage_padding_bytes(qt, raw.shape[1]),
                          dtype=torch.uint8, device='cuda')
    w = storage[:raw.size].view(raw.shape)
    w.copy_(torch.from_numpy(np.ascontiguousarray(raw)))
    return w


def run_case(raw, qt, name, m, all_outputs=False):
    raw = np.ascontiguousarray(raw)
    block, size = gguf.GGML_QUANT_SIZES[gguf.GGMLQuantizationType(qt)]
    k = raw.shape[1] // size * block
    w = padded_weight(raw, qt)
    x = torch.randn(m, k, device='cuda', dtype=torch.bfloat16)
    rows = np.arange(m) if all_outputs else np.unique([0, m//4, m//2, 3*m//4, m-1])
    cols = np.arange(len(raw)) if all_outputs else np.unique(np.linspace(0, len(raw)-1, 65, dtype=int))
    ref = (x[rows].float().cpu().numpy().astype(np.float64)
           @ gguf.dequantize(np.ascontiguousarray(raw[cols]), gguf.GGMLQuantizationType(qt)).astype(np.float64).T)
    selected = ops.dense_select_method(w, x, qt, len(raw))
    for _ in range(3):
        ops.ggml_dense(w, x, qt, len(raw))
    torch.cuda.synchronize()
    before = torch.cuda.memory_allocated()
    torch.cuda.reset_peak_memory_stats()
    graph = torch.cuda.CUDAGraph()
    with torch.cuda.graph(graph):
        y = ops.ggml_dense(w, x, qt, len(raw))
    start, end = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
    start.record()
    for _ in range(20):
        graph.replay()
    end.record()
    end.synchronize()
    actual = y[rows][:, cols].float().cpu().numpy()
    error = float(np.linalg.norm(actual.astype(np.float64)-ref) / np.linalg.norm(ref))
    finite = bool(torch.isfinite(y).all().item())
    # Q8_1 activation quantization is part of this upstream path. This is a
    # numerical smoke gate, not a claim of bit-exact FP32 multiplication.
    result = dict(name=name, qtype=qt, shape=[m, len(raw), k], method=selected.name,
                  milliseconds=start.elapsed_time(end)/20, graph_replays=20,
                  reference_input_rows=rows.tolist(), reference_output_columns=cols.tolist(),
                  reference='CPU FP64 GEMM of original FP32 GGUF-decoded weights and exact BF16 inputs',
                  relative_rms_to_reference=error, finite_all_outputs=finite,
                  numerical_smoke_limit=0.02, passed=finite and error < 0.02,
                  output_dtype=str(y.dtype), allocated_before_capture=before,
                  peak_allocated_in_capture_and_replay=torch.cuda.max_memory_allocated())
    return result


def main():
    torch.set_num_threads(1)
    torch.manual_seed(20261023)
    torch.cuda.set_device(0)
    if OUT.exists():
        raise FileExistsError(OUT)
    plugin = ROOT/'upstream-pr141'
    source_files = [p for p in plugin.glob('vllm_gguf_plugin/_C*.so')]
    source_files += [plugin/'vllm_gguf_plugin/ops.py', plugin/'vllm_gguf_plugin/quantization/linear.py', Path(__file__)]
    state = dict(utc_started=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                 scope='PR141 upstream automatic kernel dispatch; standalone graph timings, not serving',
                 device=torch.cuda.get_device_name(), torch=torch.__version__, cuda=torch.version.cuda,
                 plugin_commit=subprocess.check_output(['git','-C',str(plugin),'rev-parse','HEAD'],text=True).strip(),
                 sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in source_files},
                 float32_matmul_precision=torch.get_float32_matmul_precision(),
                 allow_bf16_reduced_precision_reduction=torch.backends.cuda.matmul.allow_bf16_reduced_precision_reduction,
                 allow_tf32=torch.backends.cuda.matmul.allow_tf32,
                 shared_host_conditions='Exclusive GPUs; independent CPU build/research may remain active',rows=[])
    reader = gguf.GGUFReader(str(SOURCE), 'r')
    selected = {t.name:t for t in reader.tensors if t.name in {'blk.0.ffn_down.weight','blk.10.ffn_down.weight','token_embd.weight'}}
    for name in ('blk.0.ffn_down.weight','blk.10.ffn_down.weight','token_embd.weight'):
        t = selected[name];qt=int(t.tensor_type)
        raw = t.data[:t.data.shape[0]//2] if name=='token_embd.weight' else t.data[:,:t.data.shape[1]//2]
        if name != 'token_embd.weight':
            size = gguf.GGML_QUANT_SIZES[t.tensor_type][1]
            cases = [(raw[:129,:size*3],33,True),(raw,4,False),(raw,16,False),(raw,4096,False)]
        else:
            cases = [(raw,4,False),(raw,16,False)]
        for packed,m,all_outputs in cases:
            row=run_case(packed,qt,name,m,all_outputs)
            state['rows'].append(row)
            OUT.write_text(json.dumps(state,indent=2)+'\n')
            print(json.dumps(row),flush=True)
            if not row['passed']:
                raise AssertionError(row)
    state['utc_finished']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    OUT.write_text(json.dumps(state,indent=2)+'\n')


if __name__=='__main__':
    main()
