#!/usr/bin/env python3
"""CPU source audit of installed vLLM sampling; never imports vLLM or Triton.

Selected unmodified AST function bodies execute with a small Torch CPU
implementation of the Triton operations they use. This checks scalar/vector
semantics, not compiled CUDA behavior, concurrency, or model output quality.
"""
import argparse
import ast
import copy
import datetime
import hashlib
import inspect
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import torch


class Pointer:
    def __init__(self, tensor, offset=0):
        self.tensor = tensor.reshape(-1)
        self.offset = torch.as_tensor(offset, dtype=torch.int64)

    def __add__(self, offset):
        return Pointer(self.tensor, self.offset + offset)

    def __sub__(self, offset):
        return Pointer(self.tensor, self.offset - offset)


class CPUOps:
    """Only the operations used by the audited straight-line kernel branches."""
    int64, int32, int1, float32 = torch.int64, torch.int32, torch.bool, torch.float32
    ids = (0, 0)

    def program_id(self, axis):
        return torch.tensor(self.ids[axis], dtype=torch.int64)

    @staticmethod
    def load(ptr, mask=True, other=0):
        idx, mask = torch.broadcast_tensors(ptr.offset, torch.as_tensor(mask))
        valid = mask.bool()
        if bool(valid.any()):
            assert bool(((idx[valid] >= 0) & (idx[valid] < ptr.tensor.numel())).all())
        safe = torch.where(valid, idx, 0)
        return torch.where(valid, ptr.tensor[safe], torch.as_tensor(other, dtype=ptr.tensor.dtype))

    @staticmethod
    def store(ptr, value, mask=True):
        idx, value, mask = torch.broadcast_tensors(ptr.offset, torch.as_tensor(value), torch.as_tensor(mask))
        valid = mask.bool()
        ptr.tensor[idx[valid]] = value[valid].to(ptr.tensor.dtype)

    @staticmethod
    def where(cond, yes, no):
        return torch.where(torch.as_tensor(cond), torch.as_tensor(yes), torch.as_tensor(no))

    @staticmethod
    def maximum(a, b):
        return torch.maximum(torch.as_tensor(a), torch.as_tensor(b))

    arange = staticmethod(torch.arange)
    range = staticmethod(lambda n: range(int(n)))
    zeros = staticmethod(lambda shape, dtype: torch.zeros(shape, dtype=dtype))
    max = staticmethod(lambda value, axis=None: torch.amax(value, dim=axis))
    sum = staticmethod(lambda value, axis=None: torch.sum(value, dim=axis))
    argmax = staticmethod(lambda value, axis=0: torch.argmax(value, dim=axis))
    exp = staticmethod(lambda value: torch.exp(torch.as_tensor(value)))
    log = staticmethod(lambda value: torch.log(torch.as_tensor(value)))
    cdiv = staticmethod(lambda a, b: (a + b - 1) // b)


def extract(root, relative, symbol, namespace, records):
    raw = (root / relative).read_bytes()
    tree = ast.parse(raw)
    body = tree.body
    for name in symbol.split('.'):
        node = next(n for n in body if isinstance(n, (ast.FunctionDef, ast.ClassDef)) and n.name == name)
        body = node.body
    assert isinstance(node, ast.FunctionDef)
    node = copy.deepcopy(node)
    node.decorator_list = []
    node.returns = None
    for arg in (*node.args.posonlyargs, *node.args.args, *node.args.kwonlyargs):
        arg.annotation = None
    module = ast.Module(body=[node], type_ignores=[])
    exec(compile(ast.fix_missing_locations(module), str(root / relative), 'exec'), namespace)
    records.setdefault(relative, {'sha256': hashlib.sha256(raw).hexdigest(), 'symbols': {}})['symbols'][symbol] = node.lineno
    return namespace[node.name]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vllm-root', type=Path, required=True)
    parser.add_argument('--runtime-snapshot', type=Path, required=True)
    parser.add_argument('--runtime-manifest', type=Path, required=True)
    parser.add_argument('--historical-launch', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    torch.set_num_threads(1)
    assert torch.version.cuda is None, 'Use a CPU-only Torch environment.'
    tl = CPUOps()
    ns = {'torch': torch, 'np': np, 'tl': tl, 'LogitsContext': SimpleNamespace}
    records, checks = {}, []

    def get(relative, name):
        return extract(args.vllm_root, relative, name, ns, records)

    gpu = 'v1/worker/gpu/'
    penalty_kernel = get(gpu + 'sample/penalties.py', '_penalties_kernel')
    min_p_kernel = get(gpu + 'sample/min_p.py', '_min_p_kernel')
    temp_kernel = get(gpu + 'sample/gumbel.py', '_temperature_kernel')
    post_kernel = get(gpu + 'input_batch.py', '_post_update_kernel')
    pipeline = get(gpu + 'sample/sampler.py', 'Sampler.apply_sampling_params')
    topkp = get('v1/sample/ops/topk_topp_sampler.py', 'apply_top_k_top_p_pytorch')

    def check(name, condition, **details):
        assert bool(condition), name
        checks.append({'name': name, 'passed': True, **details})

    def penalties(logits, mapping, tokens, positions, prompt_masks, output_counts,
                  rep=(1.1,), freq=(0.0,), presence=(0.0,)):
        logits = logits.clone()
        vocab = logits.shape[1]
        rep, freq, presence = [torch.tensor(x, dtype=torch.float32) for x in (rep, freq, presence)]
        for row in range(len(logits)):
            for block in range((vocab + 31) // 32):
                tl.ids = (row, block)
                penalty_kernel(Pointer(logits), vocab, Pointer(mapping), Pointer(tokens), Pointer(positions),
                               Pointer(rep), Pointer(freq), Pointer(presence), Pointer(prompt_masks),
                               prompt_masks.shape[1], Pointer(output_counts), vocab, vocab, 32)
        return logits

    vocab = 64
    mapping = torch.zeros(3, dtype=torch.int64)
    # Row 0 consumes last committed token 3; rows 1 and 2 consume drafts 7,7.
    tokens = torch.tensor([3, 7, 7])
    positions = torch.tensor([0, 1, 2])
    prompt = torch.zeros((1, 2), dtype=torch.int32)
    prompt[0, 0] = 1 << 11
    counts = torch.zeros((1, vocab), dtype=torch.int32)
    counts[0, 3] = 1
    raw = torch.full((3, vocab), 2.0)
    observed = penalties(raw, mapping, tokens, positions, prompt, counts,
                         rep=(2.0,), freq=(0.25,), presence=(0.5,))
    expected_seven = torch.tensor([2.0, 0.25, 0.0])
    check('previous_speculative_tokens_included_without_double_counting_last_committed',
          torch.equal(observed[:, 7], expected_seven) and torch.equal(observed[:, 3], torch.full((3,), 0.25)),
          draft_token_logits=observed[:, 7].tolist(), last_committed_logits=observed[:, 3].tolist())
    check('prompt_tokens_receive_repetition_only', torch.equal(observed[:, 11], torch.ones(3)))
    check('verification_does_not_mutate_committed_counts', counts.sum() == 1)

    # A second request uses a different penalty and one draft; no batch bleed.
    batch_map = torch.tensor([0, 0, 0, 1, 1])
    batch_tokens = torch.tensor([3, 7, 7, 5, 9])
    batch_pos = torch.tensor([0, 1, 2, 0, 1])
    batch_counts = torch.zeros((2, vocab), dtype=torch.int32)
    batch_counts[0, 3] = batch_counts[1, 5] = 1
    batch_prompt = torch.zeros((2, 2), dtype=torch.int32)
    batch_logits = penalties(torch.full((5, vocab), 2.0), batch_map, batch_tokens,
                             batch_pos, batch_prompt, batch_counts, rep=(2., 4.),
                             freq=(.25, .1), presence=(.5, .2))
    check('mixed_request_histories_and_penalties_are_separate',
          torch.allclose(batch_logits[3:, 9], torch.tensor([2.0, .2])) and
          torch.equal(batch_logits[:3, 9], torch.full((3,), 2.0)))

    single_counts = torch.zeros_like(counts)
    single_counts[0, 7] = 1
    repeat_counts = single_counts * 62784
    args_one = (torch.full((1, vocab), 2.0), torch.tensor([0]), torch.tensor([7]),
                torch.tensor([0]), prompt)
    once = penalties(*args_one, single_counts)
    many = penalties(*args_one, repeat_counts)
    check('repetition_penalty_is_presence_based_not_count_escalating', torch.equal(once, many),
          occurrences_compared=[1, 62784], penalty=1.1, repeated_token_logit=float(many[0, 7]))

    committed_counts = counts.clone()
    emitted = torch.tensor([[7, 8, 9]])
    last = torch.zeros(1, dtype=torch.int64)
    total = torch.tensor([10])
    computed = torch.tensor([10])
    all_ids = torch.full((1, 20), -1, dtype=torch.int64)
    tl.ids = (0, 0)
    post_kernel(Pointer(torch.tensor([0])), Pointer(computed), Pointer(last),
                Pointer(committed_counts), vocab, Pointer(emitted), 3,
                Pointer(torch.tensor([2])), Pointer(torch.tensor([1])),
                Pointer(torch.tensor([0, 3])), Pointer(all_ids), 20, Pointer(total))
    check('post_update_commits_only_emitted_tokens_and_rolls_back_rejection',
          committed_counts[0, 7] == 1 and committed_counts[0, 8] == 1 and
          committed_counts[0, 9] == 0 and total[0] == 12 and computed[0] == 12,
          committed_tokens=all_ids[0, 10:12].tolist(), last_token=int(last[0]))

    calls = []
    class States:
        temperature = .6
        min_p = .05
        top_k = 64
        top_p = .9

        def apply_temperature(self, logits, mapping, _):
            calls.append('temperature')
            for row in range(len(logits)):
                tl.ids = (row, 0)
                temp_kernel(Pointer(logits), vocab, Pointer(mapping),
                            Pointer(torch.tensor([self.temperature])), vocab, vocab)

        def apply_min_p(self, logits, mapping, _):
            calls.append('min_p')
            for row in range(len(logits)):
                tl.ids = (row, 0)
                min_p_kernel(Pointer(logits), vocab, Pointer(mapping),
                             Pointer(torch.tensor([self.min_p])), vocab, 32)

        def apply_top_k_top_p(self, logits, mapping, _):
            calls.append('top_k_top_p')
            return topkp(logits, torch.full((len(logits),), self.top_k),
                         torch.full((len(logits),), self.top_p))

    class PenaltyProcessor:
        def apply(self, logits, ctx):
            calls.append('penalties')
            return penalties(logits, ctx.expanded_idx_mapping, ctx.input_ids,
                             ctx.expanded_local_pos, prompt, counts)

    class NoThinkingBudget:
        def apply(self, logits, ctx):
            calls.append('thinking_budget_noop')

    sampler = SimpleNamespace(needs_logits_processing=np.ones(1, dtype=bool),
                              logits_processors=[PenaltyProcessor()],
                              thinking_budget_state=NoThinkingBudget(), sampling_states=States())
    def run_pipeline(logits):
        return pipeline(sampler, logits, mapping, torch.tensor([0]), np.array([0]),
                        torch.tensor([10, 11, 12]), tokens, positions, np.array([13]))

    p = torch.tensor([.4] + [.6 / 63] * 63)
    filtered = run_pipeline((p.log() * .6).repeat(3, 1))
    check('v2_verification_and_bonus_all_apply_min_p',
          torch.equal(torch.isfinite(filtered).sum(1), torch.ones(3, dtype=torch.int64)),
          finite_tokens_per_row=torch.isfinite(filtered).sum(1).tolist(), pipeline_order=calls[:])

    # Isolate top-k and top-p with neutral penalties, temperature and min-p.
    sampler.logits_processors = []
    sampler.sampling_states.temperature = 1.
    sampler.sampling_states.min_p = 0.
    sampler.sampling_states.top_k = 2
    sampler.sampling_states.top_p = 1.
    filter_logits = torch.arange(vocab, dtype=torch.float32).repeat(3, 1)
    check('top_k_applies_to_every_expanded_row', (torch.isfinite(run_pipeline(filter_logits)).sum(1) == 2).all())
    sampler.sampling_states.top_k = 64
    sampler.sampling_states.top_p = .5
    check('top_p_applies_to_every_expanded_row', (torch.isfinite(run_pipeline(filter_logits)).sum(1) == 1).all())

    # Concrete synthetic loop-compatible distribution, not a Gemma measurement.
    sampler.logits_processors = [PenaltyProcessor()]
    sampler.sampling_states = States()
    loop_raw = torch.zeros((3, vocab))
    loop_raw[:, 7] = 10.
    loop_probs = run_pipeline(loop_raw).softmax(-1)
    check('production_like_filters_can_leave_repeated_token_as_only_choice',
          torch.equal(loop_probs[:, 7], torch.ones(3)),
          repeated_token_probability=loop_probs[:, 7].tolist(),
          synthetic_logits=True, measured_model_logits=False)

    relative = gpu + 'spec_decode/rejection_sampler_utils.py'
    for name in ('_compute_global_logsumexp', '_compute_global_target_argmax',
                 '_compute_global_logprobs_and_logsumexp'):
        get(relative, name)
    reject = get(relative, '_rejection_kernel')
    ns['tl_rand32'] = lambda seed, pos, includes_zero: torch.tensor(uniforms[int(pos)])

    def accepted_length(probability, uniform, temperature=.6):
        nonlocal uniforms
        uniforms = [uniform, uniform, uniform]
        dist = torch.full((3, vocab), (1-probability)/(vocab-1))
        dist[:, 7] = probability
        logits = dist.log()
        maxima = logits.max(-1).values[:, None]
        sums = torch.exp(logits - maxima).sum(-1)[:, None]
        argmax = logits.argmax(-1)[:, None]
        length = torch.zeros(1, dtype=torch.int64)
        arguments = {p: None for p in inspect.signature(reject).parameters}
        arguments.update(sampled_ptr=Pointer(torch.zeros((1, 3), dtype=torch.int64)), sampled_stride=3,
                         rejected_steps_ptr=Pointer(length), target_rejected_logsumexp_ptr=Pointer(torch.zeros(1)),
                         draft_rejected_logsumexp_ptr=Pointer(torch.zeros(1)), target_logits_ptr=Pointer(logits),
                         target_logits_stride=vocab, target_local_argmax_ptr=Pointer(argmax), target_local_argmax_stride=1,
                         target_local_max_ptr=Pointer(maxima), target_local_max_stride=1,
                         target_local_sumexp_ptr=Pointer(sums), target_local_sumexp_stride=1,
                         draft_sampled_ptr=Pointer(torch.tensor([3, 7, 7])),
                         cu_num_logits_ptr=Pointer(torch.tensor([0, 3])), idx_mapping_ptr=Pointer(torch.tensor([0])),
                         temp_ptr=Pointer(torch.tensor([temperature])), seed_ptr=Pointer(torch.tensor([0])),
                         pos_ptr=Pointer(torch.tensor([0, 1, 2])), vocab_num_blocks=1,
                         PADDED_VOCAB_NUM_BLOCKS=1, HAS_DRAFT_LOGITS=False,
                         SYNTHETIC_MODE=False, USE_BLOCK_VERIFICATION=False)
        tl.ids = (0, 0)
        reject(**arguments)
        return int(length[0])

    uniforms = []
    ratio_cases = []
    for prob in (0., .1, .4, .9, 1.):
        for uniform in (.05, .2, .5, .95):
            observed = accepted_length(prob, uniform)
            assert observed == (2 if prob > uniform else 0)
            ratio_cases.append({'target_probability': prob, 'uniform': uniform, 'accepted': observed})
    check('one_hot_repeated_drafts_obey_target_probability_ratio', True, cases=ratio_cases)
    check('greedy_verification_requires_target_argmax',
          accepted_length(0., .2, 0.) == 0 and accepted_length(.9, .2, 0.) == 2)

    # Describe both code paths, but do not confuse an inactive legacy omission
    # with a demonstrated problem in the captured production runner.
    legacy = 'v1/sample/rejection_sampler.py'
    legacy_constraints = get(legacy, 'apply_sampling_constraints')
    legacy_processors = get(legacy, 'RejectionSampler.apply_logits_processors')
    legacy_nodes = ast.parse((args.vllm_root / legacy).read_text())
    legacy_functions = [n for n in ast.walk(legacy_nodes) if isinstance(n, ast.FunctionDef)
                        and n.name in ('apply_sampling_constraints', 'apply_logits_processors')]
    legacy_text = '\n'.join(ast.unparse(n) for n in legacy_functions)
    check('legacy_target_path_has_no_min_p_processor_reference',
          'argmax_invariant' not in legacy_text.replace('non_argmax_invariant', '') and 'min_p' not in legacy_text)

    # Hash caller, configuration and protocol files read during the audit too.
    for relative in (
        gpu + 'spec_decode/rejection_sampler.py', gpu + 'spec_decode/speculator.py',
        gpu + 'spec_decode/gemma4/speculator.py', gpu + 'sample/states.py',
        gpu + 'sample/thinking_budget.py', gpu + 'model_runner.py',
        'config/speculative.py', 'entrypoints/anthropic/serving.py',
        'entrypoints/anthropic/protocol.py', 'entrypoints/openai/chat_completion/protocol.py',
    ):
        raw = (args.vllm_root / relative).read_bytes()
        records.setdefault(relative, {'sha256': hashlib.sha256(raw).hexdigest(), 'symbols': {}})

    runtime_raw = args.runtime_snapshot.read_bytes()
    runtime = json.loads(runtime_raw)
    launch_raw = args.historical_launch.read_bytes()
    launch = json.loads(launch_raw)
    manifest = json.loads(args.runtime_manifest.read_text())
    for relative, record in records.items():
        prior = manifest.get('vllm/' + relative)
        record['matches_captured_20261023_manifest'] = prior == record['sha256'] if prior else None
    argv = runtime['args']
    spec = json.loads(argv[argv.index('--speculative-config') + 1])
    history_environment = launch.get('environment', launch.get('env', {}))
    report = {
        'status': 'cpu_source_semantics_checked_not_cuda_or_incident_reproduction',
        'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'torch_version': torch.__version__, 'torch_cuda_build': torch.version.cuda,
        'gpu_calls': 0, 'vllm_imported': False, 'production_packages_modified': False,
        'cpu_threads': torch.get_num_threads(),
        'runtime_snapshot_sha256': hashlib.sha256(runtime_raw).hexdigest(),
        'historical_launch_sha256': hashlib.sha256(launch_raw).hexdigest(),
        'captured_v2_runner': runtime['environment'].get('VLLM_USE_V2_MODEL_RUNNER'),
        'historical_launch_v2_runner': history_environment.get('VLLM_USE_V2_MODEL_RUNNER'),
        'captured_speculative_config': spec,
        'historical_request_sampler_recovered': False,
        'oct22_exact_installed_sampling_source_verified': False,
        'confirmed_active_path_sampling_bug': False,
        'incident_cause_established': False,
        'checks': checks, 'source_files': records,
        'limitations': [
            'Triton operations emulated on CPU; no CUDA kernel compilation or launch.',
            'top_k/top_p use the installed PyTorch fallback, not the active GPU Triton/FlashInfer kernels.',
            'No model forward pass, logits capture, RNG quality test, batch race test, or exact incident replay.',
            'The synthetic singleton distribution demonstrates a possible loop mechanism, not measured Gemma behavior.',
            'Legacy min_p omission is not the captured active V2 path.',
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'checks_passed': len(checks),
                      'v2_runner': report['captured_v2_runner'],
                      'historical_v2_runner': report['historical_launch_v2_runner'],
                      'all_sources_match_captured_manifest': all(r['matches_captured_20261023_manifest'] for r in records.values())}))


if __name__ == '__main__':
    main()
