#!/usr/bin/env python3
"""Measure a running debug build's cadence, redraws and execution phases.

Use the same scene/settings for comparisons. Phase samples measure host wall
time (including waits), not the percentage of guest instructions compiled.
This tool neither changes settings nor supplies input to the game.
"""
import argparse
import json
import math
from pathlib import Path
import time

from debug_client import query


def measure(host, port, seconds, counter_prefix):
    def ask(cmd, **values):
        result = query(host, port, dict(cmd=cmd, **values), timeout=30)
        if not result.get('ok'):
            raise RuntimeError(f'{cmd}: {result.get("err", "request failed")}')
        return result

    def sample():
        return {cmd: ask(cmd) for cmd in ('get_registers', 'mod_counters', 'render_pass_stats')}

    def frame_count(sample):
        return next((entry['count'] for entry in sample['mod_counters']['counters']
                     if entry['name'] == counter_prefix + '.frames'), None)

    before = sample()
    start = time.monotonic()
    time.sleep(seconds)
    after = sample()
    elapsed = time.monotonic() - start
    guest_hz = (after['get_registers']['frame'] - before['get_registers']['frame']) / elapsed
    redraw_hz = (after['render_pass_stats']['passes'] - before['render_pass_stats']['passes']) / elapsed
    native_before, native_after = frame_count(before), frame_count(after)
    return dict(
        elapsed_seconds=elapsed, guest_vblanks_per_second=guest_hz,
        extra_geometry_renders_per_second=redraw_hz,
        original_draws_per_second=(native_after-native_before)/elapsed
            if native_after is not None and native_before is not None else None,
        game_phase=ask('turbo_loads'), video=ask('video_info'),
        execution_phase_samples=ask('phase_profile', window=min(60, math.ceil(seconds))),
        timing=ask('frame_perf'), presenter=ask('gl_interp'), before=before, after=after,
        method='Host wall-time phase samples and event-counter deltas; neither instruction coverage nor unique displayed FPS.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=4370)
    parser.add_argument('--seconds', type=float, default=10)
    parser.add_argument('--counter-prefix', default='', help='For example medievil.fr or medievil2.fr')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or not 1 <= args.seconds <= 60:
        parser.error('--seconds must be between 1 and 60')
    result = measure(args.host, args.port, args.seconds, args.counter_prefix)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(f'Guest VBlanks: {result["guest_vblanks_per_second"]:.2f}/s; '
          f'extra geometry renders: {result["extra_geometry_renders_per_second"]:.2f}/s')
    print(f'Evidence: {args.output}')


if __name__ == '__main__':
    main()
