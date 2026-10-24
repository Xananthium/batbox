#!/usr/bin/env python3
"""Rebuild private sampler fixtures from published synthetic-only definitions.

No model requests are sent. Capped payload hashes must match the exact payloads
counted by /tokenize in the recorded experiment. Runtime verification is separate.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path

from sampling_sweep import private_write


def filler(i):
    names = ['cedar', 'birch', 'maple', 'aspen', 'willow', 'oak', 'elm']
    return (f'Entry {i:04d}: service {names[i%7]}-worker; incident IX-{i:04d}; revision 1; '
            f'owner team-{i%19:02d}; retry budget {3+i%8}; observations {100+i%83}; '
            f'queue delay {20+i%73} ms; rollout batch {i%11}; status verified. '
            'The review confirmed successful canary checks and retained the previous rollback snapshot.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--definitions', type=Path, default=Path(__file__).with_name('sampling_synthetic_fixtures.json'))
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    os.umask(0o077)
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    spec = json.loads(args.definitions.read_text())
    short = spec['short_fixture']
    # Match the original JSON serialization exactly (no final newline).
    short_path = args.output / 'short-fixture-private.json'
    private_write(short_path, json.dumps(short, indent=2))
    expanded = copy.deepcopy(short)
    expanded['source_fixture_sha256'] = hashlib.sha256(short_path.read_bytes()).hexdigest()
    expanded['changes'] = ('Code and arithmetic receive uniform 4096-token output budgets after observed '
                           '2048/768 budget exhaustion; original sweep results retained.')
    for case in expanded['cases']:
        if case['id'] in ('code_evidence', 'arithmetic'):
            case['payload']['max_tokens'] = 4096
    private_write(args.output / 'short-fixture-expanded-private.json', json.dumps(expanded, indent=2))
    supplement = dict(expanded, cases=[case for case in expanded['cases'] if case['id'] in ('code_evidence', 'arithmetic')])
    private_write(args.output / 'budget-supplement-private.json', json.dumps(supplement, indent=2))
    if 'creative_fixture' in spec:
        private_write(args.output / 'creativity-fixture-private.json', spec['creative_fixture'])
        combined = copy.deepcopy(expanded)
        combined['cases'].extend(spec['creative_fixture']['cases'])
        combined['creative_case_excluded_from_objective_panel'] = True
        private_write(args.output / 'short-plus-creativity-private.json', combined)
    directory = args.output / 'capped-fixtures'
    directory.mkdir(mode=0o700)
    manifest = dict(spec['capped_manifest_template'], cases=[])
    for row in spec['capped_cases']:
        payload = copy.deepcopy(row['payload_template'])
        source = [row['source_overrides'].get(str(i), filler(i)) for i in range(row['row_count'])]
        payload['messages'][3]['content'] = row['dossier_prefix'] + '\n' + '\n'.join(source) + '\nEND OF DOSSIER.'
        path = directory / (row['id'] + '.json')
        private_write(path, json.dumps(payload, indent=2))
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        assert digest == row['expected_payload_sha256'], 'Synthetic capped body differs from token-counted fixture'
        manifest['cases'].append({'id': row['id'], 'path': str(path.resolve()), 'sha256': digest,
                                  'tokenized_input_count': row['tokenized_input_count'],
                                  'total_tokens_at_full_output_budget': row['total_tokens_at_full_output_budget'],
                                  'case': row['case']})
    private_write(args.output / 'capped-c4-manifest-private.json', manifest)
    print(json.dumps({'output': str(args.output), 'short_cases': len(short['cases']),
                      'capped_cases': len(manifest['cases']), 'all_capped_payload_hashes_match': True}), flush=True)


if __name__ == '__main__':
    main()
