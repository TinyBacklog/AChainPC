#!/usr/bin/env python3
"""Integration checks against a built CLI, a ROM and an isolated profile."""
import argparse
import json
from pathlib import Path
import signal
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--rom', type=Path, required=True)
p.add_argument('--profile', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
base = [str(a.binary.resolve()), '--user-dir', str(a.profile.resolve()), '-g', str(a.rom.resolve())]

def check(name, options, expected, reason=None, send_signal=None):
    report = a.output / f'{name}.json'
    report.unlink(missing_ok=True)
    with (a.output / f'{name}.log').open('w') as log:
        proc = subprocess.Popen(base + options + ['--recomp-stats', str(report)], stdout=log, stderr=log)
        try:
            if send_signal:
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline and proc.poll() is None:
                    if report.exists() and json.loads(report.read_text())['termination'] == 'running':
                        proc.send_signal(send_signal)
                        break
                    time.sleep(.02)
                else:
                    raise AssertionError(f'{name}: never entered run loop')
            code = proc.wait(timeout=30)
            assert code == expected, (name, code, expected)
            if reason:
                result = json.loads(report.read_text())
                assert result['termination'] == reason, (name, result['termination'])
            print(f'{name}: passed', flush=True)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

check('invalid-timeout', ['--headless', '--headless-timeout=abc'], 1)
check('negative-timeout', ['--headless', '--headless-timeout=-1'], 1)
check('strict-without-aot', ['--headless', '--recomp-strict'], 2, 'aot_backend_unavailable')
check('timeout', ['--headless', '--headless-timeout=1'], 0, 'timeout')
check('sigint', ['--headless'], 0, 'signal', signal.SIGINT)
check('sigterm', ['--headless'], 0, 'signal', signal.SIGTERM)
