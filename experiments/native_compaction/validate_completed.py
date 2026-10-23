#!/usr/bin/env python3
"""Revalidate completed private native runs offline; never makes HTTP calls."""
import argparse
import copy
import datetime as dt
import json
from pathlib import Path
import re

from live_100k import FACT, CONSTRAINT, archive_row_count, digest


def response_content(raw):
    try:
        records = [json.loads(raw)]
    except ValueError:
        records = [json.loads(line[6:]) for line in raw.splitlines()
                   if line.startswith(b'data: ') and line != b'data: [DONE]']
    return ''.join(choice.get('delta', choice.get('message', {})).get('content') or ''
                   for record in records for choice in record.get('choices', []))


def validate(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    original_raw = (directory / 'sanitized-result.json').read_bytes()
    original = json.loads(original_raw)
    assert len(original['runs']) == 2, 'Both native processes must have completed'
    assert [run['cycle'] for run in original['runs']] == [1, 2]
    assert original['uses_installed_native_binary'] and original['inference_is_live']
    assert not original.get('upstream_errors')
    assert digest(Path(manifest['binary']).read_bytes()) == manifest['binary_sha256']
    assert original['binary_sha256'] == manifest['binary_sha256']
    assert original['script_sha256'] == manifest['script_sha256']
    assert digest((directory / 'tested-backend-identity.json').read_bytes()) == manifest['tested_backend']['identity_sha256']

    seed = json.loads((directory / 'seed-messages.json').read_text())
    session = json.loads((directory / 'state/sessions' / (manifest['session_id'] + '.json')).read_text())
    assert session['messages'][:len(seed)] == seed, 'Seed transcript changed'
    archive = seed[2]['content']
    expected_rows = archive_row_count(archive)
    assert expected_rows > 0
    checkpoints = [m for m in session['messages'] if 'context_checkpoint' in m]
    assert len(checkpoints) == 1
    checkpoint = checkpoints[0]
    summary = checkpoint['context_checkpoint'][0]['content']
    assert FACT in summary and CONSTRAINT in summary
    assert checkpoint['prompt_tokens_before'] >= 100000 > checkpoint['prompt_tokens_after']
    for phase in (1, 2):
        assert json.loads((directory / f'checkpoint-after-run-{phase}.json').read_text()) == checkpoint
    for run in original['runs']:
        assert run['summary_sha256'] == digest(summary.encode())
        assert run['checkpoint_count'] == 1
        assert run['tokens_before'] == checkpoint['prompt_tokens_before']
        assert run['tokens_after'] == checkpoint['prompt_tokens_after']
        assert all(run[k] for k in ('summary_preserves_fact_and_constraint',
            'answer_preserves_fact_and_constraint', 'full_seed_transcript_preserved', 'project_unchanged'))
    state = json.loads((directory / 'state/state.json').read_text())
    assert state['session_id'] == manifest['session_id'] and state['cycle'] == 2
    assert FACT in state['last_response'] and CONSTRAINT in state['last_response']
    assert not (directory / 'state/running.json').exists()
    assert set(p.name for p in (directory / 'project').iterdir()) == {'DO_NOT_CHANGE.txt'}
    assert (directory / 'project/DO_NOT_CHANGE.txt').read_text() == 'Isolated native compaction test project.\n'

    events = copy.deepcopy(original['requests'])
    normal, summaries, answers, row_evidence = [], [], [], []
    for event in events:
        raw = (directory / 'requests' / f"{event['index']:03d}.request.json").read_bytes()
        body = json.loads(raw)
        assert digest(raw) == event['request_sha256']
        assert event['status'] == 200 and 'error_type' not in event
        content = '\n'.join(m.get('content') or '' for m in body.get('messages', []))
        count = archive_row_count(content)
        event['has_archive_label'] = 'ARCHIVE_ROW_' in content
        event['archive_row_count'] = count
        event['has_archive_rows'] = count > 0
        event['entire_original_archive_present'] = archive in content
        if event['kind'] == 'normal':
            normal.append(event)
            assert digest(body['messages'][0]) == event['system_sha256']
            assert digest(body.get('tools', [])) == event['tools_sha256']
            assert body.get('chat_template_kwargs') is None
            row_evidence.append({k: event[k] for k in ('index', 'phase', 'path',
                'archive_row_count', 'has_archive_label', 'entire_original_archive_present')})
        if event['path'] == '/v1/chat/completions':
            assert not event['response']['tool_intent']
            assert event['response']['finish_reasons'] == ['stop']
            assert body['model'] == manifest['model']
            if event['kind'] == 'summary':
                summaries.append(event)
                assert body['max_tokens'] == 20000 and body['temperature'] == 0
                assert body.get('chat_template_kwargs') is None
            else:
                assert body['max_tokens'] == 4096
                response = (directory / 'requests' / f"{event['index']:03d}.response.bin").read_bytes()
                content = response_content(response).strip()
                strict_json = True
                try:
                    answer = json.loads(content)
                except ValueError:
                    fenced = re.fullmatch(r'```(?:json)?\s*(\{.*\})\s*```', content, re.S)
                    assert fenced is not None, 'Answer was neither JSON nor a fenced JSON object'
                    answer = json.loads(fenced[1])
                    strict_json = False
                assert answer['access_phrase'] == FACT and CONSTRAINT in answer['compatibility']
                answers.append({'phase': event['phase'], 'json_object_valid': True,
                    'strict_json_only': strict_json,
                    'format': 'json' if strict_json else 'markdown_fenced_json',
                    'fact_correct': True, 'constraint_correct': True})
    assert [a['phase'] for a in answers] == [1, 2]
    assert summaries and all(e['phase'] == 1 for e in summaries)
    assert len({e['system_sha256'] for e in normal}) == 1
    assert len({e['tools_sha256'] for e in normal}) == 1 and min(e['tool_count'] for e in normal) > 10
    baseline = next(e for e in normal if e['phase'] == 1 and e['path'] == '/tokenize')
    assert baseline['archive_row_count'] == expected_rows and baseline['entire_original_archive_present']
    assert baseline['count'] == checkpoint['prompt_tokens_before']
    assert all(e['archive_row_count'] == 0 and not e['entire_original_archive_present']
               for e in normal if e is not baseline)
    assert all(e['max_model_len'] == 200000 for e in normal if e['path'] == '/tokenize')
    assert all(e.get('count', 0) < 100000 for e in normal if e['phase'] == 2)

    report = copy.deepcopy(original)
    report.pop('error', None)
    report.update(phase='passed', verification_kind='live_native_run_with_offline_predicate_revalidation',
        summary_model_calls=len(summaries), tool_count=normal[0]['tool_count'],
        stable_system_sha256=normal[0]['system_sha256'], stable_tools_sha256=normal[0]['tools_sha256'],
        restart_used_checkpoint=True, native_request_count=len(events), requests=events,
        answer_checks=answers, archive_evidence=row_evidence, seed_message_count=len(seed),
        strict_answer_format_passed=all(a['strict_json_only'] for a in answers),
        result_scope='Compaction, required fact/compatibility retention, transcript, stable prefix, checkpoint restart and project isolation.',
        tested_backend=manifest['tested_backend'], shared_load_observation=json.loads((directory / 'run-observation.json').read_text()),
        completed_at_utc=dt.datetime.fromtimestamp(max(e['started_at'] + e['elapsed_seconds'] for e in events), dt.timezone.utc).isoformat(),
        offline_revalidation={
            'utc': dt.datetime.now(dt.timezone.utc).isoformat(),
            'validator_sha256': digest(Path(__file__).read_bytes()),
            'corrected_harness_sha256': digest(Path(__file__).with_name('live_100k.py').read_bytes()),
            'original_harness_sha256': original['script_sha256'],
            'original_harness_phase': original['phase'],
            'original_report_sha256': digest(original_raw),
            'original_report_preserved': True,
            'initial_failure': 'Substring ARCHIVE_ROW_ also matches valid summary references to labels.',
            'corrected_assertion': 'No complete synthetic archive records and no entire original archive in compacted/restarted requests.',
            'extra_inference_requests': 0})
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = validate(args.directory.resolve())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('phase', 'verification_kind', 'summary_model_calls',
        'tool_count', 'restart_used_checkpoint', 'seed_message_count')}))


if __name__ == '__main__':
    main()
