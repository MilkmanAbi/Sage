#!/usr/bin/env python3
"""SageTree Benchmark Suite — Comparative analysis against C++, Rust, Python"""

import subprocess, time, os, sys, re, json

SAGE = './core/sage'
BENCHMARKS = ['fib', 'alloc', 'string', 'struct']
LANGS = ['cpp', 'rust', 'python', 'sage']
RESULTS = {}

def run_cmd(cmd, timeout=120):
    """Run command, return (stdout, wall_time_sec, peak_rss_kb)"""
    # Use /usr/bin/time for memory measurement
    start = time.monotonic()
    try:
        r = subprocess.run(
            ['/usr/bin/time', '-v'] + cmd,
            capture_output=True, text=True, timeout=timeout
        )
        wall = time.monotonic() - start
        # Parse peak RSS from /usr/bin/time output
        rss = 0
        for line in r.stderr.split('\n'):
            if 'Maximum resident' in line:
                m = re.search(r'(\d+)', line)
                if m: rss = int(m.group(1))
        return r.stdout.strip(), wall, rss, r.returncode
    except subprocess.TimeoutExpired:
        return "TIMEOUT", timeout, 0, -1
    except Exception as e:
        return f"ERROR: {e}", 0, 0, -1

def compile_cpp(bench):
    src = f'benchmarks/bench_{bench}.cpp'
    out = f'/tmp/bench_{bench}_cpp'
    r = subprocess.run(['g++', '-O2', '-o', out, src], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  C++ compile error: {r.stderr[:200]}")
        return None
    return out

def compile_rust(bench):
    src = f'benchmarks/bench_{bench}.rs'
    out = f'/tmp/bench_{bench}_rust'
    r = subprocess.run([os.path.expanduser('~/.cargo/bin/rustc'), '-O', '-o', out, src],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  Rust compile error: {r.stderr[:200]}")
        return None
    return out

def extract_self_time(output):
    """Extract the self-reported time from benchmark output"""
    for line in output.split('\n'):
        if line.startswith('time:'):
            m = re.search(r'([\d.]+)', line)
            if m: return float(m.group(1))
    return None

def extract_result(output):
    """Extract the computation result (first line)"""
    lines = output.strip().split('\n')
    return lines[0] if lines else "?"

def run_bench(bench):
    print(f"\n{'='*60}")
    print(f"  BENCHMARK: {bench}")
    print(f"{'='*60}")
    
    results = {}
    
    # C++ (compiled, -O2)
    print(f"\n  [C++ -O2]")
    binary = compile_cpp(bench)
    if binary:
        out, wall, rss, rc = run_cmd([binary])
        self_time = extract_self_time(out)
        result = extract_result(out)
        results['cpp'] = {'wall': wall, 'self_time': self_time, 'rss_kb': rss, 'result': result, 'rc': rc}
        print(f"    result: {result}")
        print(f"    self_time: {self_time:.6f}s" if self_time else "    self_time: N/A")
        print(f"    wall_time: {wall:.3f}s")
        print(f"    peak_rss: {rss}KB ({rss/1024:.1f}MB)")
    
    # Rust (compiled, -O)
    print(f"\n  [Rust -O]")
    binary = compile_rust(bench)
    if binary:
        out, wall, rss, rc = run_cmd([binary])
        self_time = extract_self_time(out)
        result = extract_result(out)
        results['rust'] = {'wall': wall, 'self_time': self_time, 'rss_kb': rss, 'result': result, 'rc': rc}
        print(f"    result: {result}")
        print(f"    self_time: {self_time:.6f}s" if self_time else "    self_time: N/A")
        print(f"    wall_time: {wall:.3f}s")
        print(f"    peak_rss: {rss}KB ({rss/1024:.1f}MB)")
    
    # Python (interpreted)
    print(f"\n  [Python 3.12]")
    out, wall, rss, rc = run_cmd(['python3', f'benchmarks/bench_{bench}.py'])
    self_time = extract_self_time(out)
    result = extract_result(out)
    results['python'] = {'wall': wall, 'self_time': self_time, 'rss_kb': rss, 'result': result, 'rc': rc}
    print(f"    result: {result}")
    print(f"    self_time: {self_time:.6f}s" if self_time else "    self_time: N/A")
    print(f"    wall_time: {wall:.3f}s")
    print(f"    peak_rss: {rss}KB ({rss/1024:.1f}MB)")
    
    # Sage (interpreted, default MossVM)
    print(f"\n  [SageTree MossVM]")
    out, wall, rss, rc = run_cmd([SAGE, f'benchmarks/bench_{bench}.sage'])
    self_time = extract_self_time(out)
    result = extract_result(out)
    results['sage_vm'] = {'wall': wall, 'self_time': self_time, 'rss_kb': rss, 'result': result, 'rc': rc}
    print(f"    result: {result}")
    print(f"    self_time: {self_time:.6f}s" if self_time else "    self_time: N/A")
    print(f"    wall_time: {wall:.3f}s")
    print(f"    peak_rss: {rss}KB ({rss/1024:.1f}MB)")
    
    # Sage (AST interpreter)
    print(f"\n  [SageTree AST]")
    out, wall, rss, rc = run_cmd([SAGE, '--runtime', 'ast', f'benchmarks/bench_{bench}.sage'])
    self_time = extract_self_time(out)
    result = extract_result(out)
    results['sage_ast'] = {'wall': wall, 'self_time': self_time, 'rss_kb': rss, 'result': result, 'rc': rc}
    print(f"    result: {result}")
    print(f"    self_time: {self_time:.6f}s" if self_time else "    self_time: N/A")
    print(f"    wall_time: {wall:.3f}s")
    print(f"    peak_rss: {rss}KB ({rss/1024:.1f}MB)")
    
    return results

# Run all benchmarks
print("SageTree Benchmark Suite")
print(f"System: {subprocess.getoutput('uname -m')} / {subprocess.getoutput('lscpu | grep Model\\ name | sed s/.*://')}")
print(f"GCC: {subprocess.getoutput('gcc --version | head -1')}")
print(f"Rust: {subprocess.getoutput(os.path.expanduser('~/.cargo/bin/rustc') + ' --version')}")
print(f"Python: {subprocess.getoutput('python3 --version')}")
print(f"Sage: {subprocess.getoutput(SAGE + ' --version')}")

all_results = {}
for bench in BENCHMARKS:
    all_results[bench] = run_bench(bench)

# Summary table
print(f"\n{'='*80}")
print(f"  SUMMARY — Self-reported time (seconds)")
print(f"{'='*80}")
print(f"{'Benchmark':<12} {'C++ -O2':>10} {'Rust -O':>10} {'Python':>10} {'Sage VM':>10} {'Sage AST':>10}")
print(f"{'-'*12} {'-'*10} {'-'*10} {'-'*10} {'-'*10} {'-'*10}")
for bench in BENCHMARKS:
    r = all_results[bench]
    def fmt(k):
        if k not in r or r[k]['self_time'] is None: return "N/A"
        return f"{r[k]['self_time']:.4f}"
    print(f"{bench:<12} {fmt('cpp'):>10} {fmt('rust'):>10} {fmt('python'):>10} {fmt('sage_vm'):>10} {fmt('sage_ast'):>10}")

print(f"\n{'='*80}")
print(f"  SUMMARY — Peak RSS (KB)")
print(f"{'='*80}")
print(f"{'Benchmark':<12} {'C++ -O2':>10} {'Rust -O':>10} {'Python':>10} {'Sage VM':>10} {'Sage AST':>10}")
print(f"{'-'*12} {'-'*10} {'-'*10} {'-'*10} {'-'*10} {'-'*10}")
for bench in BENCHMARKS:
    r = all_results[bench]
    def fmt(k):
        if k not in r: return "N/A"
        return f"{r[k]['rss_kb']}"
    print(f"{bench:<12} {fmt('cpp'):>10} {fmt('rust'):>10} {fmt('python'):>10} {fmt('sage_vm'):>10} {fmt('sage_ast'):>10}")

# Sage vs Python ratio
print(f"\n{'='*80}")
print(f"  Sage VM vs Python (ratio, lower is better for Sage)")
print(f"{'='*80}")
for bench in BENCHMARKS:
    r = all_results[bench]
    if 'sage_vm' in r and 'python' in r:
        st_s = r['sage_vm'].get('self_time')
        st_p = r['python'].get('self_time')
        if st_s and st_p and st_p > 0:
            ratio = st_s / st_p
            print(f"  {bench:<12} Sage is {ratio:.1f}x {'slower' if ratio > 1 else 'faster'} than Python")
