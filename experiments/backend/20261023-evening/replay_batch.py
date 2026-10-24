#!/usr/bin/env python3
"""Concurrent exact-body replays with private raw artifacts and safe summary."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.request


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def metrics(base):
    return urllib.request.urlopen(base + '/metrics', timeout=10).read().decode()


def counts(raw):
    result = {}
    for line in raw.splitlines():
        if line.startswith('#'):
            continue
        try:
            label, value = line.rsplit(' ', 1)
            name = label.split('{', 1)[0]
            if name.endswith(('_created', '_bucket')):
                continue
            result[name] = result.get(name, 0) + float(value)
        except ValueError:
            pass
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--mode', required=True)
    p.add_argument('--base', default='http://127.0.0.1:8002')
    p.add_argument('--run-live', action='store_true')
    a = p.parse_args()
    if not a.run_live:
        p.error('--run-live required after coordination')
    fixture = json.loads(a.manifest.read_text())
    cases = fixture['cases']
    assert len(cases) == 4
    for row in cases:
        assert hashlib.sha256(Path(row['path']).read_bytes()).hexdigest() == row['sha256']
    a.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    before_raw = metrics(a.base)
    before = counts(before_raw)
    assert before.get('vllm:num_requests_running', 0) == 0, 'Active inference would confound the batch'
    assert before.get('vllm:num_requests_waiting', 0) == 0
    (a.output / 'metrics-before.prom').write_text(before_raw)
    state = {'mode': a.mode, 'started_at': utc(), 'fixture_sha256': hashlib.sha256(a.manifest.read_bytes()).hexdigest(),
             'description': fixture['description'], 'concurrency': 4,
             'max_tokens_per_request': 4096, 'requests': [], 'tools_executed': False}
    start = time.monotonic()
    script = Path(__file__).with_name('replay_captured.py')
    def run(row):
        directory = a.output / row['id']
        command = ['python3', str(script), '--payload', row['path'], '--output', str(directory),
                   '--label', a.mode + '-' + row['id'], '--base', a.base, '--run-live']
        process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if process.returncode:
            fd = os.open(a.output / (row['id'] + '-failure-private.txt'), os.O_WRONLY | os.O_CREAT, 0o600)
            with os.fdopen(fd, 'w') as f:
                f.write(process.stdout + '\n' + process.stderr)
            return {'id': row['id'], 'process_returncode': process.returncode, 'failed': True}
        summary = json.loads((directory / 'summary.json').read_text())
        summary['id'] = row['id']
        summary['first_tool_delta_seconds'] = None
        for line in (directory / 'stream.jsonl').read_text().splitlines():
            event = json.loads(line)
            if any(c.get('delta', {}).get('tool_calls') for c in event['event'].get('choices', [])):
                summary['first_tool_delta_seconds'] = event['seconds']
                break
        print(json.dumps({'id': row['id'], 'mode': a.mode, 'wall_seconds': summary['wall_seconds'],
                          'ttft': summary['first_output_seconds'], 'stop': summary['stop_condition'],
                          'usage': summary['usage'], 'tools': summary['tools']}), flush=True)
        return summary
    with ThreadPoolExecutor(max_workers=4) as pool:
        state['requests'] = list(pool.map(run, cases))
    state['wall_seconds'] = time.monotonic() - start
    state['ended_at'] = utc()
    after_raw = metrics(a.base)
    (a.output / 'metrics-after.prom').write_text(after_raw)
    after = counts(after_raw)
    names = ('generation_tokens_total', 'prompt_tokens_total', 'spec_decode_num_drafts_total',
             'spec_decode_num_draft_tokens_total', 'spec_decode_num_accepted_tokens_total',
             'prefix_cache_queries_total', 'prefix_cache_hits_total', 'external_prefix_cache_queries_total',
             'external_prefix_cache_hits_total', 'num_preemptions_total', 'kv_offload_load_bytes_total',
             'kv_offload_load_time_total', 'kv_offload_store_bytes_total', 'kv_offload_store_time_total')
    state['metrics_delta'] = {k: after.get(k, 0) - before.get(k, 0) for k in set(before) | set(after)
                              if k.removeprefix('vllm:') in names}
    completed_tokens = sum(r.get('usage', {}).get('completion_tokens', 0) for r in state['requests'])
    state['completed_response_tokens'] = completed_tokens
    state['completed_response_aggregate_tps_including_prefill'] = completed_tokens / state['wall_seconds']
    state['all_final_usage_available'] = all(bool(r.get('usage')) for r in state['requests'])
    drafted = state['metrics_delta'].get('vllm:spec_decode_num_draft_tokens_total', 0)
    state['draft_acceptance'] = state['metrics_delta'].get('vllm:spec_decode_num_accepted_tokens_total', 0) / drafted if drafted else None
    state['limitations'] = ['Cold-prefill elapsed time is included in aggregate throughput.',
                            'Different output lengths make real-task token throughput descriptive, not fixed-work speedup.',
                            'Early-censored loops have no final usage; metrics may include tokens generated just before disconnect.']
    (a.output / 'summary.json').write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps({k: v for k, v in state.items() if k != 'requests'}), flush=True)


if __name__ == '__main__':
    main()
