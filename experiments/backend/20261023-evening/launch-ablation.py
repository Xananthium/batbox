#!/home/isnotreal/vllm-env/bin/python
"""Fresh isolated serving for MTP ablation; production restoration is separate."""
import hashlib
import json
import os
from pathlib import Path
import sys


def configuration(mode):
    if mode not in ('mtp2', 'off'):
        raise ValueError('Expected mtp2 or off')
    root = Path('/home/isnotreal/model-tools/gguf-pr141-releases/20261023-e62877c-e3cefbc')
    manifest = json.loads((root / 'release.json').read_text())
    for name, expected in manifest['sha256'].items():
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError('Frozen GGUF release integrity mismatch: ' + name)
    env = json.loads((root / 'environment.json').read_text())
    for key in ['INVOCATION_ID', 'JOURNAL_STREAM', 'SYSTEMD_EXEC_PID',
                'MEMORY_PRESSURE_WATCH', 'MEMORY_PRESSURE_WRITE']:
        if key in os.environ:
            env[key] = os.environ[key]
    env['PYTHONPATH'] = str(root / 'python')
    env['VLLM_PLUGINS'] = 'gguf'
    env['VLLM_GGUF_CUDA_KERNEL'] = 'upstream'
    args = json.loads((root / 'args.json').read_text())
    if mode == 'off':
        i = args.index('--speculative-config')
        del args[i:i + 2]
    args[args.index('--port') + 1] = '8002'
    assert args[args.index('--max-model-len') + 1] == '200000'
    assert args[args.index('--kv-cache-memory-bytes') + 1] == '6442450944'
    return args, env


if __name__ == '__main__':
    args, env = configuration(sys.argv[1])
    os.execve(args[0], args, env)
