#!/usr/bin/env python3
"""Compare saved capped sampling runs; publish synthetic answers, never reasoning."""
import argparse
import copy
import datetime
import hashlib
import json
from pathlib import Path
import re

from summarize_sampling import compact


def parse_visible(text):
    text = re.sub(r'^```(?:json)?\s*|\s*```$', '', text.strip()).strip()
    try:
        return json.loads(text)
    except ValueError:
        return None


def normalize_order(case, answer):
    answer = copy.deepcopy(answer)
    ambiguous = False
    if case == 'deployment_report' and isinstance(answer, dict):
        report = answer.get('report', {})
        if isinstance(report, dict) and isinstance(report.get('planned_order'), str):
            report['planned_order'] = list(report['planned_order'])
            ambiguous = True
    return answer, ambiguous


def normalize_scalar_types(answer, expected, path='$'):
    """Expose value-equivalent strings separately; never change contract scores."""
    changes = []
    if isinstance(answer, dict) and isinstance(expected, dict):
        result = copy.deepcopy(answer)
        for key in result:
            if key in expected:
                result[key], nested = normalize_scalar_types(result[key], expected[key], path + '.' + key)
                changes.extend(nested)
        return result, changes
    if isinstance(answer, list) and isinstance(expected, list):
        result = copy.deepcopy(answer)
        for index in range(min(len(result), len(expected))):
            result[index], nested = normalize_scalar_types(result[index], expected[index], f'{path}[{index}]')
            changes.extend(nested)
        return result, changes
    result = answer
    if isinstance(answer, str):
        if expected is None and answer == 'null':
            result = None
        elif isinstance(expected, bool) and answer in ('true', 'false'):
            result = answer == 'true'
        elif type(expected) is int and re.fullmatch(r'-?(?:0|[1-9][0-9]*)', answer):
            result = int(answer)
    if type(result) is not type(answer):
        changes.append({'path': path, 'actual': answer, 'normalized': result})
    return result, changes


