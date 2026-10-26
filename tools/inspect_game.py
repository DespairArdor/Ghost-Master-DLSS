"""Read-only fingerprint and narrow ASCII/UTF-16 engine feature scan."""
import argparse
import hashlib
import json
import re
from pathlib import Path

def inspect(root):
    exe = root / 'GhostMasterRemake/Binaries/Win64/GhostMasterRemake-Win64-Shipping.exe'
    data = exe.read_bytes()
    patterns = [r'\+\+UE5\+Release-[0-9.]+', r'5\.[0-9]+\.[0-9]+-[0-9]+[^\x00\r\n]{0,80}',
                r'r\.(?:NGX|FidelityFX|TSR|TemporalAA|AntiAliasingMethod|ScreenPercentage|XeSS)[A-Za-z0-9_.]*',
                r'[^\x00\r\n]{0,50}TemporalSuperResolution[^\x00\r\n]{0,100}',
                r'[^\x00\r\n]{0,30}D3D(?:11|12)RHI[^\x00\r\n]{0,60}']
    matches = {}
    for encoding in ('ascii', 'utf-16-le'):
        text = data.decode(encoding, errors='replace')
        matches[encoding] = sorted(set(m.group() for p in patterns for m in re.finditer(p, text)))
    binaries = []
    for path in sorted(root.rglob('*')):
        if path.is_file() and path.suffix.lower() in ('.dll', '.addon64', '.exe', '.original'):
            binaries.append({'path': str(path.relative_to(root)), 'size': path.stat().st_size,
                             'sha256': hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
                             if hasattr(hashlib, 'file_digest') else hashlib.sha256(path.read_bytes()).hexdigest()})
    return {'root': str(root), 'exe_sha256': hashlib.sha256(data).hexdigest(),
            'exe_bytes': len(data), 'strings': matches, 'binaries': binaries}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.game)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k != 'binaries'}, indent=2))
