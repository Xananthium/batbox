#!/home/isnotreal/vllm-env/bin/python
"""Matched ablation: remove only the saved speculative-config argument pair."""
import json
import os
from pathlib import Path

backup = Path('/home/isnotreal/model-tools/backend-qat-backup-20261023')
env = json.loads((backup / 'process-environment.json').read_text())
args = json.loads((backup / 'process-args.json').read_text())
assert args.count('--speculative-config') == 1
index = args.index('--speculative-config')
spec = json.loads(args[index + 1])
assert spec['method'] == 'mtp' and spec['num_speculative_tokens'] == 2
del args[index:index + 2]
os.execve(args[0], args, env)
