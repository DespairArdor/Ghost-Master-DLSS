"""Summarize direct GPU/runtime evidence; this does not certify visual quality."""
import argparse
import json
import math
import pathlib
import struct

p = argparse.ArgumentParser()
p.add_argument('capture', type=pathlib.Path)
a = p.parse_args()
events = []
for line in (a.capture / 'capture.jsonl').read_text(encoding='utf-8').splitlines():
    try:
        events.append(json.loads(line))
    except json.JSONDecodeError:
        pass  # The running game may still be writing its last record.
initializations = [e for e in events if e.get('event') == 'dlss_initialized']
evaluations = [e for e in events if e.get('event') == 'dlss_evaluated']
errors = [e for e in events if e.get('event') in ('dlss_error', 'dlss_skipped')]
raw = a.capture / 'dlss-output.rgba16f'
pixels = None
if raw.exists() and initializations:
    data = raw.read_bytes()
    expected = initializations[0]['output_width'] * initializations[0]['output_height'] * 8
    if len(data) == expected:
        count = finite = nonzero = 0
        minimum, maximum = math.inf, -math.inf
        for rgba in struct.iter_unpack('<4e', data):
            for v in rgba[:3]:
                count += 1
                if math.isfinite(v):
                    finite += 1
                    nonzero += abs(v) > 0.00001
                    minimum, maximum = min(minimum, v), max(maximum, v)
        pixels = dict(rgb_channels=count, finite=finite, nonzero=nonzero,
                      minimum=minimum, maximum=maximum)
replacements = sum(e.get('replaced', False) for e in evaluations)
passed = bool(initializations and replacements and not errors and pixels and
              pixels['finite'] == pixels['rgb_channels'] and pixels['nonzero'] > 0)
report = dict(capture=str(a.capture.resolve()), runtime_evidence_passed=passed,
              initializations=initializations,
              last_evaluation_count=max((e['count'] for e in evaluations), default=0),
              logged_replacement_checkpoints=replacements, errors=errors,
              input_skips=[e for e in events if e.get('event') == 'dlss_input_skip'],
              output_pixels=pixels,
              screenshots=[e for e in events if e.get('event') == 'screen_snapshot'],
              visual_quality_certified=False, performance_gain_measured=False)
(a.capture / 'validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
raise SystemExit(0 if passed else 1)
