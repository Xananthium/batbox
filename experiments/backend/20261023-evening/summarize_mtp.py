#!/usr/bin/env python3
"""Emit prompt-free metadata from the private paired MTP measurements."""
import argparse
import datetime
import json
from pathlib import Path


def counters(path):
    out = {}
    for line in path.read_text().splitlines():
        if line.startswith('#'):
            continue
        try:
            label, value = line.rsplit(' ', 1)
            key = label.split('{', 1)[0]
            if key.endswith('_created') or key.endswith('_bucket'):
                continue
            out[key] = out.get(key, 0) + float(value)
        except ValueError:
            pass
    return out


def real_case(root, mode, role):
    directory = root / f'{mode}-{role}-cold'
    path = directory / 'summary.json'
    if not path.exists():
        return None
    summary = json.loads(path.read_text())
    before = counters(directory / 'metrics-before.prom')
    after = counters(directory / 'metrics-after.prom')
    delta = {k: after.get(k, 0) - before.get(k, 0) for k in set(before) | set(after)}
    keep = ('prompt_tokens_total', 'generation_tokens_total', 'spec_decode_num_drafts_total',
            'spec_decode_num_draft_tokens_total', 'spec_decode_num_accepted_tokens_total',
            'prefix_cache_queries_total', 'prefix_cache_hits_total',
            'external_prefix_cache_queries_total', 'external_prefix_cache_hits_total',
            'num_preemptions_total', 'kv_offload_load_time_total', 'kv_offload_load_bytes_total')
    summary['isolated_metrics_delta'] = {k: v for k, v in delta.items()
                                        if k.removeprefix('vllm:') in keep}
    summary['prompt_tokens_from_isolated_metrics'] = delta.get('vllm:prompt_tokens_total')
    tokens = summary.get('usage', {}).get('completion_tokens')
    first = summary.get('first_output_seconds')
    summary['completed_response_decode_tps'] = ((tokens - 1) / (summary['wall_seconds'] - first)
        if tokens and tokens > 1 and first is not None else None)
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--private-root', type=Path, default=Path.home() / '.batbox/diagnostics/evening-backend-20261023/ablation')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    result = {'generated_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'status': 'partial', 'controlled_difference': 'Remove --speculative-config only',
              'preserved': {'model': 'original Hauhau Q4_K_M GGUF, pinned PR141 plugin',
                  'context': 200000, 'tensor_parallel': 2, 'kv_dtype': 'turboquant_4bit_nc',
                  'kv_bytes': 6442450944, 'kv_offload_gib': 80, 'max_num_seqs': 16,
                  'max_num_batched_tokens': 4096, 'prefix_caching': True,
                  'temperature': 0.6, 'top_p': 0.9, 'top_k': 64, 'min_p': 0.05,
                  'repetition_penalty': 1.1},
              'isolation': 'Both modes use fresh processes on localhost:8002. Batbox and memory are intentionally paused; proxy retries remain on8001. CPU compilation and tests finished before generation.',
              'speed_method': 'C1 and C4 each have one 128-token warmup and two measured 512-token trials, with ignore_eos=true. This is synthetic fixed-output throughput, not live production productivity.',
              'modes': {}, 'comparison': {},
              'limitations': ['Two measured short-speed trials per concurrency; no formal confidence interval.',
                  'Identical seeds do not force identical sampled text across speculative decoding algorithms.',
                  'Captured requests retain max_tokens=4096; clear loops abort after 256 identical tail words or512 tokens in a repeated cycle.',
                  'Returned tools are inspected but never executed; valid tool JSON alone is not semantic correctness.',
                  'Fresh-process results do not establish the cause of repetition in the long-running production process.',
                  'The concurrent real requests can have different admission timing and output lengths; their aggregate rates are not fixed-work speedup estimates.',
                  'Startup durations are not a controlled performance comparison: the initial MTP2 startup overlapped CPU builds and fixture tests.']}
    for mode in ('mtp2', 'off'):
        path = a.private_root / f'{mode}-speed/summary.json'
        if not path.exists():
            continue
        summary = json.loads(path.read_text())
        result['modes'][mode] = {'short_speed': summary['speed'], 'quality': summary['quality'],
            'speed_started_at': summary['started_at'], 'speed_ended_at': summary['ended_at'],
            'speed_payload_sha256': [r['payload_sha256'] for row in summary['performance'] for r in row['requests']],
            'real_captured': {role: real_case(a.private_root, mode, role) for role in ('worker', 'coordinator')}}
        batch = a.private_root / f'{mode}-c4-long/summary.json'
        result['modes'][mode]['c4_long'] = json.loads(batch.read_text()) if batch.exists() else None
        result['modes'][mode]['quality_counts'] = {
            'short_exact_answers_passed': sum(r['passed'] for r in summary['quality']),
            'short_exact_answers_total': len(summary['quality'])}
        if batch.exists():
            requests = result['modes'][mode]['c4_long']['requests']
            result['modes'][mode]['quality_counts'].update(
                real_c4_requests=len(requests),
                real_c4_completed_with_valid_tool_json=sum(
                    r.get('stop_condition') == 'done' and bool(r.get('tools')) and
                    all(t['arguments_valid_json_object'] for t in r['tools']) for r in requests),
                real_c4_repetition_aborts=sum(r.get('stop_condition') == 'early_censored_clear_repetition'
                                              for r in requests))
    if len(result['modes']) == 2:
        result['status'] = 'paired_speed_complete'
        on, off = result['modes']['mtp2'], result['modes']['off']
        result['identical_speed_payloads'] = on['speed_payload_sha256'] == off['speed_payload_sha256']
        if on['c4_long'] and off['c4_long']:
            result['identical_c4_long_fixture'] = on['c4_long']['fixture_sha256'] == off['c4_long']['fixture_sha256']
            result['status'] = 'paired_short_speed_and_long_c4_complete'
            result['conclusions'] = [
                'MTP2 increased fixed-output throughput by about84% at C1 and93% aggregate at C4 in this configuration.',
                'Both modes passed the four short exact-answer/tool checks and the standalone29k/95k captured requests.',
                'The matched cold C4 batch returned valid tool JSON for4/4 requests with MTP2. Without speculation,2/4 completed and2/4 entered clear reasoning-channel repetition loops.',
                'The two no-spec failures each emitted256 consecutive copies of our and were censored early. Their final usage is unavailable; completed-response token totals exclude those loops.',
                'No-spec cold C4 had zero local/external prefix hits and zero preemptions, with stores but no offload-load counter. MTP, prefix hits and CPU KV restoration are not necessary for this observed batch failure.',
                'This does not prove which kernel or state mechanism caused the failure, or that MTP2 prevents future loops.',
                'User explicitly requested MTP remain enabled. Production restoration is tracked separately.']
        for c in ('1', '4'):
            x = on['short_speed'][c]['aggregate_tps_mean']
            y = off['short_speed'][c]['aggregate_tps_mean']
            result['comparison'][c] = {'mtp2_aggregate_tps': x, 'off_aggregate_tps': y,
                'mtp2_throughput_gain_percent_vs_off': 100 * (x / y - 1),
                'disabling_mtp_throughput_change_percent_vs_on': 100 * (y / x - 1)}
    loop_meta = a.private_root / 'off-c4-loop-metadata.json'
    if loop_meta.exists():
        result['no_spec_loop_metadata'] = json.loads(loop_meta.read_text())
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'output': str(a.output), 'status': result['status'], 'comparison': result['comparison']}))


if __name__ == '__main__':
    main()
