"""Stage the measured PR141 artifact without changing the active service."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess

ROOT=Path(__file__).resolve().parent
RELEASE=Path('/home/isnotreal/model-tools/gguf-pr141-releases/20261023-e62877c-e3cefbc')
BACKUP=Path('/home/isnotreal/model-tools/backend-qat-backup-20261023')
PERSISTENT='/etc/systemd/system/qwen-vllm.service.d/zzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-gguf-pr141-release.conf'
TRANSIENT='/run/systemd/system/qwen-vllm.service.d/zzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-qat-format-trial.conf'


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


assert not RELEASE.exists(), 'Never overwrite a measured frozen release'
RELEASE.mkdir(parents=True)
shutil.copytree(ROOT/'upstream-pr141/vllm_gguf_plugin',RELEASE/'python/vllm_gguf_plugin',
                ignore=shutil.ignore_patterns('csrc','__pycache__','*.pyc'))
shutil.copy2(ROOT/'upstream-pr141/LICENSE',RELEASE/'LICENSE')
shutil.copy2(ROOT/'upstream-pr141/third_party/llama.cpp/LICENSE',RELEASE/'LLAMA_CPP_LICENSE')
for source,target in [('upstream-pr141-compatibility.patch','compatibility.patch'),
                      ('pr141-build-manifest.json','build-manifest.json')]:
    shutil.copy2(ROOT/source,RELEASE/target)
shutil.copy2(ROOT.parent/'original-gguf-args.json',RELEASE/'args.json')
shutil.copy2(BACKUP/'process-environment.json',RELEASE/'environment.json')
os.chmod(RELEASE/'environment.json',0o600)
rollback=RELEASE/'rollback';rollback.mkdir()
unit=Path('/etc/systemd/system/qwen-vllm.service')
files=[unit,*sorted(unit.with_name('qwen-vllm.service.d').glob('*.conf'))]
assert not Path(PERSISTENT).exists()
baseline=[]
for i,p in enumerate(files):
    dest=rollback/f'{i:02d}-{p.name}';shutil.copy2(p,dest)
    baseline.append({'path':str(p),'snapshot':dest.name,'sha256':sha(p),
                     'mode':stat.S_IMODE(p.stat().st_mode),'uid':p.stat().st_uid,'gid':p.stat().st_gid})
(rollback/'baseline.json').write_text(json.dumps(baseline,indent=2)+'\n')

launcher='''#!/home/isnotreal/vllm-env/bin/python
import hashlib, json, os
from pathlib import Path
root=Path(__file__).resolve().parent
manifest=json.loads((root/'release.json').read_text())
for name,expected in manifest['sha256'].items():
    actual=hashlib.sha256((root/name).read_bytes()).hexdigest()
    if actual!=expected:
        raise RuntimeError('Frozen GGUF release integrity mismatch: '+name)
env=json.loads((root/'environment.json').read_text())
# Retain systemd process identity from this invocation; model/runtime settings
# remain the measured baseline with only plugin location/backend selected.
for key in ['INVOCATION_ID','JOURNAL_STREAM','SYSTEMD_EXEC_PID',
            'MEMORY_PRESSURE_WATCH','MEMORY_PRESSURE_WRITE']:
    if key in os.environ:env[key]=os.environ[key]
env['PYTHONPATH']=str(root/'python')
env['VLLM_PLUGINS']='gguf'
env['VLLM_GGUF_CUDA_KERNEL']='upstream'
args=json.loads((root/'args.json').read_text())
os.execve(args[0],args,env)
'''
(RELEASE/'launch.py').write_text(launcher);os.chmod(RELEASE/'launch.py',0o755)
rollback_code='''#!/usr/bin/python3
"""Restore the complete saved converted service; leave its original helper intact."""
import hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parent
rows=json.loads((root/'rollback/baseline.json').read_text())
for row in rows:
    source=root/'rollback'/row['snapshot']
    assert hashlib.sha256(source.read_bytes()).hexdigest()==row['sha256'], source
if '--check' in sys.argv:
    print('Rollback snapshot verified: '+str(len(rows))+' persistent files. No mutation.')
    raise SystemExit(0)
if os.geteuid()!=0:
    os.execv('/usr/bin/sudo',['sudo','-n','/usr/bin/python3',str(Path(__file__).resolve())])
subprocess.run(['systemctl','stop','qwen-vllm.service'],check=False)
for filename in [PERSISTENT,TRANSIENT]:
    Path(filename).unlink(missing_ok=True)
for row in rows:
    dest=Path(row['path']);dest.parent.mkdir(parents=True,exist_ok=True)
    fd,tmp=tempfile.mkstemp(prefix='.'+dest.name+'.rollback-',dir=dest.parent)
    try:
        with os.fdopen(fd,'wb') as stream:
            stream.write((root/'rollback'/row['snapshot']).read_bytes());stream.flush();os.fsync(stream.fileno())
        os.chmod(tmp,row['mode']);os.chown(tmp,row['uid'],row['gid'])
        os.replace(tmp,dest)
        directory=os.open(dest.parent,os.O_DIRECTORY);os.fsync(directory);os.close(directory)
    finally:
        if Path(tmp).exists():Path(tmp).unlink()
subprocess.run(['systemctl','daemon-reload'],check=True)
subprocess.run(['systemctl','reset-failed','qwen-vllm.service'],check=False)
subprocess.run(['systemctl','start','qwen-vllm.service'],check=True)
print('Converted MTP2 service restored and starting. Check /health for readiness.')
'''
rollback_code=rollback_code.replace('for filename in [PERSISTENT,TRANSIENT]:',f'for filename in [{PERSISTENT!r},{TRANSIENT!r}]:')
(RELEASE/'restore-converted.py').write_text(rollback_code);os.chmod(RELEASE/'restore-converted.py',0o755)
args=json.loads((RELEASE/'args.json').read_text())
model=args[2]
unit_body=f'''[Unit]
Description=Hauhau Gemma4 original QAT GGUF PR141 Vision MTP Q4 TurboQuant
ConditionPathExists=
ConditionPathExists={model}
ConditionPathExists={RELEASE}/release.json
ConditionPathExists={RELEASE}/python/vllm_gguf_plugin/_C_gguf.abi3.so

[Service]
ExecStart=
ExecStart={RELEASE}/launch.py
Restart=always
RestartSec=10
TimeoutStopSec=30
'''
(RELEASE/'qwen-vllm.conf').write_text(unit_body)
manifest={'plugin_revision':'e62877c30519305f3c82dc0d616cf62b5ed67133',
          'llama_revision':'002a12ad25503a93501b2e188c360029830a241a',
          'model_revision':'9654466e82d83f5ebfe1518a369bc5900873abb1',
          'extension_sha256':sha(RELEASE/'python/vllm_gguf_plugin/_C_gguf.abi3.so'),
          'persistent_override':PERSISTENT,'transient_override':TRANSIENT,
          'rollback':str(RELEASE/'restore-converted.py'),
          'compiler_note':'Measured incremental SM86 build combines nvcc13.3 upstream TUs and final6 objects from nvcc13.0.88; runtime13.0. See build-manifest.json.',
          'sha256':{str(p.relative_to(RELEASE)):sha(p) for p in sorted(RELEASE.rglob('*')) if p.is_file()}}
(RELEASE/'release.json').write_text(json.dumps(manifest,indent=2)+'\n')
# Remove write permission from the staged artifact. The launcher rechecks every
# recorded file before execution, including code, extension, arguments and env.
for p in RELEASE.rglob('*'):
    if p.is_file():os.chmod(p,stat.S_IMODE(p.stat().st_mode)&~0o222)
for p in sorted(RELEASE.rglob('*'),reverse=True):
    if p.is_dir():os.chmod(p,0o555)
os.chmod(RELEASE,0o555)
subprocess.run(['/usr/bin/python3',str(RELEASE/'restore-converted.py'),'--check'],check=True)
(ROOT/'durable-release.json').write_text(json.dumps({k:v for k,v in manifest.items() if k!='sha256'}|{
    'path':str(RELEASE),'release_manifest_sha256':sha(RELEASE/'release.json'),
    'status':'staged; not yet activated'},indent=2)+'\n')
print(json.dumps({'release':str(RELEASE),'extension_sha256':manifest['extension_sha256'],'status':'staged only'}))
