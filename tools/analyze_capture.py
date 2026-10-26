"""Summarize observed compute work without claiming that a size match identifies TSR."""
import argparse
import collections
import json
from pathlib import Path
import subprocess
import re

def active_registers(disassembly):
    # DXC's reflected table survives stripped variable names. Only the HLSL bind column is used.
    registers=set()
    for line in disassembly.splitlines():
        if not line.startswith(';'): continue
        match=re.search(r'\b(cb|t|u)(\d+)(?:,space(\d+))?\s+(\d+)\s*$',line)
        if match:
            kind,reg,space,count=match.groups()
            for i in range(min(int(count),128)): registers.add((kind,int(reg)+i,int(space or 0)))
    return registers

def register_kind(kind):
    return {2:'t',4:'t',3:'u',5:'u',6:'cb'}.get(kind,'unknown')

def analyze(directory, dxc=None):
    events=[]
    with (directory/'capture.jsonl').open(encoding='utf-8') as f:
        for line in f:
            try: events.append(json.loads(line))
            except json.JSONDecodeError: pass  # The last line can be incomplete in a live capture.
    shaders=collections.defaultdict(lambda: {'dispatches':0,'groups':set(),'textures':set(),'names':set()})
    for e in events:
        if e.get('event')!='dispatch': continue
        s=shaders[e['shader']]; s['dispatches']+=1; s['groups'].add(tuple(e['groups']))
        for b in e['bindings']:
            if 'width' in b: s['textures'].add((b['type'],b['register'],b['width'],b['height'],b['format']))
            if b.get('name'): s['names'].add(b['name'])
    report={'captured_dispatches':sum(s['dispatches'] for s in shaders.values()),
            'capture_windows':sum(e.get('event')=='capture_window' for e in events),
            'shaders':{h:{k:sorted(v) if isinstance(v,set) else v for k,v in s.items()} for h,s in shaders.items()},
            'limitation':'Observed bindings are not validated TSR inputs; capture is CPU recording order, not GPU execution order. Indirect dispatch and root constant data are not captured.'}
    (directory/'summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    if dxc:
        out=directory/'disassembly'; out.mkdir(exist_ok=True)
        for h in shaders:
            shader=directory/'shaders'/f'{h}.cso'
            if not shader.exists(): continue
            result=subprocess.run([str(dxc),'-dumpbin',str(shader)],capture_output=True)
            (out/f'{h}.txt').write_bytes(result.stdout+result.stderr)
    filtered=[]
    reflection={}
    for h in shaders:
        path=directory/'disassembly'/f'{h}.txt'
        if path.exists(): reflection[h]=active_registers(path.read_text(errors='replace'))
    # Last observed recording for each shader; easier to inspect than stale unused slots.
    last={e['shader']:e for e in events if e.get('event')=='dispatch'}
    for h,e in last.items():
        filtered.append({**e,'bindings':[b for b in e['bindings'] if
            (register_kind(b['type']),b['register'],b['space']) in reflection.get(h,set())]})
    (directory/'active-bindings.json').write_text(json.dumps(filtered,indent=2),encoding='utf-8')
    print(json.dumps({'dispatches':report['captured_dispatches'],'windows':report['capture_windows'],
                      'shaders':len(shaders),'summary':str(directory/'summary.json')},indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('directory',type=Path); p.add_argument('--dxc',type=Path)
    a=p.parse_args(); analyze(a.directory,a.dxc)
