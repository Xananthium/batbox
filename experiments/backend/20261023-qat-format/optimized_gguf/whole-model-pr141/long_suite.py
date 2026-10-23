"""Continue the unchanged benchmark's long suite without repeating short tests."""
import datetime
import hashlib
import json
from pathlib import Path
import benchmark as b

root=Path(__file__).resolve().parent
out=root/'upstream_pr141_long.json'
assert not out.exists(), 'Preserve the first matched trial artifact'
fixture=json.loads((root/'fixtures.json').read_text())
raw,before=b.metrics()
(root/'upstream_pr141-long-metrics-before.txt').write_text(raw)
state={'utc_started':datetime.datetime.now(datetime.timezone.utc).isoformat(),
       'sampling':b.SAMPLING,'fixtures_sha256':hashlib.sha256((root/'fixtures.json').read_bytes()).hexdigest(),
       'cache_state':'63k dossier not previously requested on this PR141 process; short tests completed',
       'long_context':[]}

def save():
    out.write_text(json.dumps(state,indent=2)+'\n')

save()
_,m0=b.metrics()
row=b.assess(fixture['long_cold'],b.measure(fixture['long_cold']))
_,m1=b.metrics()
row['metrics_delta']=b.diff(m0,m1)
state['long_context'].append(row)
save()
print(json.dumps({'stage':'cold','passed':row['assessment']['passed'],
                  'first_output_seconds':row['first_output_seconds'],'usage':row['usage']}),flush=True)
_,m0=b.metrics()
row=b.batch(fixture['long_shared'])
_,m1=b.metrics()
for spec,result in zip(fixture['long_shared'],row['requests']):
    b.assess(spec,result)
row.update(id='long_shared_prefix_c4',metrics_delta=b.diff(m0,m1))
state['long_context'].append(row)
raw,after=b.metrics()
(root/'upstream_pr141-long-metrics-after.txt').write_text(raw)
state.update(utc_finished=datetime.datetime.now(datetime.timezone.utc).isoformat(),
             metrics_delta=b.diff(before,after),
             exact_answer_passes=int(state['long_context'][0]['assessment']['passed'])+
                                sum(r['assessment']['passed'] for r in row['requests']),
             exact_answer_total=5)
save()
print(json.dumps({'stage':'shared','wall_seconds':row['wall_seconds'],
                  'passes':sum(r['assessment']['passed'] for r in row['requests']),
                  'metrics_delta':row['metrics_delta']}),flush=True)
print(json.dumps({'completed':True,'exact_answer_passes':state['exact_answer_passes'],'exact_answer_total':5}),flush=True)
