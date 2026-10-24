#!/usr/bin/env python3
"""Publish sampler metrics without prompts, model reasoning, tool arguments, or auth."""
import argparse
import copy
import datetime
import hashlib
import json
from pathlib import Path
import re

from sampling_sweep import summarize


def rows(state):
    return [row for batch in state['batches'] for row in batch['requests']]


def compact(state):
    result = {k: state[k] for k in ('phase', 'started_at', 'ended_at', 'fixture_sha256',
                                    'samplers', 'sampler', 'matched_seeds', 'case_invariants',
                                    'scoring_notes', 'schedule', 'schedule_adjustment',
                                    'completed_temperature_blocks', 'unexecuted_planned_temperature_blocks',
                                    'limitations') if k in state}
    result['arms'] = state.get('arms', {})
    result['batches'] = [{k: batch[k] for k in ('batch', 'arms', 'concurrency', 'wall_seconds',
                                               'metrics_delta', 'isolation')} for batch in state['batches']]
    result['requests'] = rows(state)
    result['clear_repetition_count'] = sum(row.get('clear_repetition', False) for row in rows(state))
    result['output_budget_exhaustions'] = [{'arm': row['arm'], 'case': row['case'], 'seed': row['seed'],
                                          'max_tokens': row['request_sampling']['max_tokens']}
                                         for row in rows(state) if 'length' in row.get('finish_reasons', [])]
    result['all_batches_accounted_for_own_requests'] = all(
        not batch['isolation']['timing_contaminated_or_accounting_incomplete'] for batch in state['batches'])
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--private-root', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--candidate', type=Path)
    args = p.parse_args()
    states = {}
    for phase in ('temperature', 'budget-supplement', 'temperature-1.2', 'penalties-1.2', 'temperature-descent',
                  'temperature-descent-rest', 'creativity-0.9', 'creativity-prior-temperatures',
                  'topk-1.2', 'factors', 'capped-baseline', 'capped-candidate'):
        path = args.private_root / phase / 'summary.json'
        if path.exists():
            states[phase] = json.loads(path.read_text())
    result = {'schema': 1, 'created_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'scope': {'user_requested': 'Explore higher temperature, top-k, and repetition penalty; cap test contexts around 20k total tokens.',
                  'max_model_len_unchanged': 200000, 'compaction_threshold_unchanged': 100000,
                  'mtp_enabled': True, 'mtp_depth_unchanged': 2,
                  'executed_sampler_axes': ['temperature', 'repetition_penalty'],
                  'fixed_sampling': {'top_k': 64, 'top_p': 0.9, 'min_p': 0.05},
                  'top_k_sweep_executed': False,
                  'fixed_work_speed_comparison_executed': False,
                  'server_defaults_changed_by_experiment': False,
                  'production_native_clients_paused_during_experiment': True,
                  'native_prompts_outputs_reasoning_and_tool_arguments_published': False,
                  'model_reasoning_published': False,
                  'synthetic_prompt_definitions_published': True,
                  'selected_synthetic_incorrect_visible_answers_published': True,
                  'structured_decoding_used': False,
                  'tool_choice': 'auto', 'tool_validation': 'Post hoc exact answer and JSON Schema validation.',
                  'tools_executed': False},
        'phases': {key: compact(state) for key, state in states.items()},
        'interpretation_limits': ['Small deterministic fixture panel and two seeds are not an optimal-sampler proof.',
            'Initial code/arithmetic budget exhaustions remain reported; the matched supplement raises their budgets and batches one request from each temperature together.',
            'Correctness after removing a JSON Markdown fence is separate from strict JSON-only formatting compliance.',
            'The capped synthetic dossier batch does not reproduce the earlier 29k–96k native requests or prove their loop cause.',
            'All samples use one shared MTP2 process; changes in age, scheduling and batch composition can affect results.',
            'Varying completion lengths make elapsed time descriptive, not a fixed-work speed comparison.',
            'Absence of detected repetition in this bounded panel does not establish a general repetition fix.']}
    result['synthetic_semantic_failure_examples'] = []
    result['synthetic_tool_failure_examples'] = []
    for phase, state in states.items():
        plan_path = args.private_root / phase / 'run-plan-private.json'
        if not plan_path.exists():
            continue
        plan = json.loads(plan_path.read_text())
        for batch in state['batches']:
            jobs = {job['id']: job for job in plan['batches'][batch['batch']]}
            for row in batch['requests']:
                assessment = row.get('assessment', {})
                if row.get('completed') and not assessment.get('passed') and assessment.get('kind') == 'tool':
                    path = args.private_root / phase / f'batch-{batch["batch"]:02d}' / row['id'] / 'response-private.json'
                    tools = json.loads(path.read_text()).get('tools', {})
                    result['synthetic_tool_failure_examples'].append({
                        'phase': phase, 'arm': row['arm'], 'case': row['case'], 'seed': row['seed'],
                        'expected': jobs[row['id']]['case']['tool_expected'],
                        'actual': [{'name': tool['name'], 'arguments_text': tool['arguments']} for tool in tools.values()]})
                if not row.get('completed') or assessment.get('passed') or assessment.get('kind') != 'exact':
                    continue
                path = args.private_root / phase / f'batch-{batch["batch"]:02d}' / row['id'] / 'response-private.json'
                visible = json.loads(path.read_text())['content'].strip()
                visible = re.sub(r'^```(?:json)?\s*|\s*```$', '', visible).strip()
                try:
                    actual = json.loads(visible)
                except ValueError:
                    continue
                result['synthetic_semantic_failure_examples'].append({
                    'phase': phase, 'arm': row['arm'], 'case': row['case'], 'seed': row['seed'],
                    'expected': jobs[row['id']]['case']['expected'], 'actual': actual})
    all_rows = [row for state in states.values() for row in rows(state)]
    all_batches = [batch for state in states.values() for batch in state['batches']]
    result['research_cost_and_integrity'] = {
        'request_count_including_calibration': len(all_rows),
        'total_output_tokens_including_calibration': sum(row.get('usage', {}).get('completion_tokens', 0) for row in all_rows),
        'all_final_usage_available': all(bool(row.get('usage')) for row in all_rows),
        'clear_repetition_count': sum(row.get('clear_repetition', False) for row in all_rows),
        'timing_contaminated_or_incomplete_accounting_batches': sum(batch['isolation']['timing_contaminated_or_accounting_incomplete'] for batch in all_batches),
        'selected_metric_deltas': {name: sum(batch['metrics_delta'].get(name, 0) for batch in all_batches)
            for name in ('vllm:prefix_cache_hits_total', 'vllm:external_prefix_cache_hits_total',
                         'vllm:kv_offload_load_bytes_total', 'vllm:num_preemptions_total')},
        'length_finished_but_semantically_correct_objective_cases': [
            {'arm': row['arm'], 'case': row['case'], 'seed': row['seed']} for row in all_rows
            if 'length' in row.get('finish_reasons', []) and row.get('assessment', {}).get('passed') and
            row['assessment']['kind'] in ('exact', 'tool')]}
    if 'temperature' in states and 'budget-supplement' in states:
        initial = states['temperature']
        supplementary = states['budget-supplement']
        replaced = {(row['arm'], row['seed'], row['case']) for row in rows(supplementary)}
        kept = [row for row in rows(initial) if (row['arm'], row['seed'], row['case']) not in replaced]
        adjusted = copy.deepcopy(initial)
        adjusted['batches'] = [{'requests': kept + rows(supplementary)}]
        result['normalized_short_panel'] = {
            'description': 'All seven tasks at both seeds. Original arithmetic/code entries are replaced by their uniform 4096-token supplement with matched cross-temperature C4 batches; every other entry is unchanged. Initial outcomes are retained in phases.temperature. Batch composition differs from the initial panel.',
            'arms': summarize(adjusted)['arms']}
        if 'temperature-1.2' in states:
            result['normalized_short_panel']['arms'].update(states['temperature-1.2']['arms'])
        if 'temperature-descent' in states:
            result['normalized_short_panel']['arms'].update(states['temperature-descent']['arms'])
        if 'temperature-descent-rest' in states:
            rest = copy.deepcopy(states['temperature-descent-rest'])
            for batch in rest['batches']:
                batch['requests'] = [row for row in batch['requests'] if row.get('assessment', {}).get('kind') != 'creative']
            result['normalized_short_panel']['arms'].update(summarize(rest)['arms'])
        scoreboard = []
        for arm, scores in result['normalized_short_panel']['arms'].items():
            if scores['requests'] == 14:
                scoreboard.append({'temperature': float(arm[1:]), 'repetition_penalty': 1.1,
                    'top_k': 64, 'top_p': 0.9, 'min_p': 0.05,
                    'correct_objective_answers': scores['objective_passes'], 'objective_total': scores['objective_total'],
                    'completed_tasks': scores['completed'], 'task_total': scores['requests'],
                    'total_output_tokens': scores['completion_tokens']})
        result['temperature_scoreboard'] = sorted(scoreboard, key=lambda row: -row['temperature'])
        penalties = []
        if 'penalties-1.2' in states:
            for arm, scores in states['penalties-1.2']['arms'].items():
                if scores['requests'] == 14:
                    penalties.append({'temperature': 1.2, 'repetition_penalty': float(arm[2:]),
                        'top_k': 64, 'top_p': 0.9, 'min_p': 0.05,
                        'correct_objective_answers': scores['objective_passes'], 'objective_total': scores['objective_total'],
                        'completed_tasks': scores['completed'], 'task_total': scores['requests'],
                        'total_output_tokens': scores['completion_tokens']})
        result['penalty_scoreboard'] = penalties
        result['ranked_conditions'] = sorted(scoreboard + penalties, key=lambda row: (
            -row['correct_objective_answers'], -row['completed_tasks'], row['total_output_tokens']))
        result['ranking_rule'] = {
            'priority': ['More correct objective answers', 'More completed tasks', 'Fewer total output tokens'],
            'objective_answers': 'Twelve exact-answer or exact-tool checks across two seeds. Markdown fences do not change semantic correctness.',
            'completed_tasks': 'Finished before the fixed output cap; correctness is scored separately. Fourteen requests include two open designs.',
            'total_output_tokens': 'Sum of API usage.completion_tokens across the same 14 selected requests, including reasoning, visible content, tool output, and tokens spent on failures/capped responses.',
            'calibration_cost': 'Original budget-exhausted rows replaced by the matched supplement are retained in the research phases but are not double-counted in a condition.'}
        creative = {}
        for state in states.values():
            for row in rows(state):
                if row.get('assessment', {}).get('kind') != 'creative':
                    continue
                temperature = row['request_sampling']['temperature']
                entry = creative.setdefault(temperature, {'temperature': temperature, 'samples': 0,
                    'completed_before_output_cap': 0, 'total_output_tokens': 0, 'visible_word_counts': [],
                    'objective_correctness_scored': False})
                entry['samples'] += 1
                entry['completed_before_output_cap'] += int(row['completed'])
                entry['total_output_tokens'] += row.get('usage', {}).get('completion_tokens', 0)
                entry['visible_word_counts'].append(row['assessment']['word_count'])
        result['creativity_cost_and_completion'] = sorted(creative.values(), key=lambda row: -row['temperature'])
        result['consistent_16_request_totals'] = []
        for row in scoreboard:
            extra = creative.get(row['temperature'])
            if extra and extra['samples'] == 2:
                result['consistent_16_request_totals'].append(dict(row, task_total=16,
                    completed_tasks=row['completed_tasks'] + extra['completed_before_output_cap'],
                    total_output_tokens=row['total_output_tokens'] + extra['total_output_tokens']))
    manifest_path = args.private_root / 'capped-c4-manifest-private.json'
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        result['capped_fixture'] = {'manifest_sha256': hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
                                   'fixture_type': manifest['fixture_type'],
                                   'cases': [{k: row[k] for k in ('id', 'sha256', 'tokenized_input_count', 'total_tokens_at_full_output_budget')}
                                             for row in manifest['cases']]}
    if args.candidate:
        result['candidate_sampling'] = json.loads(args.candidate.read_text())
        validation = states.get('capped-candidate', {})
        validation_rows = rows(validation) if validation.get('batches') else []
        result['selected_profile'] = {
            'sampling': result['candidate_sampling'],
            'selection_basis': 'Best observed objective accuracy/completion, lowest output cost among those tied leaders, and the highest blind creativity mean in this small panel.',
            'validation_complete': len(validation_rows) == 4,
            'validation_correct': sum(row.get('assessment', {}).get('passed', False) for row in validation_rows),
            'validation_completed_before_cap': sum(row.get('completed', False) for row in validation_rows),
            'capped_tool_contract_passed': len(validation_rows) == 4 and all(row.get('assessment', {}).get('passed', False) for row in validation_rows),
            'capped_outcome_interpretation': 'The selected profile remains the overall choice from the matched quality/cost and blind creativity panel. Both selected and baseline profiles fail the strict capped correct-tool contract; this is retained as a limitation, not relabeled as a passed validation. The candidate also exhausts the code output budget.',
            'optimality_claim': 'Best observed profile in this small panel, not a universal optimum or a proven repetition fix.',
            'production_activation_owned_by': 'Root/backend agent; this experiment does not mutate defaults.',
            'conditional_1_3_1_4_extension_triggered': False,
            'conditional_extension_reason': 'Temperature 1.2 did not win the requested quality/completion/output comparison.'}
    result['capped_visible_output_diagnostics'] = {}
    for phase in ('capped-candidate', 'capped-baseline'):
        if phase not in states:
            continue
        plan = json.loads((args.private_root / phase / 'run-plan-private.json').read_text())
        jobs = {job['id']: job for batch in plan['batches'] for job in batch}
        diagnostics = []
        for batch in states[phase]['batches']:
            for row in batch['requests']:
                path = args.private_root / phase / f'batch-{batch["batch"]:02d}' / row['id'] / 'response-private.json'
                response = json.loads(path.read_text())
                visible = re.sub(r'^```(?:json)?\s*|\s*```$', '', response.get('content', '').strip()).strip()
                try:
                    actual = json.loads(visible)
                except ValueError:
                    actual = None
                expected = jobs[row['id']]['case']['tool_expected']['arguments']
                normalized = copy.deepcopy(actual)
                ambiguous_order_type = False
                if row['case'] == 'deployment_report' and isinstance(normalized, dict):
                    report = normalized.get('report', {})
                    if isinstance(report.get('planned_order'), str):
                        ambiguous_order_type = True
                        report['planned_order'] = list(report['planned_order'])
                diagnostics.append({'case': row['case'], 'completed_before_cap': row['completed'],
                    'parsed_tool_calls': len(response.get('tools', {})),
                    'visible_json_report': actual,
                    'synthetic_tool_calls': [{'name': call['name'], 'arguments_text': call['arguments']}
                                             for call in response.get('tools', {}).values()],
                    'visible_json_matches_expected_report_arguments': actual == expected,
                    'planned_order_string_vs_array_ambiguity': ambiguous_order_type,
                    'visible_report_equivalent_after_that_representation_normalization': normalized == expected})
        result['capped_visible_output_diagnostics'][phase] = {
            'cases': diagnostics,
            'actual_parsed_tool_call_count': sum(row['parsed_tool_calls'] for row in diagnostics),
            'strict_correct_tool_contract_passes': sum(row.get('assessment', {}).get('passed', False) for row in rows(states[phase])),
            'note': 'These read-only diagnostics do not change the failed tool-call assessment. The deployment prompt asks for an order but does not explicitly require an array; a correct letter string is a representation ambiguity rather than a demonstrated factual error. No fixture or sampling input was changed.'}
    review_path = args.private_root / 'creativity-blind-review.json'
    map_path = args.private_root / 'creativity-blind-map-private.json'
    if review_path.exists() and map_path.exists():
        review = json.loads(review_path.read_text())
        mapping = json.loads(map_path.read_text())
        grouped = {}
        for row in review.get('reviews', []):
            if row['label'] not in mapping:
                continue
            grouped.setdefault(mapping[row['label']]['temperature'], []).append(row)
        result['blind_creativity_review'] = {
            'status': review['status'], 'scale': review['scale'], 'grading_note': review['grading_note'],
            'originality_finalization': review['originality_finalization'],
            'aggregates': [{'temperature': temperature, 'sample_count': len(samples),
                            'mean_total_out_of_16': sum(row['total'] for row in samples) / len(samples),
                            'min_total_out_of_16': min(row['total'] for row in samples),
                            'max_total_out_of_16': max(row['total'] for row in samples),
                            'max_originality_out_of_4': max(row['scores']['originality'] for row in samples)}
                           for temperature, samples in sorted(grouped.items(), reverse=True)],
            'interpretation': 'Modest variation among useful sketches; no originality score exceeds 2/4. Two samples and one blind qualitative reviewer do not establish a universal creativity optimum.'}
    result['fixed_work_speed'] = {}
    for arm in ('candidate', 'baseline'):
        path = args.private_root / ('speed-' + arm) / 'summary.json'
        if path.exists():
            result['fixed_work_speed'][arm] = json.loads(path.read_text())
    result['visible_design_review'] = {
        'reviewed_by': 'Root agent', 'seed': 17, 'temperatures': [0.6, 1.0],
        'observation': 'Both are coherent. Temperature 1.0 gives an explicit exponential-backoff formula and clearer in-flight cancellation limits; 0.6 explicitly records every transition and tests the payment-success/crash boundary more directly.',
        'conclusion': 'No universal quality winner from one example; no observed readability collapse at 1.0. This is qualitative evidence, not a creativity score.',
        'temperature_1_2_review': {'seed_29': 'Coherent and explicitly reuses the same payment idempotency key.',
                                  'seed_17': 'Weaker than the reviewed 1.0 answer: categorically calls payment irreversible without support; the audit section says to check the API or log rather than explicitly retry with the same idempotency key.',
                                  'conclusion': 'Mixed qualitative quality. Passing headings and word count does not establish semantic correctness or a general improvement.'},
        'visible_only_examples_private_directory': str(args.private_root / 'visible-examples')}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'published': str(args.output), 'phases': list(states),
                      'normalized_short_panel': result.get('normalized_short_panel', {}).get('arms')}), flush=True)


if __name__ == '__main__':
    main()
