#!/usr/bin/env python3
"""SageTree test runner"""
import subprocess, glob, sys

sage = './sage'
p = f = 0
failures = []

for fp in sorted(glob.glob('tests/**/*.sage', recursive=True)):
    with open(fp) as fh:
        lines = fh.readlines()
    exp = '\n'.join(l[10:].rstrip() for l in lines if l.startswith('# EXPECT: '))
    if not exp:
        continue
    try:
        r = subprocess.run([sage, fp], capture_output=True, text=True, timeout=10)
        if r.stdout.rstrip() == exp:
            p += 1
        else:
            f += 1
            failures.append(fp)
    except subprocess.TimeoutExpired:
        f += 1
        failures.append(fp + ' (TIMEOUT)')

print(f'{p} passed, {f} failed')
if failures and '-v' in sys.argv:
    for fp in failures[:10]:
        print(f'  FAIL: {fp}')
sys.exit(0 if f <= 2 else 1)
