"""Sample NVML through nvidia-smi; sampled usage is not an allocator peak."""
import argparse
import datetime
import json
import os
from pathlib import Path
import signal
import subprocess
import time

p=argparse.ArgumentParser()
p.add_argument('output', type=Path)
args=p.parse_args()
stop=False

def finish(signum, frame):
    global stop
    stop=True

signal.signal(signal.SIGTERM, finish)
signal.signal(signal.SIGINT, finish)
args.output.with_suffix('.pid').write_text(str(os.getpid())+'\n')
with args.output.open('x') as f:
    while not stop:
        now=datetime.datetime.now(datetime.timezone.utc).isoformat()
        value=subprocess.check_output(['nvidia-smi','--query-gpu=index,memory.used,utilization.gpu,temperature.gpu,power.draw,clocks.sm,clocks.mem','--format=csv,noheader,nounits'],text=True)
        f.write(json.dumps({'utc':now,'rows':value.strip().splitlines()})+'\n')
        f.flush()
        time.sleep(1)
