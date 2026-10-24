#!/usr/bin/env python3
"""Identical isolated C1/C4 speed and short quality fixtures for both MTP modes.

Uses the earlier measured fixture and request implementation. Fixed-length
ignore_eos throughput tests are synthetic; real captured requests are separate.
Private full results retain reasoning/content. Stdout includes metadata only.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import statistics


PRIOR = Path('/home/isnotreal/batbox/experiments/backend/20261023-qat-format/optimized_gguf/whole-model-pr141')


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode', required=True, choices=('mtp2', 'off'))
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--base', default='http://127.0.0.1:8002')
    p.add_argument('--run-live', action='store_true')
    a = p.parse_args()
    if not a.run_live:
        p.error('--run-live required after checking isolation and CPU-build completion')
    a.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    spec = importlib.util.spec_from_file_location('previous_benchmark', PRIOR / 'benchmark.py')
    b = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(b)
    b.BASE = a.base
    fixture = json.loads((PRIOR / 'fixtures.json').read_text())
    state = {'mode': a.mode, 'started_at': utc(), 'sampling': b.SAMPLING,
             'server_context': 200000, 'performance': [], 'quality': [],
             'speed_budget': {'warmup': 128, 'measured': 512, 'ignore_eos': True},
             'fixture_sha256': hashlib.sha256((PRIOR / 'fixtures.json').read_bytes()).hexdigest(),
             'request_implementation_sha256': hashlib.sha256((PRIOR / 'benchmark.py').read_bytes()).hexdigest()}
    def save():
        fd = os.open(a.output / 'full-private.json', os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f:
            json.dump(state, f, indent=2)
    raw, initial = b.metrics()
    (a.output / 'metrics-before.prom').write_text(raw)
    if initial.get('vllm:num_requests_running', 0) != 0 or initial.get('vllm:num_requests_waiting', 0) != 0:
        raise RuntimeError('Server is not isolated/idle at benchmark start')
    for concurrency in (1, 4):
        for repeat in range(3):
            tokens = 128 if repeat == 0 else 512
            specs = []
            for client in range(concurrency):
                prompt = (f'Final matched benchmark BX-20261023 C{concurrency} request {client}. '
                          'Write a detailed engineering review of a durable job queue, including idempotency, '
                          'retry handling, worker crashes, cancellation, audit records, and practical regression checks.')
                specs.append({'id': f'perf_c{concurrency}_r{repeat}_{client}', 'payload': {
                    'messages': [{'role': 'user', 'content': prompt}],
                    'max_tokens': tokens, 'ignore_eos': True}})
            _, before = b.metrics()
            started = utc()
            row = b.batch(specs)
            _, after = b.metrics()
            row.update(concurrency=concurrency, repeat=repeat, warmup=repeat == 0,
                       started_at=started, ended_at=utc(), metrics_delta=b.diff(before, after))
            state['performance'].append(row)
            save()
            print(json.dumps({k: v for k, v in row.items() if k not in ('requests', 'metrics_delta')}), flush=True)
    chosen = [s for s in fixture['quality'] if s['id'] in ('facts', 'arithmetic', 'constraints')]
    chosen.append(fixture['tool'])
    for q in chosen:
        row = b.assess(q, b.measure(q))
        state['quality'].append(row)
        save()
        print(json.dumps({'case': row['id'], 'passed': row['assessment']['passed'],
                          'wall_seconds': row['wall_seconds'], 'finish_reason': row['finish_reason']}), flush=True)
    state['ended_at'] = utc()
    raw, final = b.metrics()
    (a.output / 'metrics-after.prom').write_text(raw)
    state['metrics_delta'] = b.diff(initial, final)
    summary = {k: v for k, v in state.items() if k not in ('performance', 'quality')}
    summary['performance'] = []
    for row in state['performance']:
        safe = {k: v for k, v in row.items() if k != 'requests'}
        safe['requests'] = [{k: v for k, v in r.items() if k not in ('content', 'reasoning', 'tool_calls')}
                            for r in row['requests']]
        summary['performance'].append(safe)
    summary['quality'] = [{k: r[k] for k in ('id', 'usage', 'wall_seconds', 'first_output_seconds',
                          'finish_reason', 'payload_sha256')} | {'passed': r['assessment']['passed']}
                          for r in state['quality']]
    summary['speed'] = {}
    for c in (1, 4):
        rows = [r for r in state['performance'] if r['concurrency'] == c and not r['warmup']]
        summary['speed'][str(c)] = {
            'aggregate_tps_mean': statistics.mean(r['aggregate_output_tokens_per_second'] for r in rows),
            'aggregate_tps_trials': [r['aggregate_output_tokens_per_second'] for r in rows],
            'per_request_decode_tps_mean': statistics.mean(r['decode_tokens_per_second'] for row in rows for r in row['requests']),
            'ttft_seconds_mean': statistics.mean(r['first_output_seconds'] for row in rows for r in row['requests'])}
    save()
    (a.output / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps({'mode': a.mode, 'speed': summary['speed'],
                      'quality_passes': sum(x['passed'] for x in summary['quality']), 'quality_total': len(chosen)}), flush=True)


if __name__ == '__main__':
    main()
