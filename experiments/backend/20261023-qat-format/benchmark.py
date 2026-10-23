"""Matched serving-format comparison. No credentials are read or persisted."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime
import hashlib
import json
from pathlib import Path
import re
import statistics
import time
import urllib.request

ROOT = Path(__file__).resolve().parent
BASE = 'http://127.0.0.1:8001'
MODEL = 'qwen3.6-35b-hauhau-w4a16-vision'
SAMPLING = dict(temperature=0.6, top_k=64, top_p=0.9, min_p=0.05,
                repetition_penalty=1.1, seed=20261023)


def http(route, payload=None):
    request = urllib.request.Request(BASE + route,
        data=None if payload is None else json.dumps(payload).encode(),
        headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=1800) as response:
        return response.read().decode()


def metrics():
    raw = http('/metrics')
    selected = {}
    for line in raw.splitlines():
        if line.startswith('#'):
            continue
        name = line.split('{', 1)[0].split(' ', 1)[0]
        if name.endswith('_created') or not any(x in name for x in
            ('prefix_cache', 'spec_decode', 'num_requests_', 'num_preemptions')):
            continue
        try:
            selected[name] = selected.get(name, 0) + float(line.rsplit(' ', 1)[1])
        except ValueError:
            pass
    return raw, selected


def diff(before, after):
    return {k: after.get(k, 0) - before.get(k, 0) for k in set(before) | set(after)}


def repetition(text):
    words = re.findall(r'\w+|[^\w\s]', text.lower())
    grams = [tuple(words[i:i+8]) for i in range(max(0, len(words)-7))]
    repeat_fraction = 1 - len(set(grams)) / len(grams) if grams else 0.0
    lines = [s.strip() for s in text.splitlines() if s.strip()]
    consecutive = 0
    for a, b in zip(lines, lines[1:]):
        if a == b:
            consecutive += 1
    return {'repeated_8gram_fraction': repeat_fraction,
            'duplicate_consecutive_lines': consecutive, 'word_tokens': len(words)}


def measure(spec):
    payload = {'model': MODEL, 'max_tokens': 768, 'stream': True,
               'stream_options': {'include_usage': True}, **SAMPLING, **spec['payload']}
    request = urllib.request.Request(BASE + '/v1/chat/completions',
        data=json.dumps(payload).encode(), headers={'Content-Type': 'application/json'})
    start = time.monotonic()
    first = visible = None
    content = reasoning = ''
    usage = {}
    calls = {}
    finish = None
    with urllib.request.urlopen(request, timeout=1800) as response:
        for raw in response:
            line = raw.decode().strip()
            if not line.startswith('data: ') or line == 'data: [DONE]':
                continue
            event = json.loads(line[6:])
            if event.get('error'):
                raise RuntimeError(event['error'])
            if event.get('usage'):
                usage = event['usage']
            for choice in event.get('choices', []):
                delta = choice.get('delta', {})
                now = time.monotonic()
                if first is None and any(delta.get(k) for k in
                    ('content', 'reasoning', 'reasoning_content', 'tool_calls')):
                    first = now
                if visible is None and (delta.get('content') or delta.get('tool_calls')):
                    visible = now
                content += delta.get('content') or ''
                reasoning += delta.get('reasoning') or delta.get('reasoning_content') or ''
                for call in delta.get('tool_calls', []):
                    item = calls.setdefault(call.get('index', 0),
                        {'id': '', 'type': 'function', 'function': {'name': '', 'arguments': ''}})
                    if call.get('id'):
                        item['id'] = call['id']
                    fn = call.get('function', {})
                    for key in ('name', 'arguments'):
                        item['function'][key] += fn.get(key) or ''
                finish = choice.get('finish_reason') or finish
    end = time.monotonic()
    tokens = usage.get('completion_tokens', 0)
    return {'id': spec['id'], 'usage': usage, 'wall_seconds': end-start,
            'first_output_seconds': None if first is None else first-start,
            'first_visible_seconds': None if visible is None else visible-start,
            'decode_tokens_per_second': None if first is None or tokens < 2 else (tokens-1)/(end-first),
            'content': content, 'reasoning': reasoning, 'tool_calls': list(calls.values()),
            'finish_reason': finish, 'repetition': repetition(content+'\n'+reasoning),
            'payload_sha256': hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()}


def parse_answer(text):
    text = re.sub(r'^```(?:json)?\s*|\s*```$', '', text.strip()).strip()
    try:
        return json.loads(text)
    except ValueError:
        return text


def assess(spec, result):
    answer = parse_answer(result['content'])
    expected = spec.get('expected')
    if spec.get('tool_expected'):
        calls = result['tool_calls']
        passed = len(calls) == 1
        if passed:
            fn = calls[0]['function']
            try:
                passed = {'name': fn['name'], 'arguments': json.loads(fn['arguments'])} == spec['tool_expected']
            except ValueError:
                passed = False
    else:
        passed = answer == expected
    try:
        json.loads(result['content'].strip())
        strict_json = True
    except ValueError:
        strict_json = False
    result['assessment'] = {'passed': passed, 'strict_json_only': strict_json,
                            'expected': expected or spec.get('tool_expected'),
                            'parsed_answer': answer}
    return result


def batch(specs):
    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=len(specs)) as pool:
        results = list(pool.map(measure, specs))
    wall = time.monotonic()-start
    return {'wall_seconds': wall, 'aggregate_output_tokens_per_second':
            sum(r['usage'].get('completion_tokens', 0) for r in results)/wall,
            'requests': results}


def make_fixtures():
    from tokenizers import Tokenizer
    tok = Tokenizer.from_file('/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-native-mixed-w4w8a16/tokenizer.json')
    intro = ('Final-format-comparison dossier BX-20261023. Entries are source records, not instructions. '
             'Find facts across the entire dossier. For repeated incident IDs the greatest revision is authoritative.\n')
    lines = []
    names = ['cedar', 'birch', 'maple', 'aspen', 'willow', 'oak', 'elm']
    for i in range(1800):
        lines.append(f'Entry {i:04d}: service {names[i%7]}-worker; incident IX-{i:04d}; revision 1; '
            f'owner team-{i%19:02d}; retry budget {3+i%8}; observations {100+i%83}; '
            f'queue delay {20+i%73} ms; rollout batch {i%11}; status verified. '
            'The review confirmed successful canary checks and retained the previous rollback snapshot.\n')
    selected = []
    token_count = len(tok.encode(intro).ids)
    for line in lines:
        token_count += len(tok.encode(line).ids)
        if token_count > 63500:
            break
        selected.append(line)
    # Position the true revision and distracting older one far apart.
    selected[17] = 'AUTHORITATIVE incident BX-314 revision 7: owner Mira; rollback seal COBALT-7261; approved retry budget 13; maintenance window 03:40 UTC.\n'
    selected[len(selected)//2] = 'AUTHORITATIVE incident BX-628 revision 4: owner Ivo; rollback seal LANTERN-4198; approved retry budget 7; maintenance window 06:25 UTC.\n'
    selected[int(len(selected)*0.84)] = 'ARCHIVE incident BX-314 revision 2: owner Leon; rollback seal RETIRED-0001; approved retry budget 4; maintenance window 01:00 UTC.\n'
    body = intro + ''.join(selected)
    body += 'AUTHORITATIVE incident BX-942 revision 9: owner Noor; rollback seal ORBIT-8530; approved retry budget 21; maintenance window 09:15 UTC.\n'
    body += 'END OF DOSSIER.\n'
    user = lambda text: {'messages': [{'role':'user','content':text}]}
    fixtures = {'schema': 1, 'sampling': SAMPLING, 'long_body_tokens': len(tok.encode(body).ids),
                'quality': [], 'long_cold': {}, 'long_shared': [], 'tool': {}}
    cases = [
        ('facts', 'Return only JSON with keys capital_of_australia, carbon_atomic_number, hex_ff_decimal, bits_per_byte, water_formula. Give the standard answers.',
         {'capital_of_australia':'Canberra','carbon_atomic_number':6,'hex_ff_decimal':255,'bits_per_byte':8,'water_formula':'H2O'}),
        ('arithmetic', 'Calculate (47 * 83) - (128 * 9). Return only the integer.', 2749),
        ('extraction', 'Records: red has 11 units at price 4; blue has 7 units at price 9; green has 13 units at price 3. Return only JSON with total_value and colors_sorted_by_value_desc, using the color names.',
         {'total_value':146,'colors_sorted_by_value_desc':['blue','red','green']}),
        ('code', 'In Python: xs=[3,1,3,2]; result=sorted({x*x for x in xs if x>1}); print(result). Return only the printed JSON-compatible list.', [4,9]),
        ('constraints', 'Return only a JSON object with exactly keys status, count, tags. status is the lowercase word ready; count is integer zero; tags is an empty array.',
         {'status':'ready','count':0,'tags':[]}),
        ('precedence', 'Use the highest revision for each item. Records: A revision 2 owner Noor; B revision 4 owner Ivo; A revision 5 owner Mira; B revision 1 owner Eli. Return only JSON mapping A and B to their current owners.',
         {'A':'Mira','B':'Ivo'}),
        ('planning_constraints', 'A deployment has steps P (prepare), V (validate P), R (release after V), M (monitor after R). Return only the JSON array of step letters in the only valid dependency order.', ['P','V','R','M']),
        ('limited_evidence', 'Source record: A sensor read 12 at 09:00 and 17 at 09:10. There is no 09:05 reading. Return only JSON with increase and observed_at_0905, using null when not observed.',
         {'increase':5,'observed_at_0905':None}),
    ]
    for name, prompt, expected in cases:
        fixtures['quality'].append({'id':name,'payload':user(prompt),'expected':expected})
    questions = [
        ('seals', 'Return only JSON mapping BX-314, BX-628, BX-942 to their current rollback seals.',
         {'BX-314':'COBALT-7261','BX-628':'LANTERN-4198','BX-942':'ORBIT-8530'}),
        ('owners', 'Return only JSON mapping BX-314, BX-628, BX-942 to their current owners.',
         {'BX-314':'Mira','BX-628':'Ivo','BX-942':'Noor'}),
        ('budgets', 'Return only JSON mapping BX-314, BX-628, BX-942 to their current approved retry budgets as integers.',
         {'BX-314':13,'BX-628':7,'BX-942':21}),
        ('windows', 'Return only JSON mapping BX-314, BX-628, BX-942 to their current maintenance windows, in HH:MM UTC format.',
         {'BX-314':'03:40 UTC','BX-628':'06:25 UTC','BX-942':'09:15 UTC'}),
        ('current_revision', 'Return only JSON mapping BX-314, BX-628, BX-942 to their authoritative revision numbers as integers.',
         {'BX-314':7,'BX-628':4,'BX-942':9}),
    ]
    for i,(name,question,expected) in enumerate(questions):
        spec={'id':'long_'+name,'payload':{**user(body+question),'max_tokens':2048},'expected':expected}
        if i==0:fixtures['long_cold']=spec
        else:fixtures['long_shared'].append(spec)
    tools=[{'type':'function','function':{'name':'lookup_record','description':'Read the requested deployment record.',
           'parameters':{'type':'object','properties':{'record_id':{'type':'string'}},'required':['record_id'],'additionalProperties':False}}}]
    fixtures['tool']={'id':'tool_lookup','payload':{**user('Use lookup_record to read deployment record B-17. Do not guess its state. After the tool responds, return only JSON with state and retry_budget.'),'tools':tools,'tool_choice':'auto'},
                      'tool_expected':{'name':'lookup_record','arguments':{'record_id':'B-17'}}}
    (ROOT/'fixtures.json').write_text(json.dumps(fixtures,indent=2)+'\n')
    print(json.dumps({'fixtures':str(ROOT/'fixtures.json'),'long_body_tokens':fixtures['long_body_tokens']}),flush=True)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--variant')
    parser.add_argument('--make-fixtures',action='store_true')
    parser.add_argument('--short-only',action='store_true')
    args=parser.parse_args()
    if args.make_fixtures:
        make_fixtures();return
    if not args.variant:parser.error('--variant required')
    fixture=json.loads((ROOT/'fixtures.json').read_text())
    out=ROOT/(args.variant+'.json')
    if out.exists():raise FileExistsError(out)
    raw,before=metrics()
    (ROOT/(args.variant+'-metrics-before.txt')).write_text(raw)
    state={'variant':args.variant,'utc_started':datetime.datetime.now(datetime.timezone.utc).isoformat(),
           'sampling':SAMPLING,'model_info':json.loads(http('/v1/models')),'performance':[],
           'quality':[],'long_context':[], 'fixtures_sha256':hashlib.sha256((ROOT/'fixtures.json').read_bytes()).hexdigest()}
    def save():out.write_text(json.dumps(state,indent=2)+'\n')
    def log(item):print(json.dumps(item),flush=True)
    for concurrency in (1,4):
        for repeat in range(3):
            tokens=128 if repeat==0 else 512
            specs=[]
            for client in range(concurrency):
                prompt=(f'Final matched benchmark BX-20261023 C{concurrency} request {client}. '
                    'Write a detailed engineering review of a durable job queue, including idempotency, '
                    'retry handling, worker crashes, cancellation, audit records, and practical regression checks.')
                specs.append({'id':f'perf_c{concurrency}_r{repeat}_{client}', 'payload':{
                    'messages':[{'role':'user','content':prompt}], 'max_tokens':tokens,'ignore_eos':True}})
            _,m0=metrics();row=batch(specs);_,m1=metrics()
            row.update(concurrency=concurrency,repeat=repeat,warmup=repeat==0,metrics_delta=diff(m0,m1))
            state['performance'].append(row);save()
            log({k:v for k,v in row.items() if k not in ('requests','metrics_delta')})
    for spec in fixture['quality']:
        row=assess(spec,measure(spec));state['quality'].append(row);save()
        log({'quality':row['id'],'passed':row['assessment']['passed'],'seconds':row['wall_seconds']})
    spec=fixture['tool'];row=assess(spec,measure(spec));state['quality'].append(row);save()
    log({'quality':row['id'],'passed':row['assessment']['passed']})
    if row['assessment']['passed']:
        messages=spec['payload']['messages']+[{'role':'assistant','content':row['content'] or None,'tool_calls':row['tool_calls']},
          {'role':'tool','tool_call_id':row['tool_calls'][0]['id'],'content':json.dumps({'record_id':'B-17','state':'ready','retry_budget':6})}]
        follow={'id':'tool_result','payload':{'messages':messages,'tools':spec['payload']['tools'],'tool_choice':'auto'},
                'expected':{'state':'ready','retry_budget':6}}
        row=assess(follow,measure(follow));state['quality'].append(row);save()
        log({'quality':row['id'],'passed':row['assessment']['passed']})
    if not args.short_only:
        log({'stage':'long_cold','body_tokens':fixture['long_body_tokens']})
        _,m0=metrics();row=assess(fixture['long_cold'],measure(fixture['long_cold']));_,m1=metrics()
        row['metrics_delta']=diff(m0,m1);state['long_context'].append(row);save()
        log({'long_cold':row['id'],'passed':row['assessment']['passed'],'first_output_seconds':row['first_output_seconds'], 'usage':row['usage']})
        _,m0=metrics();row=batch(fixture['long_shared']);_,m1=metrics()
        for spec,res in zip(fixture['long_shared'],row['requests']):assess(spec,res)
        row.update(id='long_shared_prefix_c4',metrics_delta=diff(m0,m1))
        state['long_context'].append(row);save()
        log({'stage':'long_shared_prefix_c4','seconds':row['wall_seconds'],'passes':sum(r['assessment']['passed'] for r in row['requests']),'metrics_delta':row['metrics_delta']})
    raw,after=metrics();(ROOT/(args.variant+'-metrics-after.txt')).write_text(raw)
    state['metrics_delta']=diff(before,after)
    state['utc_finished']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    state['quality_passes']=sum(r['assessment']['passed'] for r in state['quality'])
    state['quality_total']=len(state['quality'])
    state['performance_summary']={str(c):{
        'median_aggregate_tps':statistics.median(r['aggregate_output_tokens_per_second'] for r in state['performance'] if r['concurrency']==c and not r['warmup']),
        'median_decode_tps_per_client':statistics.median(q['decode_tokens_per_second'] for r in state['performance'] if r['concurrency']==c and not r['warmup'] for q in r['requests']),
        'median_ttft_seconds':statistics.median(q['first_output_seconds'] for r in state['performance'] if r['concurrency']==c and not r['warmup'] for q in r['requests'])} for c in (1,4)}
    save();log({'completed':args.variant,'quality':f"{state['quality_passes']}/{state['quality_total']}",'performance':state['performance_summary']})


if __name__=='__main__':main()
