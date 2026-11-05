# Daly charging-switch and Edecoa lamp test — 2026-11-01

## Result

The Bluetooth charge-switch command works on `DL-401710013BB4`: the BMS reported
charging disabled, then enabled again after restoration. Discharging remained
enabled. Sustained mains bypass without battery charging is **not established**.
The lamp's behavior requires the user's observation.

The test supplied mains for about 20 seconds before disabling charging. Baseline
battery current was approximately +25.2 A at 13.3 V. The first disabled-switch
sample still showed +19.8 A, so the supervisor aborted rather than completing
the planned 30-second observation. A later zero-current sample occurred with the
plug already off and therefore does not prove mains bypass.

A raw BMS status value `0000000000000010` appeared during the experiment and
persisted after both MOS switches were enabled again. Its exact meaning on this
firmware remains unverified. The upstream D2 error map marks register `0x3D`
reserved, but that is insufficient to declare this device's value harmless.
Automatic approval review rejected a proposed low-16-bit alarm filter because
it could mask safety alarms. **That proposal was not applied.** Existing alarm
checks remain intact; no battery thresholds or discharge controls were written.

## Restoration and artifacts

- Both physical MOS readbacks were enabled before rollback; the plug was off.
- Original settings: Automatic, 90% target, 07:00 finish, 100 ore/kWh ceiling.
- The exact previously verified image, `.esphome/releases/controller-100420/firmware.ota.bin`,
  was uploaded again after the test. See `.esphome/bypass-test/restored-status.json`
  for the subsequent live verification.
- Raw experiment samples: `.esphome/bypass-test/samples.jsonl`.
- Final rollback upload log: `.esphome/bypass-test/final-rollback-upload.log`.
- Initial source checkpoint remains Git commit `77081d9`.

## Experimental source (not installed after rollback)

The uncommitted source adds a bounded commissioning endpoint at
`/charging/api/bypass-test`. POST starts the sequence; GET reports its stage and
MOS/current/power readings. It writes only D2 Modbus register `0x00A5`, function
`0x06`, with CRC16: zero disables charging, one enables it. It never writes
discharge register `0x00A6`.

The experiment records a persistent recovery marker before starting and retries
charge-enable after an interrupted test/restart until physical readback confirms
restoration. Off aborts the test. All BMS alarm checks remain enabled.

Bring-up exposed inconsistent header ordering for the history-size definition
and excessive HTTP stack use. The source includes the generated definitions
before the shared core header and isolates the test handler with a heap response.
Measured HTTP handler stack allocation returned from 1392 to 656 bytes; the
separate test handler uses 192 bytes. Portable command/MOS tests passed with
AddressSanitizer and UndefinedBehaviorSanitizer, with both history configurations.
The test POST response was subsequently corrected from unsupported HTTP 202 to
HTTP 200; this final source correction has not been flashed.

Further testing should first identify the persistent BMS status value and account
for possible current-sensor settling. Do not infer charging isolation or
uninterrupted mains supply from switch readback alone.

Protocol references:

- https://github.com/fl4p/batmon-ha/blob/master/bmslib/models/daly2.py
- https://github.com/syssi/esphome-daly-bms/blob/main/components/daly_bms_ble/daly_bms_ble.cpp
