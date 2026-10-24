#!/usr/bin/env python3
"""Bounded sampler comparisons; private fixtures and streams, metadata-only summary.

Requires explicit --run-live after the operator confirms backend readiness and
CPU isolation. Does not change services, server defaults, or execute model tools.
The fixture is private; it must contain cases with payload and assessment_kind.
Long mode preserves complete capped fixture bodies except declared sampler/cache
fields. The manifest must certify a tokenized input count no larger than 20k.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import copy
import datetime
import hashlib
import json
import os
from pathlib import Path
import random
import re
import secrets
import statistics
import subprocess
import time
import urllib.request

import jsonschema


BASELINE = dict(temperature=0.6, top_k=64, top_p=0.9, min_p=0.05,
                repetition_penalty=1.1)
MODEL = 'qwen3.6-35b-hauhau-w4a16-vision'
SAMPLER_FIELDS = set(BASELINE)
REPLAY = Path(__file__).with_name('replay_captured.py')
METRIC_NAMES = ('request_success_total', 'request_prompt_tokens_count',
                'prompt_tokens_total', 'generation_tokens_total',
                'prefix_cache_queries_total', 'prefix_cache_hits_total',
                'external_prefix_cache_queries_total', 'external_prefix_cache_hits_total',
                'spec_decode_num_drafts_total', 'spec_decode_num_draft_tokens_total',
                'spec_decode_num_accepted_tokens_total', 'num_preemptions_total',
                'kv_offload_load_bytes_total', 'kv_offload_load_time_total',
                'kv_offload_store_bytes_total', 'kv_offload_store_time_total')


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(',', ':')).encode()


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def private_write(path, obj):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.chmod(path, 0o600)
    with os.fdopen(fd, 'w') as stream:
        if isinstance(obj, str):
            stream.write(obj)
        else:
            json.dump(obj, stream, indent=2)
            stream.write('\n')


def metrics(base):
    with urllib.request.urlopen(base + '/metrics', timeout=10) as response:
        raw = response.read().decode()
    values = {}
    for line in raw.splitlines():
        if line.startswith('#'):
            continue
        try:
            label, value = line.rsplit(' ', 1)
            name = label.split('{', 1)[0]
            if name.endswith(('_created', '_bucket')):
                continue
            values[name] = values.get(name, 0) + float(value)
        except ValueError:
            pass
    return raw, values


def difference(before, after):
    return {f'vllm:{name}': after.get(f'vllm:{name}', 0) - before.get(f'vllm:{name}', 0)
            for name in METRIC_NAMES}


def repetition_extra(text):
    words = re.findall(r'\w+|[^\w\s]', text.lower())
    longest = count = 0
    prior = None
    for word in words:
        count = count + 1 if word == prior else 1
        longest = max(longest, count)
        prior = word
    grams = Counter(tuple(words[i:i+8]) for i in range(max(0, len(words)-7)))
    return {'longest_identical_token_run': longest,
            'most_frequent_8gram_occurrences': max(grams.values(), default=0)}


def assess(spec, response, payload):
    content = response.get('content', '')
    calls = list(response.get('tools', {}).values())
    try:
        answer = json.loads(content.strip())
        strict_json = True
    except ValueError:
        strict_json = False
        # Semantic correctness and compliance with "JSON only" are separate.
        # This matches the earlier benchmark's fence-tolerant answer comparison.
        stripped = re.sub(r'^```(?:json)?\s*|\s*```$', '', content.strip()).strip()
        try:
            answer = json.loads(stripped)
        except ValueError:
            answer = None
    kind = spec.get('assessment_kind', 'captured_tool')
    result = {'kind': kind, 'strict_json_only': strict_json}
    if kind == 'exact':
        result['passed'] = answer == spec['expected'] and not calls
        result['strict_format_passed'] = strict_json and not calls
        result['matching_fields'] = ({k: isinstance(answer, dict) and answer.get(k) == v
                                     for k, v in spec['expected'].items()}
                                    if isinstance(spec['expected'], dict) else {})
    elif kind == 'tool':
        matched = False
        if len(calls) == 1:
            try:
                matched = {'name': calls[0]['name'], 'arguments': json.loads(calls[0]['arguments'])} == spec['tool_expected']
            except ValueError:
                pass
        result['passed'] = matched
    elif kind == 'open_design':
        words = len(re.findall(r"\b[\w'-]+\b", content))
        headings = {heading: bool(re.search(r'^\s*(?:#{1,6}\s*|\*\*)?' + re.escape(heading) +
                                            r'(?:\*\*)?\s*:?\s*$', content, re.MULTILINE))
                    for heading in spec['required_headings']}
        result.update(word_count=words, heading_presence=headings,
                      word_range_passed=spec['word_range'][0] <= words <= spec['word_range'][1],
                      manual_semantic_review_required=True)
        result['passed'] = all(headings.values()) and result['word_range_passed']
    elif kind == 'creative':
        words = len(re.findall(r"\b[\w'-]+\b", content))
        result.update(word_count=words,
                      word_range_passed=spec['word_range'][0] <= words <= spec['word_range'][1],
                      semantic_review_required=True, objective_correctness_scored=False)
    schemas = {tool['function']['name']: tool['function'].get('parameters', {})
               for tool in payload.get('tools', []) if tool.get('type') == 'function'}
    tool_checks = []
    for call in calls:
        valid_json = known = schema_valid = False
        try:
            arguments = json.loads(call['arguments'])
            valid_json = isinstance(arguments, dict)
            known = call['name'] in schemas
            if valid_json and known:
                jsonschema.validate(arguments, schemas[call['name']])
                schema_valid = True
        except (ValueError, jsonschema.ValidationError, jsonschema.SchemaError):
            pass
        tool_checks.append({'arguments_json_object': valid_json, 'known_tool_name': known,
                            'schema_valid': schema_valid, 'argument_chars': len(call['arguments'])})
    result['tool_checks'] = tool_checks
    if kind == 'captured_tool':
        result['passed'] = bool(calls) and all(row['schema_valid'] for row in tool_checks)
        result['semantic_tool_choice_review_required'] = True
    return result


def replay(args, job, batch_dir):
    directory = batch_dir / job['id']
    payload_path = batch_dir / (job['id'] + '-request-private.json')
    private_write(payload_path, job['payload'])
    command = ['python3', str(REPLAY), '--payload', str(payload_path), '--output', str(directory),
               '--label', job['id'], '--base', args.base, '--run-live',
               '--deadline', str(args.deadline), '--read-timeout', '600']
    process = subprocess.run(command, capture_output=True, text=True)
    if process.returncode:
        private_write(batch_dir / (job['id'] + '-failure-private.txt'), process.stdout + '\n' + process.stderr)
        return {'id': job['id'], 'case': job['case']['id'], 'seed': job['seed'],
                'arm': job['arm'], 'process_returncode': process.returncode, 'failed': True}
    source = json.loads((directory / 'summary.json').read_text())
    response = json.loads((directory / 'response-private.json').read_text())
    fields = ('usage', 'first_event_seconds', 'first_output_seconds', 'wall_seconds',
              'finish_reasons', 'stop_condition', 'transport_error', 'content_chars',
              'reasoning_chars', 'content_repetition', 'reasoning_repetition',
              'prompt_tools_sha256', 'request_cache_salt_sha256', 'request_sampling')
    row = {k: source[k] for k in fields}
    row.update(id=job['id'], case=job['case']['id'], seed=job['seed'], arm=job['arm'],
               assessment=assess(job['case'], response, job['payload']),
               payload_sha256=sha(canonical(job['payload'])), tools_executed=False)
    row['completed'] = source['stop_condition'] == 'done' and bool(source['usage']) and 'length' not in source['finish_reasons']
    row['has_visible_output'] = bool(response.get('content') or response.get('tools'))
    row['first_visible_seconds'] = row['first_tool_delta_seconds'] = None
    for line in (directory / 'stream.jsonl').read_text().splitlines():
        event = json.loads(line)
        for choice in event['event'].get('choices', []):
            delta = choice.get('delta', {})
            if row['first_visible_seconds'] is None and (delta.get('content') or delta.get('tool_calls')):
                row['first_visible_seconds'] = event['seconds']
            if row['first_tool_delta_seconds'] is None and delta.get('tool_calls'):
                row['first_tool_delta_seconds'] = event['seconds']
    for channel in ('content', 'reasoning'):
        row[channel + '_repetition'].update(repetition_extra(response.get(channel, '')))
    row['clear_repetition'] = any(row[c + '_repetition']['longest_identical_token_run'] >= 64 or
                                  row[c + '_repetition']['tail_cycle']['repetitions'] >= 32
                                  for c in ('content', 'reasoning'))
    return row


def run_batch(args, jobs, number):
    directory = args.output / f'batch-{number:02d}'
    directory.mkdir(mode=0o700)
    before_raw, before = metrics(args.base)
    if before.get('vllm:num_requests_running', 0) or before.get('vllm:num_requests_waiting', 0):
        raise RuntimeError('Backend is not idle; no batch request sent')
    private_write(directory / 'metrics-before.prom', before_raw)
    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=len(jobs)) as pool:
        rows = list(pool.map(lambda job: replay(args, job, directory), jobs))
    wall = time.monotonic() - start
    after_raw, after = metrics(args.base)
    # Allow the metrics logger to account for the just-finished requests.
    for _ in range(15):
        count = after.get('vllm:request_success_total', 0) - before.get('vllm:request_success_total', 0)
        if count >= len(jobs) and not after.get('vllm:num_requests_running', 0):
            break
        time.sleep(0.2)
        after_raw, after = metrics(args.base)
    private_write(directory / 'metrics-after.prom', after_raw)
    delta = difference(before, after)
    count = delta['vllm:request_success_total']
    expected_prompt = sum(row.get('usage', {}).get('prompt_tokens', 0) for row in rows)
    all_usage = all(bool(row.get('usage')) for row in rows)
    contamination = count != len(jobs) or bool(after.get('vllm:num_requests_running', 0) or after.get('vllm:num_requests_waiting', 0))
    if all_usage and delta['vllm:prompt_tokens_total'] != expected_prompt:
        contamination = True
    result = {'batch': number, 'arms': sorted({job['arm'] for job in jobs}),
              'concurrency': len(jobs), 'wall_seconds': wall, 'requests': rows, 'metrics_delta': delta,
              'isolation': {'idle_at_start': True, 'own_request_count': len(jobs),
                            'observed_completed_request_delta': count,
                            'own_prompt_tokens_from_final_usage': expected_prompt if all_usage else None,
                            'observed_prompt_token_delta': delta['vllm:prompt_tokens_total'],
                            'running_after': after.get('vllm:num_requests_running', 0),
                            'waiting_after': after.get('vllm:num_requests_waiting', 0),
                            'timing_contaminated_or_accounting_incomplete': contamination,
                            'admission_counter_available': False}}
    private_write(directory / 'summary.json', result)
    print(json.dumps({'batch': number, 'arms': result['arms'], 'wall_seconds': wall,
                      'cases': [{k: row.get(k) for k in ('case', 'seed', 'arm', 'completed', 'clear_repetition')} |
                                {'kind': row.get('assessment', {}).get('kind'),
                                 'passed': row.get('assessment', {}).get('passed')} for row in rows],
                      'isolation': result['isolation']}), flush=True)
    return result


def summarize(state):
    rows = [row for batch in state['batches'] for row in batch['requests']]
    result = copy.deepcopy(state)
    result['arms'] = {}
    for arm in sorted({row['arm'] for row in rows}):
        selected = [row for row in rows if row['arm'] == arm]
        objective = [row for row in selected if row.get('assessment', {}).get('kind') in ('exact', 'tool', 'captured_tool')]
        open_rows = [row for row in selected if row.get('assessment', {}).get('kind') == 'open_design']
        creative_rows = [row for row in selected if row.get('assessment', {}).get('kind') == 'creative']
        exact_rows = [row for row in selected if row.get('assessment', {}).get('kind') == 'exact']
        mean = lambda values: statistics.mean(values) if values else None
        result['arms'][arm] = {'requests': len(selected),
            'objective_passes': sum(row.get('assessment', {}).get('passed', False) for row in objective),
            'objective_total': len(objective),
            'strict_json_format_passes': sum(row['assessment']['strict_json_only'] for row in exact_rows),
            'strict_json_format_total': len(exact_rows),
            'open_format_passes': sum(row['assessment']['passed'] for row in open_rows),
            'open_total': len(open_rows),
            'creative_total': len(creative_rows),
            'completed': sum(row.get('completed', False) for row in selected),
            'clear_repetition_count': sum(row.get('clear_repetition', False) for row in selected),
            'completion_tokens': sum(row.get('usage', {}).get('completion_tokens', 0) for row in selected),
            'wall_seconds_mean': mean([row['wall_seconds'] for row in selected if 'wall_seconds' in row]),
            'content_repeated_8gram_fraction_mean': mean([row['content_repetition']['repeated_8gram_fraction'] for row in selected if 'content_repetition' in row]),
            'reasoning_repeated_8gram_fraction_mean': mean([row['reasoning_repetition']['repeated_8gram_fraction'] for row in selected if 'reasoning_repetition' in row])}
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('phase', choices=('temperature', 'factors', 'long'))
    p.add_argument('--fixture', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--base', default='http://127.0.0.1:8001')
    p.add_argument('--temperature', type=float, default=1.0)
    p.add_argument('--temperatures', type=float, nargs='+', default=[0.6, 0.85, 1.0, 1.15])
    p.add_argument('--repetition-penalty', type=float, default=1.1)
    p.add_argument('--factor-family', choices=('all', 'repetition', 'topk'), default='all')
    p.add_argument('--candidate', type=Path, help='JSON object of sampler overrides for the long finalist')
    p.add_argument('--long-arm', choices=('baseline', 'candidate'), default='candidate')
    p.add_argument('--deadline', type=float, default=1800)
    p.add_argument('--rescore-only', action='store_true', help='Rescore saved private outputs without sending requests')
    p.add_argument('--cross-arm-batches', action='store_true', help='For matched supplements, compare all four arms concurrently per case and seed')
    p.add_argument('--whole-arm-blocks', action='store_true', help='Complete all cases and seeds for one sampler before advancing to the next')
    p.add_argument('--pack-small-batches', action='store_true', help='Within whole-arm blocks, pack at most four same-sampler requests per batch')
    p.add_argument('--run-live', action='store_true')
    args = p.parse_args()
    if args.rescore_only:
        state = json.loads((args.output / 'summary.json').read_text())
        plan = json.loads((args.output / 'run-plan-private.json').read_text())
        for batch in state['batches']:
            jobs = {job['id']: job for job in plan['batches'][batch['batch']]}
            directory = args.output / f'batch-{batch["batch"]:02d}'
            for row in batch['requests']:
                if row.get('failed'):
                    continue
                job = jobs[row['id']]
                response = json.loads((directory / row['id'] / 'response-private.json').read_text())
                row['assessment'] = assess(job['case'], response, job['payload'])
            private_write(directory / 'summary.json', batch)
        state['scoring_notes'] = ['Semantic JSON correctness accepts a surrounding Markdown JSON fence, matching the prior benchmark.',
                                  'Strict JSON-only formatting is scored and reported independently.',
                                  'All saved arms were rescored uniformly; no new requests were sent.']
        result = summarize(state)
        private_write(args.output / 'summary.json', result)
        print(json.dumps({'arms': result['arms']}), flush=True)
        return
    if not args.run_live:
        p.error('--run-live required after readiness and CPU CLEAR')
    os.umask(0o077)
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    raw = args.fixture.read_bytes()
    fixture = json.loads(raw)
    state = {'schema': 1, 'phase': args.phase, 'started_at': utc(), 'fixture_sha256': sha(raw),
             'baseline_sampling': BASELINE, 'base': args.base, 'batches': [],
             'invariants': {'mtp_depth': 2, 'max_model_len': 200000, 'compaction_threshold': 100000,
                            'service_or_defaults_mutated': False, 'tools_executed': False},
             'limitations': ['Small synthetic sample; absence of short loops is not proof of a long-session fix.',
                             'Open design format checks are separate from qualitative human/agent review.',
                             'One shared server process and fixed seeds do not eliminate all order or runtime-state effects.',
                             'Elapsed times include prompt processing and varying output lengths; not fixed-work throughput.',
                             'Isolation uses idle gauges and completed-request/token accounting; no independent admission counter.']}
    jobs_batches = []
    if args.phase == 'long':
        sampler = dict(BASELINE)
        if args.long_arm == 'candidate':
            if args.candidate is None:
                p.error('--candidate required for long candidate')
            overrides = json.loads(args.candidate.read_text())
            if set(overrides) - SAMPLER_FIELDS:
                p.error('Candidate may only override the five declared sampling fields')
            sampler.update(overrides)
        state['sampler'] = sampler
        state['case_invariants'] = []
        jobs = []
        for row in fixture['cases']:
            assert 0 < row.get('tokenized_input_count', 0) <= 20000, 'User capped all tests near 20k input tokens'
            source = Path(row['path']).read_bytes()
            assert sha(source) == row['sha256'], 'Captured fixture changed'
            original = json.loads(source)
            assert original.get('max_tokens') == 4096 and original.get('seed') == 17
            payload = copy.deepcopy(original)
            payload.update(sampler, cache_salt=secrets.token_urlsafe(32))
            unchanged = lambda obj: {k: v for k, v in obj.items() if k not in SAMPLER_FIELDS | {'cache_salt'}}
            assert unchanged(original) == unchanged(payload)
            state['case_invariants'].append({'id': row['id'], 'original_body_sha256': row['sha256'],
                'unchanged_fields_sha256': sha(canonical(unchanged(payload))),
                'declared_changes': sorted(k for k in payload if payload.get(k) != original.get(k)),
                'max_tokens': payload['max_tokens'], 'seed': payload['seed'],
                'tokenized_input_count': row['tokenized_input_count'],
                'messages': len(payload['messages']), 'tool_count': len(payload.get('tools', []))})
            case = row.get('case', {'id': row['id'], 'assessment_kind': 'captured_tool'})
            jobs.append({'id': args.long_arm + '-' + row['id'], 'case': case,
                         'seed': 17, 'arm': args.long_arm, 'payload': payload})
        assert len(jobs) == 4
        jobs_batches.append(jobs)
    else:
        if args.phase == 'temperature':
            arms = {f't{temperature:g}': dict(BASELINE, temperature=temperature)
                    for temperature in args.temperatures}
        else:
            reference = dict(BASELINE, temperature=args.temperature, repetition_penalty=args.repetition_penalty)
            arms = {}
            if args.factor_family in ('all', 'repetition'):
                arms.update({'rp1': dict(reference, repetition_penalty=1.0),
                             'rp1.2': dict(reference, repetition_penalty=1.2)})
            if args.factor_family in ('all', 'topk'):
                arms.update({'k128': dict(reference, top_k=128), 'k_all': dict(reference, top_k=-1)})
        state['samplers'] = arms
        state['matched_seeds'] = fixture['seeds']
        rng = random.Random(20261024)
        for seed in fixture['seeds']:
            for start in range(0, len(fixture['cases']), 4):
                names = list(arms)
                rng.shuffle(names)
                for name in names:
                    jobs = []
                    for case in fixture['cases'][start:start+4]:
                        payload = {'model': MODEL, 'stream': True, 'stream_options': {'include_usage': True},
                                   **copy.deepcopy(case['payload']), **arms[name], 'seed': seed,
                                   'cache_salt': secrets.token_urlsafe(32)}
                        jobs.append({'id': f'{name}-s{seed}-{case["id"]}', 'case': case,
                                     'seed': seed, 'arm': name, 'payload': payload})
                    jobs_batches.append(jobs)
        if args.cross_arm_batches:
            matched = {}
            for batch in jobs_batches:
                for job in batch:
                    matched.setdefault((job['seed'], job['case']['id']), []).append(job)
            jobs_batches = list(matched.values())
            assert all(len(batch) == len(arms) for batch in jobs_batches)
            state['schedule'] = 'One request per arm concurrently, separately for each matched case and seed'
        if args.whole_arm_blocks:
            assert not args.cross_arm_batches, 'Choose whole-condition blocks or cross-arm supplements'
            order = {name: i for i, name in enumerate(arms)}
            jobs_batches.sort(key=lambda batch: order[batch[0]['arm']])
            state['schedule'] = 'Complete all cases and seeds for each sampler before moving to the next; at most four concurrent requests at one temperature'
            if args.pack_small_batches:
                grouped = {name: [] for name in arms}
                for batch in jobs_batches:
                    for job in batch:
                        grouped[job['arm']].append(job)
                jobs_batches = [jobs[i:i+4] for jobs in grouped.values() for i in range(0, len(jobs), 4)]
    private_write(args.output / 'run-plan-private.json', {'batches': jobs_batches})
    private_write(args.output / 'summary.json', state)
    looping_arms = set()
    for i, jobs in enumerate(jobs_batches):
        batch = run_batch(args, jobs, i)
        state['batches'].append(batch)
        private_write(args.output / 'summary.json', summarize(state))
        for row in batch['requests']:
            if row.get('clear_repetition') or row.get('failed'):
                looping_arms.add(row['arm'])
        if len(looping_arms) >= 2:
            state['paused_for_state_confound'] = True
            break
    state['ended_at'] = utc()
    summary = summarize(state)
    private_write(args.output / 'summary.json', summary)
    print(json.dumps({'phase': args.phase, 'arms': summary['arms'],
                      'paused_for_state_confound': state.get('paused_for_state_confound', False)}), flush=True)


if __name__ == '__main__':
    main()
