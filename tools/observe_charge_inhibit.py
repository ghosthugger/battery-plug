#!/usr/bin/env python3
"""Observe BMS diagnostics; optionally run a bounded normal Always charging test.

No BMS writes or protective overrides. Run on a computer with LAN access and
keep it awake. If that computer loses power/network, automatic mode restoration
cannot be guaranteed; the plug's existing protections remain in force.
"""
import argparse
import json
import signal
import time
from pathlib import Path
from urllib.parse import urlencode
from urllib.request import Request, urlopen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://192.168.1.254')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--minutes', type=float, default=120)
    parser.add_argument('--always-charging', action='store_true', help='Explicitly enable mains for this test')
    args = parser.parse_args()
    if not 0 < args.minutes <= 120:
        parser.error('--minutes must be greater than zero and at most 120')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    base = args.url.rstrip('/') + '/charging/api/'

    def api(endpoint, data=None):
        request = Request(base + endpoint, data=urlencode(data).encode() if data is not None else None)
        with urlopen(request, timeout=8) as response:
            return json.load(response)

    def save(name, value):
        path = args.output_dir / name
        temporary = path.with_suffix(path.suffix + '.tmp')
        temporary.write_text(json.dumps(value, indent=2) + '\n')
        temporary.replace(path)

    stop = [False]
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, lambda *_: stop.__setitem__(0, True))
    status = api('status')
    diagnostics = api('bms-diagnostics')
    save('before.json', {'status': status, 'diagnostics': diagnostics})
    if args.always_charging and not (status['bms_fresh'] and status['meter_fresh'] and
                                     status['storage_ok'] and status['charge_allowed']):
        raise RuntimeError('Test not started: fresh, charge-permitted telemetry and healthy storage required')
    baseline = max((s['sequence'] for s in diagnostics['snapshots']), default=0)
    changed_mode = False
    outcome = {'reason': 'timeout', 'started_at': time.time(), 'test_minutes': args.minutes}
    started = time.monotonic()
    last_log = -60
    failures = 0
    try:
        if args.always_charging:
            # Set before POST: a response failure may occur after the command applied.
            changed_mode = True
            api('mode', {'mode': 'always'})
            print('Always charging requested; existing safety stops remain active.', flush=True)
        while not stop[0] and time.monotonic() - started < args.minutes * 60:
            try:
                status = api('status')
                diagnostics = api('bms-diagnostics')
                save('latest.json', {'status': status, 'diagnostics': diagnostics})
                failures = 0
                inhibited = [s for s in diagnostics['snapshots'] if s['sequence'] > baseline and not s['charge_allowed']]
                if inhibited or not status['charge_allowed']:
                    outcome.update(reason='charge_inhibit_observed', status=status, snapshots=inhibited)
                    print('Captured BMS charging inhibit.', flush=True)
                    break
                if not status['bms_fresh'] or not status['meter_fresh'] or not status['storage_ok']:
                    outcome.update(reason='telemetry_or_storage_unhealthy', status=status)
                    break
                if args.always_charging and time.monotonic() - started > 20 and status['mode'] != 'always':
                    outcome.update(reason='mode_changed_externally', status=status)
                    break
                elapsed = time.monotonic() - started
                if elapsed - last_log >= 60:
                    print(f"{elapsed/60:.0f} min: SOC {status['soc']}%, plug {status['power']} W, {status['reason']}", flush=True)
                    last_log = elapsed
                    with (args.output_dir / 'samples.jsonl').open('a') as samples:
                        samples.write(json.dumps({'observed_at': time.time(), 'status': status}) + '\n')
            except Exception as exc:
                failures += 1
                print(f'Observation failed ({failures}/3): {exc}', flush=True)
                if failures >= 3:
                    outcome.update(reason='connection_lost', error=str(exc))
                    break
            time.sleep(5)
        if stop[0]:
            outcome['reason'] = 'interrupted'
    except Exception as exc:
        outcome.update(reason='error', error=str(exc))
        raise
    finally:
        if changed_mode:
            restored = False
            for attempt in range(8):
                try:
                    api('mode', {'mode': 'auto'})
                    time.sleep(2)
                    current = api('status')
                    if current['mode'] == 'auto':
                        restored = True
                        outcome['restored_status'] = current
                        break
                except Exception as exc:
                    outcome['restore_error'] = str(exc)
                    time.sleep(2)
            outcome['automatic_restored'] = restored
            print('Automatic mode restored.' if restored else
                  'RESTORATION FAILED: select Automatic on the plug dashboard.', flush=True)
        outcome['finished_at'] = time.time()
        save('result.json', outcome)
        print(json.dumps(outcome), flush=True)


if __name__ == '__main__':
    main()
