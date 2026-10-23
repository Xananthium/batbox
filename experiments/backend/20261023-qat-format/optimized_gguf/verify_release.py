"""Verify the promoted persistent release, rolling back on failed activation."""
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import urllib.request

ROOT=Path(__file__).resolve().parent
RELEASE=Path('/home/isnotreal/model-tools/gguf-pr141-releases/20261023-e62877c-e3cefbc')
OUT=ROOT/'promoted-release-verification.json'
BACKUP=Path('/home/isnotreal/model-tools/backend-qat-backup-20261023')
SAMPLING=dict(temperature=0.6,top_p=0.9,top_k=64,min_p=0.05,repetition_penalty=1.1,seed=20261023)


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def http(route,payload=None):
    req=urllib.request.Request('http://127.0.0.1:8001'+route,
        data=None if payload is None else json.dumps(payload).encode(),
        headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=180) as response:
        raw=response.read()
    return json.loads(raw) if raw else None


def main():
    state={'utc_started':utc(),'release':str(RELEASE),'sampling':SAMPLING,'requests':[]}
    def save():OUT.write_text(json.dumps(state,indent=2)+'\n')
    manifest=json.loads((RELEASE/'release.json').read_text())
    for name,expected in manifest['sha256'].items():
        assert hashlib.sha256((RELEASE/name).read_bytes()).hexdigest()==expected,name
    assert Path(manifest['persistent_override']).read_bytes()==(RELEASE/'qwen-vllm.conf').read_bytes()
    assert not Path(manifest['transient_override']).exists()
    baseline=json.loads((RELEASE/'rollback/baseline.json').read_text())
    for row in baseline:
        assert hashlib.sha256(Path(row['path']).read_bytes()).hexdigest()==row['sha256'],row['path']
    state['baseline_persistent_files_unchanged']=len(baseline)
    state['release_manifest_sha256']=hashlib.sha256((RELEASE/'release.json').read_bytes()).hexdigest()
    state['extension_sha256']=manifest['extension_sha256']
    save()
    start=time.monotonic()
    while True:
        values=subprocess.check_output(['systemctl','show','qwen-vllm.service','-p','ActiveState','-p','NRestarts'],text=True)
        props=dict(x.split('=',1) for x in values.splitlines())
        assert props['ActiveState'] in ('active','activating'),props
        assert int(props['NRestarts'])==0,props
        try:
            with urllib.request.urlopen('http://127.0.0.1:8001/health',timeout=2) as response:
                if response.status==200:break
        except Exception:
            pass
        assert time.monotonic()-start<900,'Release failed to become ready within15min'
        time.sleep(5)
    state['ready_utc']=utc()
    pid=int(subprocess.check_output(['systemctl','show','qwen-vllm.service','--property=MainPID','--value'],text=True))
    actual=[s.decode() for s in Path(f'/proc/{pid}/cmdline').read_bytes().rstrip(b'\0').split(b'\0')]
    expected=json.loads((RELEASE/'args.json').read_text())
    assert actual[-len(expected):]==expected,(actual,expected)
    env=dict(s.decode().split('=',1) for s in Path(f'/proc/{pid}/environ').read_bytes().rstrip(b'\0').split(b'\0'))
    base_env=json.loads((BACKUP/'process-environment.json').read_text())
    volatile={'INVOCATION_ID','JOURNAL_STREAM','SYSTEMD_EXEC_PID','MEMORY_PRESSURE_WATCH','MEMORY_PRESSURE_WRITE'}
    for key,value in base_env.items():
        if key not in volatile|{'VLLM_PLUGINS'}:assert env[key]==value,key
    assert env['PYTHONPATH']==str(RELEASE/'python')
    assert env['VLLM_PLUGINS']=='gguf' and env['VLLM_GGUF_CUDA_KERNEL']=='upstream'
    state.update(pid=pid,args=actual,environment={k:env[k] for k in ['CUDA_VISIBLE_DEVICES','VLLM_USE_V2_MODEL_RUNNER',
        'VLLM_TQ_UNIFIED_ATTN','VLLM_KV_CACHE_LAYOUT','VLLM_USE_FLASHINFER_SAMPLER','PYTHONPATH','VLLM_PLUGINS','VLLM_GGUF_CUDA_KERNEL']})
    state['model_info']=http('/v1/models')
    info=state['model_info']['data'][0]
    assert info['root']==expected[2]
    assert info['max_model_len']==200000
    fixture=json.loads((ROOT/'whole-model-pr141/fixtures.json').read_text())
    payload={'model':'qwen3.6-35b-hauhau-w4a16-vision','max_tokens':768,**SAMPLING,**fixture['tool']['payload']}
    first_started=utc();first=http('/v1/chat/completions',payload)
    state['requests'].append({'started_utc':first_started,'finished_utc':utc(),'response':first})
    message=first['choices'][0]['message'];calls=message['tool_calls']
    assert len(calls)==1 and calls[0]['function']['name']=='lookup_record'
    assert json.loads(calls[0]['function']['arguments'])=={'record_id':'B-17'}
    messages=payload['messages']+[{'role':'assistant','content':message.get('content'),'tool_calls':calls},
        {'role':'tool','tool_call_id':calls[0]['id'],'content':json.dumps({'record_id':'B-17','state':'ready','retry_budget':6})}]
    second_started=utc();second=http('/v1/chat/completions',payload|{'messages':messages})
    state['requests'].append({'started_utc':second_started,'finished_utc':utc(),'response':second})
    final=second['choices'][0]['message']
    sys.path.insert(0,str(ROOT/'whole-model-pr141'))
    from benchmark import parse_answer
    assert parse_answer(final['content'])=={'state':'ready','retry_budget':6}
    assert second['choices'][0]['finish_reason']=='stop'
    reasoning=(message.get('reasoning') or message.get('reasoning_content') or '')+(final.get('reasoning') or final.get('reasoning_content') or '')
    assert reasoning,'Default thinking must remain available'
    state.update(passed=True,reasoning_chars=len(reasoning),utc_finished=utc(),
                 settings_verified='200k,TP2,TQ4,80GiB offload,prefix cache,MTP2,vision projector,sampler.6/.9/64/.05/1.1')
    save()
    print(json.dumps({'passed':True,'pid':pid,'ready_utc':state['ready_utc'],'reasoning_chars':len(reasoning),'tool_roundtrip':True}),flush=True)


if __name__=='__main__':
    try:
        main()
    except BaseException as error:
        failure={'utc':utc(),'error':repr(error),'action':'restoring original converted MTP2 service'}
        (ROOT/'promoted-release-failure.json').write_text(json.dumps(failure,indent=2)+'\n')
        print(json.dumps(failure),flush=True)
        subprocess.run(['/usr/bin/python3',str(RELEASE/'restore-converted.py')],check=True)
        raise
