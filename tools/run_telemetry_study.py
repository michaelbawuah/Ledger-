#!/usr/bin/env python3
"""Capture a serial, reproducible SPSC matrix and an execution-accounted strategy trace."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary-dir', type=pathlib.Path, default=ROOT / 'build/native')
    parser.add_argument('--queue-outdir', type=pathlib.Path, default=ROOT / 'bench/results/spsc-study')
    parser.add_argument('--strategy-outdir', type=pathlib.Path, default=ROOT / 'bench/results/strategy-study')
    parser.add_argument('--producer-cpu', type=int)
    parser.add_argument('--consumer-cpu', type=int)
    parser.add_argument('--smoke', action='store_true')
    args = parser.parse_args()
    if (args.producer_cpu is None) != (args.consumer_cpu is None):
        parser.error('specify both CPU indices together')
    if args.producer_cpu is not None:
        if not hasattr(os, 'sched_getaffinity'):
            parser.error('affinity requests require Linux')
        allowed = os.sched_getaffinity(0)
        if args.producer_cpu == args.consumer_cpu or not {args.producer_cpu, args.consumer_cpu} <= allowed:
            parser.error('choose two distinct allowed CPUs')
    suffix = '.exe' if sys.platform == 'win32' else ''
    binaries = {name: args.binary_dir.resolve() / (name + suffix)
                for name in ['ledger_spsc_bench', 'ledger_market_maker']}
    for binary in binaries.values():
        if not binary.is_file():
            parser.error(f'build this target first: {binary}')
    queue_dir, strategy_dir = args.queue_outdir.resolve(), args.strategy_outdir.resolve()
    queue_dir.mkdir(parents=True, exist_ok=True)
    strategy_dir.mkdir(parents=True, exist_ok=True)
    count = 1000 if args.smoke else 1000000
    captures = []
    # Rotate capacity order by repetition, while preserving separate zero-clock runs.
    capacities = [8, 64, 512, 4096]
    for run in range(1, 2 if args.smoke else 4):
        order = capacities[run-1:] + capacities[:run-1]
        for work in [0, 64]:
            for samples in [0, min(10000, count)]:
                for capacity in order:
                    cmd = [str(binaries['ledger_spsc_bench']), '--commands', str(count),
                           '--queue-capacity', str(capacity), '--consumer-work', str(work),
                           '--latency-samples', str(samples)]
                    if args.producer_cpu is not None:
                        cmd += ['--producer-cpu', str(args.producer_cpu), '--consumer-cpu', str(args.consumer_cpu)]
                    print(' '.join(cmd), flush=True)
                    data = json.loads(subprocess.check_output(cmd, text=True, cwd=ROOT))
                    if args.producer_cpu is not None and not data['cpu_pinned']:
                        raise RuntimeError('requested queue affinity was not verified')
                    data['study_run'] = run
                    filename = f'queue-c{capacity}-w{work}-s{samples}-r{run}.json'
                    (queue_dir / filename).write_text(json.dumps(data, indent=2) + '\n')
                    captures.append(filename)
    cmd = [str(binaries['ledger_market_maker']), '--steps', '10000', '--seed', '42',
           '--quote-quantity', '5', '--inventory-limit', '20', '--trace', str(strategy_dir / 'trace.csv')]
    print(' '.join(cmd), flush=True)
    summary = json.loads(subprocess.check_output(cmd, text=True, cwd=ROOT))
    (strategy_dir / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    metadata = {'schema_version': 1, 'machine': platform.platform(),
                'commands_per_queue_run': count, 'queue_captures': captures,
                'binary_sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in binaries.items()},
                'method': 'Serial captures; throughput uses zero samples; latency uses separate timestamped runs; min/max run ranges are not confidence intervals.'}
    (queue_dir / 'capture-metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'Completed {len(captures)} queue captures and the 10000-step strategy trace', flush=True)

if __name__ == '__main__':
    main()
