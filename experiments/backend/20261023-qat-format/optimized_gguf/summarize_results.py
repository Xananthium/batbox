"""Summarize retained synthetic benchmark evidence, excluding private replays."""
import datetime
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parent


def load(path):
    return json.loads(path.read_text())


def record(short,longs,behaviors=None):
    flat=[q for row in longs for q in row.get('requests',[row])]
    return {'performance':short['performance_summary'],
            'short_quality':[short['quality_passes'],short['quality_total']],
            'long_exact_answers':[sum(q['assessment']['passed'] for q in flat),len(flat)],
            'cold_long_first_output_seconds':longs[0]['first_output_seconds'],
            'cold_prompt_tokens':longs[0]['usage']['prompt_tokens'],
            'shared_batch_seconds':longs[1]['wall_seconds'],
            'shared_batch_metrics':longs[1]['metrics_delta'],
            'behavior_checks':None if behaviors is None else [sum(r['passed'] for r in behaviors['records']),len(behaviors['records'])]}


rows={}
for name in ('converted','original'):
    data=load(ROOT.parent/(name+'_primary.json'))
    rows[name]=record(data,data['long_context'],load(ROOT.parent/(name+'_primary-behaviors.json')))
custom=load(ROOT/'whole-model-fast-v1/optimized_fast_v1.json')
rows['custom_fast_v1']=record(custom,custom['long_context'])
short=load(ROOT/'whole-model-pr141/upstream_pr141.json')
long=load(ROOT/'whole-model-pr141/upstream_pr141_long.json')
rows['upstream_pr141']=record(short,long['long_context'],load(ROOT/'whole-model-pr141/upstream_pr141-behaviors.json'))
release=load(ROOT/'durable-release.json')
verify=load(ROOT/'promoted-release-verification.json') if (ROOT/'promoted-release-verification.json').exists() else {}
result={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'rows':rows,
        'source_pins':{'plugin':'e62877c30519305f3c82dc0d616cf62b5ed67133','llama_cpp':'002a12ad25503a93501b2e188c360029830a241a','model':'9654466e82d83f5ebfe1518a369bc5900873abb1'},
        'extension_sha256':release['extension_sha256'],
        'release':release['path'],'rollback':release['rollback'],
        'persistent_activation_verified':bool(verify.get('passed')),
        'sampling':short['sampling'],
        'preserved_settings':{'max_model_len':200000,'tensor_parallel_size':2,'kv_cache_dtype':'turboquant_4bit_nc',
            'kv_cache_memory_bytes_per_gpu':6442450944,'kv_offloading_size_gib':80,'prefix_caching':True,
            'mtp_draft_tokens':2,'max_num_batched_tokens':4096,'max_num_seqs':16,'v2_model_runner':True},
        'limits':['Two timed fixed-length512-token batches per concurrency after warmup; no statistical superiority claim from the small 1.3%/4.0% margin.',
            'Matched long prompt has63460 tokens; configured200000 capacity was preserved but this is not a new200000-token request stress test.',
            'One synthetic suite does not prove broad model-quality equivalence or identify the historical repetition cause.',
            'PR141 changes upstream dispatch and packed-shard layout; serving gains are not attributed to a single kernel.',
            'PR141 extension is the measured incremental mixed nvcc13.3/13.0 SM86 build; a clean uniform compiler rebuild remains untested.',
            'Original GGUF codes/scales are retained, but upstream A8 kernels quantize activations and are not bitwise FP32 GEMMs.',
            'Controlled MTP2/off ablation remains deferred and was not run.'],
        'upstream_contribution':'Existing PR141 is the foundation. Ampere evidence and Gemma compatibility changes can complement it; no duplicate kernel-port PR has been published.'}
