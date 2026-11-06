"""Exercise the test supervisor's restoration paths without contacting hardware."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import URLError
from urllib.parse import parse_qs

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('observer', root/'tools/observe_charge_inhibit.py')
observer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(observer)


class SupervisorTest(unittest.TestCase):
    def run_supervisor(self, inhibit=True, lost_response=False, enable=True):
        state = dict(mode='auto', reads=0, clock=0, writes=[])

        def request(req, timeout):
            endpoint = req.full_url.rsplit('/', 1)[-1]
            if endpoint == 'mode':
                mode = parse_qs(req.data.decode())['mode'][0]
                state['mode'] = mode
                state['writes'].append(mode)
                if lost_response and mode == 'always':
                    raise URLError('Response lost after applying mode')
                payload = {'ok': True}
            elif endpoint == 'status':
                state['reads'] += 1
                payload = dict(mode=state['mode'], bms_fresh=True, meter_fresh=True,
                               storage_ok=True, charge_allowed=not(inhibit and state['reads']>=3),
                               soc=100, power=400, reason='test')
            else:
                self.assertEqual(endpoint, 'bms-diagnostics')
                tripped = inhibit and state['reads']>=3
                payload = {'snapshots': [dict(sequence=2 if tripped else 1, charge_allowed=not tripped)]}
            return io.StringIO(json.dumps(payload))

        def sleep(seconds):
            state['clock'] += seconds

        with tempfile.TemporaryDirectory() as folder:
            argv = ['observer', '--output-dir', folder, '--minutes', '1']
            if enable:
                argv.append('--always-charging')
            with patch('sys.argv', argv), patch.object(observer, 'urlopen', request), \
                 patch.object(observer.time, 'monotonic', lambda:state['clock']), \
                 patch.object(observer.time, 'sleep', sleep), patch.object(observer.signal, 'signal'), \
                 contextlib.redirect_stdout(io.StringIO()):
                if lost_response:
                    with self.assertRaises(URLError):
                        observer.main()
                else:
                    observer.main()
            result = json.loads((Path(folder)/'result.json').read_text())
        return state, result

    def test_inhibit_restores_automatic(self):
        state, result = self.run_supervisor()
        self.assertEqual(state['writes'], ['always', 'auto'])
        self.assertEqual(result['reason'], 'charge_inhibit_observed')
        self.assertTrue(result['automatic_restored'])

    def test_post_response_loss_still_restores(self):
        state, result = self.run_supervisor(lost_response=True)
        self.assertEqual(state['writes'], ['always', 'auto'])
        self.assertTrue(result['automatic_restored'])

    def test_timeout_restores(self):
        state, result = self.run_supervisor(inhibit=False)
        self.assertEqual(state['writes'], ['always', 'auto'])
        self.assertEqual(result['reason'], 'timeout')

    def test_default_is_read_only(self):
        state, result = self.run_supervisor(enable=False)
        self.assertEqual(state['writes'], [])
        self.assertEqual(result['reason'], 'charge_inhibit_observed')


if __name__ == '__main__':
    unittest.main()