def compare_phase(root, phase):
    directory = root / phase
    state = json.loads((directory / 'summary.json').read_text())
    plan = json.loads((directory / 'run-plan-private.json').read_text())
    jobs = {job['id']: job for batch in plan['batches'] for job in batch}
    cases = []
    for batch in state['batches']:
        for row in batch['requests']:
            response = json.loads((directory / f'batch-{batch["batch"]:02d}' /
                                   row['id'] / 'response-private.json').read_text())
            expected = jobs[row['id']]['case']['tool_expected']
            visible = parse_visible(response.get('content', ''))
            calls = [{'name': call['name'], 'arguments_text': call['arguments'],
                      'parsed_arguments': parse_visible(call['arguments'])}
                     for call in response.get('tools', {}).values()]
            answers = ([visible] if visible is not None else []) + [
                call['parsed_arguments'] for call in calls if call['parsed_arguments'] is not None]
            normalized = [normalize_order(row['case'], answer) for answer in answers]
            scalar_normalized = [normalize_scalar_types(answer, expected['arguments'])
                                 for answer, _ in normalized]
            cases.append({
                'case': row['case'], 'seed': row['seed'], 'completed_before_cap': row['completed'],
                'finish_reasons': row.get('finish_reasons', []),
                'input_tokens': row.get('usage', {}).get('prompt_tokens'),
                'output_tokens_including_reasoning': row.get('usage', {}).get('completion_tokens'),
                'actual_tool_calls': len(calls),
                'correct_tool_contract': row.get('assessment', {}).get('passed', False),
                'tool_validation': row.get('assessment', {}).get('tool_checks', []),
                'reported_data_exact': any(answer == expected['arguments'] for answer in answers),
                'reported_data_equivalent_after_order_normalization': any(
                    answer == expected['arguments'] for answer, _ in normalized),
                'deployment_string_array_ambiguity': any(ambiguous for _, ambiguous in normalized),
                'reported_values_equivalent_after_order_and_scalar_normalization': any(
                    answer == expected['arguments'] for answer, _ in scalar_normalized),
                'scalar_type_differences': [change for _, changes in scalar_normalized for change in changes],
                'expected_tool': expected, 'visible_json_report': visible,
                'synthetic_tool_calls': calls,
                'clear_repetition_detected': row.get('clear_repetition', False),
            })
    return {
        'phase': phase, 'sampling': state['sampler'], 'evidence': compact(state), 'cases': cases,
        'totals': {
            'requests': len(cases),
            'completed_before_cap': sum(row['completed_before_cap'] for row in cases),
            'correct_tool_contracts': sum(row['correct_tool_contract'] for row in cases),
            'actual_tool_calls': sum(row['actual_tool_calls'] for row in cases),
            'reported_data_exact': sum(row['reported_data_exact'] for row in cases),
            'reported_data_equivalent_after_order_normalization': sum(
                row['reported_data_equivalent_after_order_normalization'] for row in cases),
            'reported_values_equivalent_after_order_and_scalar_normalization': sum(
                row['reported_values_equivalent_after_order_and_scalar_normalization'] for row in cases),
            'reports_with_scalar_type_differences': sum(bool(row['scalar_type_differences']) for row in cases),
            'output_tokens_including_reasoning': sum(
                row['output_tokens_including_reasoning'] or 0 for row in cases),
            'batch_wall_seconds': sum(batch['wall_seconds'] for batch in state['batches']),
            'clear_repetition_detected': sum(row['clear_repetition_detected'] for row in cases),
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--followup-phase', default='capped-temperature-1.2')
    parser.add_argument('--additional-phase', action='append', default=[])
    args = parser.parse_args()
    phases = ('capped-baseline', 'capped-candidate', args.followup_phase, *args.additional_phase)
    arms = [compare_phase(args.private_root, phase) for phase in phases]
    original = arms[0]['evidence']['case_invariants']
    stable_fields = ('id', 'original_body_sha256', 'unchanged_fields_sha256', 'max_tokens',
                     'seed', 'tokenized_input_count', 'messages', 'tool_count')
    invariants = lambda rows: [{key: row[key] for key in stable_fields} for row in rows]
    assert all(invariants(arm['evidence']['case_invariants']) == invariants(original) for arm in arms)
    assert all(arm['sampling'][key] == arms[0]['sampling'][key]
               for arm in arms for key in ('top_k', 'top_p', 'min_p'))
    assert all(arm['sampling']['repetition_penalty'] == arms[0]['sampling']['repetition_penalty']
               for arm in arms[:3])
    manifest = args.private_root / 'capped-c4-manifest-private.json'
    result = {
        'schema': 1, 'created_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'request': 'Test temperature 1.2 on the exact capped four-case fixture previously used at 0.6 and 0.85, then test its previously strong repetition-penalty 1.2 variant separately.',
        'original_report_commit': 'c869510fe0f255b44f6f2ae57371c5868af77b3f',
        'manifest_sha256': hashlib.sha256(manifest.read_bytes()).hexdigest(),
        'same_complete_fixture_across_all_arms': True,
        'primary_three_arms_same_non_temperature_settings': True,
        'additional_arm_sampling': [arm['sampling'] for arm in arms[3:]],
        'settings': {'seed': 17, 'max_output_tokens': 4096, 'top_k': 64, 'top_p': 0.9,
                     'min_p': 0.05, 'repetition_penalty': 1.1, 'mtp_depth': 2,
                     'production_context_limit': 200000, 'compaction_threshold': 100000},
        'new_requests': sum(arm['totals']['requests'] for arm in arms[2:]),
        'production_default_changed_by_experiment': False,
        'higher_temperature_extension_executed': False,
        'tools_executed': False, 'structured_decoding': False, 'tool_choice': 'auto',
        'scoreboard': [{'temperature': arm['sampling']['temperature'],
                        'repetition_penalty': arm['sampling']['repetition_penalty'],
                        **arm['totals']} for arm in arms],
        'interpretation_notes': [
            'Value-equivalent output with wrong JSON scalar types is not a fully correct tool call. Original exact contract scores remain unchanged.',
            'The repetition-penalty 1.2 follow-up returns two actual calls, but code revision is a string instead of an integer, and deployment revision/boolean/null are strings instead of their expected types. The differing fields are listed per case.',
            'The repetition-penalty 1.1 follow-up returns four completed direct reports with correct values after the documented order representation normalization, but emits no actual tool calls.',
            'These completed synthetic requests do not establish a fix for native long-context stalls or other runtime failures. Production defaults remain at the previously activated profile.',
        ],
        'scoring': {
            'correct_tool_contract': 'The actual parsed tool call must have the expected name and exact expected arguments, under the original scorer.',
            'reported_data_exact': 'Expected report data appears exactly in visible JSON or parsed tool arguments; this alone does not pass the tool-call contract.',
            'order_normalization': 'Only deployment_report planned_order may normalize a letter string to an array; the fixture did not explicitly specify its representation. This remains separate from strict contract scoring.',
            'scalar_normalization': 'An additional diagnostic uniformly converts integer strings and the strings true/false/null only where the expected synthetic field has that type. Each conversion is listed. This distinguishes correct values with wrong types from wrong values; it does not pass or alter the actual tool-call contract.',
            'completed_before_cap': 'The response finished without exhausting its output budget, independent of correctness.',
        },
        'limitations': [
            'One four-case batch per temperature and one seed are limited evidence, not a universal sampler ranking.',
            'The 1.2 follow-up ran later, after production activity and a backend restart since the original two arms; runtime/process state is not perfectly paired.',
            'Fresh cache salts and recorded metrics measure cache/isolation behavior; they do not eliminate all process-state confounds.',
            'Batch time includes prompt processing and variable-length reasoning/output; it is not fixed-work throughput.',
            'Synthetic roughly 20k cases do not reproduce the earlier 96k native incident or establish its cause.',
        ],
        'arms': arms,
        'privacy': 'Only synthetic expected/visible reports, tool arguments, hashes and metrics are included. Raw model reasoning and native content remain private.',
    }
    readiness = args.private_root / 'capped-followup-readiness-private.json'
    if readiness.exists():
        result['followup_runtime_readiness'] = json.loads(readiness.read_text())
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps([{'temperature': arm['sampling']['temperature'],
                       'repetition_penalty': arm['sampling']['repetition_penalty'],
                       **arm['totals']} for arm in arms]), flush=True)


if __name__ == '__main__':
    main()
