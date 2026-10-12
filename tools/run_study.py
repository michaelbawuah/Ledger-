#!/usr/bin/env python3
"""Run Ledger's deterministic experiment matrix sequentially on the local host."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

def matrix(smoke=False):
    cases = []
    seeds = [7] if smoke else [20261011, 20261012, 20261013]
    profiles = ['balanced', 'cancel_heavy', 'trade_storm', 'replace_heavy', 'sparse_fok', 'rejection_stress']
    for profile in profiles:
        for seed in seeds:
            cases.append((profile, 16 if smoke else 256, 2048 if smoke else 20000,
                          seed, 500 if smoke else 100000, 'overview'))
    if not smoke:
        for depth in [64, 256, 1024, 4096, 16384]:
            for seed in seeds:
                cases.append(('balanced', depth, 65536, seed, 20000, 'depth'))
        for domain in [1024, 4096, 16384, 65536, 262144, 1000000]:
            for seed in seeds:
                cases.append(('sparse_fok', 64, domain, seed,
                              5000 if domain > 65536 else 20000, 'domain'))
    return cases

def run_checked(arguments, **kwargs):
    print(' '.join(map(str, arguments)), flush=True)
    return subprocess.run(list(map(str, arguments)), check=True, cwd=ROOT, **kwargs)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary-dir', type=pathlib.Path, default=ROOT / 'build/native')
    parser.add_argument('--outdir', type=pathlib.Path, default=ROOT / 'bench/results/study')
    parser.add_argument('--cpu', type=int, default=None, help='Linux process affinity, optional')
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--resume', action='store_true', help='Skip completed cells with summary.json')
    parser.add_argument('--family', choices=['all', 'overview', 'depth', 'domain'], default='all')
    args = parser.parse_args()
    if args.cpu is not None:
        if not hasattr(os, 'sched_setaffinity'):
            parser.error('--cpu requires Linux affinity support')
        if args.cpu not in os.sched_getaffinity(0):
            parser.error('--cpu is not in the process affinity mask')
        os.sched_setaffinity(0, {args.cpu})
    binary = args.binary_dir.resolve() / ('ledger_study.exe' if sys.platform == 'win32' else 'ledger_study')
    if not binary.is_file():
        parser.error(f'build the ledger_study target first: {binary}')
    output = args.outdir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    records = []
    for profile, depth, domain, seed, commands, family in matrix(args.smoke):
        if args.family != 'all' and family != args.family:
            continue
        path = output / family / f'{profile}-d{depth}-p{domain}-s{seed}'
        required = ('summary.json', 'runs.csv', 'histogram.csv', 'diagnostics.csv', 'timeline.csv', 'depth.csv')
        if args.resume and all((path / name).is_file() for name in required):
            print(f'Reusing {path}', flush=True)
        else:
            run_checked([binary, '--profile', profile, '--depth', depth, '--domain', domain,
                         '--seed', seed, '--commands', commands, '--runs', 1 if args.smoke else 3,
                         '--outdir', path])
        records.append({'family': family, 'profile': profile, 'depth': depth, 'domain': domain,
                        'seed': seed, 'commands': commands, 'runs': 1 if args.smoke else 3,
                        'directory': path.relative_to(output).as_posix()})
    (output / f'experiment-matrix-{args.family}.json').write_text(json.dumps({'schema_version': 1,
        'smoke': args.smoke, 'affinity_cpu': args.cpu,
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'cells': records}, indent=2) + '\n')
    print(f'Completed {len(records)} experiment cells in {output}', flush=True)

if __name__ == '__main__':
    main()
