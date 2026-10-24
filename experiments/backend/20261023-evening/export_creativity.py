#!/usr/bin/env python3
"""Export visible creative answers for blind review, with a separate private key.

The designated blind file contains only the exact challenge and random labels
with visible answers. No temperature, seed, timing, token count or source path is
included there. Model reasoning is never copied to any export.
"""
import argparse
import json
import os
from pathlib import Path
import secrets

from sampling_sweep import private_write


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--private-root', type=Path, required=True)
    p.add_argument('--public-output', type=Path, help='Publish mapped visible answers only after blind review is complete')
    args = p.parse_args()
    fixture = json.loads((args.private_root / 'creativity-fixture-private.json').read_text())
    challenge = fixture['cases'][0]['payload']['messages'][0]['content']
    map_path = args.private_root / 'creativity-blind-map-private.json'
    mapping = json.loads(map_path.read_text()) if map_path.exists() else {}
    by_source = {entry['source_key']: label for label, entry in mapping.items()}
    answers = {}
    visible_dir = args.private_root / 'visible-examples'
    visible_dir.mkdir(mode=0o700, exist_ok=True)
    for path in sorted(args.private_root.glob('*/summary.json')):
        state = json.loads(path.read_text())
        for batch in state.get('batches', []):
            for row in batch['requests']:
                if row.get('assessment', {}).get('kind') != 'creative':
                    continue
                source_key = path.parent.name + '/' + row['id']
                label = by_source.get(source_key)
                if label is None:
                    label = 'S-' + secrets.token_hex(4).upper()
                    by_source[source_key] = label
                source_path = path.parent / f'batch-{batch["batch"]:02d}' / row['id'] / 'response-private.json'
                response = json.loads(source_path.read_text())
                visible = response.get('content', '')
                sampling = row['request_sampling']
                mapping[label] = {'source_key': source_key, 'temperature': sampling['temperature'],
                    'seed': row['seed'], 'repetition_penalty': sampling['repetition_penalty'],
                    'top_k': sampling['top_k'], 'top_p': sampling['top_p'], 'min_p': sampling['min_p'],
                    'completed_before_output_cap': row['completed'],
                    'total_output_tokens': row.get('usage', {}).get('completion_tokens'),
                    'visible_word_count': row['assessment']['word_count'],
                    'visible_word_range_passed': row['assessment']['word_range_passed'],
                    'visible_only_path': str(visible_dir / (row['id'] + '.txt'))}
                private_write(visible_dir / (row['id'] + '.txt'), visible + '\n')
                answers[label] = visible
    samples = [{'label': label, 'text': visible} for label, visible in answers.items()]
    secrets.SystemRandom().shuffle(samples)
    pending_map = map_path.with_name(map_path.name + '.new')
    private_write(pending_map, mapping)
    os.replace(pending_map, map_path)
    blind_path = args.private_root / 'creativity-blind.json'
    pending_blind = blind_path.with_name(blind_path.name + '.new')
    private_write(pending_blind, {'challenge': challenge, 'samples': samples})
    os.replace(pending_blind, blind_path)
    if args.public_output:
        public = {'challenge': challenge, 'samples': [
            {'label': label, **{k: v for k, v in entry.items() if k not in ('source_key', 'visible_only_path')},
             'text': answers[label]} for label, entry in mapping.items() if label in answers]}
        args.public_output.write_text(json.dumps(public, indent=2) + '\n')
    print(json.dumps({'blind_path': str(blind_path), 'private_map_path': str(map_path),
                      'samples': len(samples), 'reasoning_exported': False}), flush=True)


if __name__ == '__main__':
    main()
