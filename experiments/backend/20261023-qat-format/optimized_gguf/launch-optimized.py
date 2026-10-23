#!/home/isnotreal/vllm-env/bin/python
import json, os
from pathlib import Path
root=Path(__file__).resolve().parent
backup=Path('/home/isnotreal/model-tools/backend-qat-backup-20261023')
env=json.loads((backup/'process-environment.json').read_text())
env['PYTHONPATH']=str(root/'plugin')
env['VLLM_PLUGINS']='gguf'
env['VLLM_GGUF_KERNEL_POLICY']=str(root/'policy-active.json')
env['VLLM_GGUF_TRACE_SHAPES']=str(root/'shape-traces')
env['TRITON_CACHE_DIR']=str(root/'.triton-cache')
args=json.loads((root.parent/'original-gguf-args.json').read_text())
os.execve(args[0],args,env)