(ROOT/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
lines=['The final upstream PR #141 trial matched the converted model’s serving speed while loading the original Hauhau GGUF. The custom Tensor Core candidate improved prefill but did not achieve decode parity. No production checkpoint or installed Python package was overwritten.','',
       '| Serving configuration | C1 tokens/s | C4 tokens/s | Cold 63,460-token first output | Short / long / behavior checks |',
       '|---|---:|---:|---:|---|']
labels={'converted':'Converted native baseline','original':'Stock original GGUF','custom_fast_v1':'Custom fast-v1 GGUF','upstream_pr141':'Upstream PR #141 GGUF'}
for name,r in rows.items():
    behavior='not run' if r['behavior_checks'] is None else '/'.join(map(str,r['behavior_checks']))
    lines.append(f"| {labels[name]} | {r['performance']['1']['median_aggregate_tps']:.2f} | {r['performance']['4']['median_aggregate_tps']:.2f} | {r['cold_long_first_output_seconds']:.2f} s | {'/'.join(map(str,r['short_quality']))} / {'/'.join(map(str,r['long_exact_answers']))} / {behavior} |")
lines+=['',
    'PR #141 shared-prefix four-client retrieval passed 4/4, with 190,272 cached tokens out of 253,851 queried and zero preemptions. Together with the cold retrieval that is 5/5 long answers. The stock and custom 4/5 results returned correct times but omitted the required UTC suffix. Vision checks use red/blue images; tool checks include a failed-format recovery workflow.',
    '',
    'All configurations retained TP2, 200,000 configured context, TQ4 KV, 6 GiB GPU KV allocation per GPU, 80 GiB CPU KV offload, prefix caching, MTP2, native Gemma reasoning/tool parsers, vision projector and V2 runner. Sampling was temperature 0.6, top_p 0.9, top_k 64, min_p 0.05, repetition_penalty 1.1; the matched main suite used seed 20261023. Benchmark scripts and fixture bytes were unchanged. `long_suite.py` continued the same long fixture after the short-only phase without repeating performance tests.',
    '',
    'Source pins and build:',
    '',
    '- Plugin: [PR #141](https://github.com/vllm-project/vllm-gguf-plugin/pull/141), commit `e62877c30519305f3c82dc0d616cf62b5ed67133`.',
    '- Pinned llama.cpp: `002a12ad25503a93501b2e188c360029830a241a`.',
    '- Hauhau source model: `9654466e82d83f5ebfe1518a369bc5900873abb1`.',
    '- Extension SHA256: `'+release['extension_sha256']+'`.',
    '- `upstream-pr141-compatibility.patch` preserves the existing Gemma MTP adapter, assistant’s separate config, vision OIHW→HWC correction and compatible Transformers imports. It changes no CUDA kernels.',
    '- The measured extension is an incremental SM86 build: successful upstream objects from nvcc13.3 plus six remaining objects compiled with 13.0.88 against runtime 13.0. It is not a clean all-13.0 build. `pr141-build-manifest.json` records the failed toolchain attempts and final build; 10 GPU smoke cases passed, including full odd-shaped outputs and sampled actual TP shards.',
    '',
    'To reproduce the package, fetch the pinned PR commit and pinned submodule, apply the saved compatibility patch, and build the extension for SM86 in an isolated environment. A clean uniform-toolchain rebuild must receive its own numerical and serving validation. `probe_pr141.py` records graph timings, selected upstream methods, full-output finiteness and FP64-source error. Launch arguments are in `../original-gguf-args.json`; the tested upstream selector is `VLLM_GGUF_CUDA_KERNEL=upstream`. `whole-model-pr141/benchmark.py --variant NAME --short-only`, then `long_suite.py` and `check_behaviors.py`, reproduce the test stages on a fresh process using the supplied fixtures.',
    '',
    'Durable release: `'+release['path']+'`. Its startup launcher checks every recorded artifact hash. The only new persistent service file is `'+release['persistent_override']+'`; the trial `/run` override is removed. The 15 preexisting persistent unit files are preserved and snapshotted.',
    '',
    'Rollback command: `'+release['rollback']+'`. This new helper removes both the persistent PR override and transient trial override, atomically restores all 15 saved converted service files, reloads systemd and starts converted MTP2. The older rollback helper is untouched. `--check` verifies snapshots without mutation.',
    '',
    ('Persistent activation and post-restart reasoning/tool smoke passed; see `promoted-release-verification.json`.' if verify.get('passed') else 'Persistent activation verification is still running; consult `promoted-release-verification.json` before treating this as handed off.'),
    '',
    'Limits: the small 1.3% C1 and 4.0% C4 advantage establishes comparable measured speed, not statistical superiority. The long suite tests 63k input, not a new 200k request. Original weights remain original, but A8 activation quantization and floating-point arithmetic still affect outputs. The historical repetition cause remains unknown; the prepared MTP2/off ablation is deferred. Existing PR #141 deserves the kernel-port credit; our complementary results are Ampere validation and Gemma compatibility evidence. No upstream PR or comment has been published from this work.','']
(ROOT/'RESULTS.md').write_text('\n'.join(lines))
print(json.dumps({'persistent_activation_verified':result['persistent_activation_verified'],'pr141':rows['upstream_pr141']['performance']}))
