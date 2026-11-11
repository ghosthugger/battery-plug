# Standalone battery charging controller

Athom Plug V3 (ESP32-C3, 4 MB flash), Daly `DL-401710013BB4`, Swedish SE3 prices.
The plug reads the battery over Bluetooth, fetches Elklocka's underlying Gridio
price feed over verified HTTPS, plans charging locally, and retains bounded
history. No Home Assistant, cloud account or logging server is required.

- **Control page:** http://192.168.1.254/charging
- **Diagnostics / OTA upload:** http://192.168.1.254/
- **Local hostname:** `athom-smart-plug-v3-c416f8.local` (where mDNS is supported).
- Reserve the address in the router using MAC `14:63:93:C4:16:F8` if desired.

The agreed plan, including the description of the original state, is saved in
[AUTOMATIC_CHARGING_IMPLEMENTATION_PLAN.md](AUTOMATIC_CHARGING_IMPLEMENTATION_PLAN.md).

## Charging controls

### Phone access to the BMS

Under **Phone Bluetooth access** on `/charging`, **Disconnect BMS for 5 minutes**
releases the Bluetooth connection without unplugging mains. Start with mains
already enabled, a connected BMS and fresh fault-free battery/meter readings;
select Always charging first if needed. Then connect the Daly phone app.
Close/disconnect the phone app before pressing **Reconnect BMS**.

This is an explicit temporary diagnostic exception to stale-BMS shutdown:
mains remains on, including charging, while live BMS fault monitoring is
unavailable. Cached readings are marked stale immediately, and battery-derived
savings/load learning are suspended. The BMS's own protection is unchanged;
no MOS commands or BMS configuration writes are sent. Meter, overcurrent and
OTA stops remain authoritative. A known BMS inhibit prevents entry or ends the
window. New BMS faults cannot be observed while disconnected.

Automatic reconnection starts after five minutes, followed by at most 45 seconds
to receive a new BMS reading. Normal control then resumes; unsuccessful
reconnection invokes existing stale-BMS shutdown. Repeated requests cannot extend
either timer. The window is not persisted across restart and does not change the
selected charging mode. API: POST `/charging/api/bluetooth` with form `action`
equal to `disconnect` or `reconnect`; status exposes `bluetooth_connected`,
`bluetooth_handoff` and `bluetooth_remaining` (seconds).

### Operating modes

Choose **Automatic** for cost optimisation or **Always charging** to keep mains
continuously enabled. Both modes persist across restart and neither expires.
There is no Off mode, Charge now, manual target or manual finish time.

Automatic uses the existing 101-level cost optimiser and mandatory configurable
low-battery recovery (currently 10–20%). Recovery start must be at least 1%; it
cannot be disabled. Always charging ignores prices, SOC targets and time: mains
continues supplying the appliances when the battery is full, and the charger
regulates charging. The plug never commands the BMS MOSFETs.

The physical button toggles Automatic/Always charging. The diagnostics switch is
named Always charging: turning it off selects Automatic, never a disabled state.
Existing legacy Off/Charge now records migrate to Automatic; Always charging
is saved separately in the existing versioned mode word. Old mode API calls
are rejected, as are retired manual settings. Overcurrent remains latched across
restart and needs explicit acknowledgement by selecting a supported mode.

Existing BMS fault, stale BMS/meter, overcurrent and OTA stops remain authoritative.
The modes eliminate deliberate/expired Off states; they cannot guarantee freezer
continuity during hardware faults, telemetry failure or loss of mains.

Model estimates remain configurable: 4 kWh, 400 W AC charger, 90% charging/inverter
efficiency, 10 W idle, 60 W initial appliance estimate. Automatic learns consumption
from battery measurements while mains is off, using bounded seven-day history.
It extends published prices with a recent hourly-price median forecast and values
stored energy using a low-quartile continuation estimate. The assumed extra
variable tariff is 70 öre/kWh including VAT; adjust it to your bill.

## Prices and stopping rules

Gridio's raw EUR/MWh values are converted using Elklocka's calculation:

```text
öre/kWh including VAT = raw EUR/MWh × SEK/EUR × 1.25 ÷ 10
```

Displayed prices and recorded historical costs include spot+VAT only. They
exclude the configurable marginal adder, supplier/grid fixed charges and battery
wear. Automatic includes the marginal adder in its optimisation. The legacy ceiling is retained only inside old storage/history records; it is not an active control. Negative prices are supported. Missing current prices stop economic
Automatic starts; published cached intervals remain usable during outages.
Recovery can override unavailable prices, as before.

The controller polls the BMS every ten seconds and stops for a BMS reading older
than 60 seconds, stale/invalid power data (30 seconds), a BMS
charge prohibition or the plug's overcurrent latch. Fault stops and low-battery recovery override the two-minute ordinary switch cooldown. Always charging does not wait for a clock or prices. Automatic also requires a clock except during low-battery recovery. The BMS's own
settings and MOSFET configuration are never changed. The charger's and BMS's
hardware protections remain responsible for battery protection.


## On-device history

- **Last 24 hours:** 288 five-minute aggregates: SOC/range, charger power,
  charging time, price/ceiling, measured grid energy and estimated price cost.
- **Last 180 days:** daily energy/cost, unknown-price energy, charging duration,
  observed SOC range and deadlines met/missed; daily charts and monthly totals.
- **Recent events:** 128 bounded mode, relay, decision, connectivity and deadline
  events.

All page assets are embedded; charts use local JavaScript and SVG. The desktop
and mobile page works without an external chart service. Empty history is not
backfilled. Observation gaps are explicit. Current incomplete buckets may be
lost after abrupt power failure, and partial days are not presented as full-day
measurements. Daily totals exclude the current unfinished five-minute bucket.

Records are versioned and checksum protected in NVS namespace `charging_v1`.
History is stored in hourly/monthly blocks to avoid rewriting a large ring on
every sample. Wi-Fi credentials and the original partition layout are retained.
The schema rejects corrupt records and can recover partial daily totals from
retained five-minute buckets. Source assertions bound retained history/price
buffers to 24 KB and persistent history (including NVS overhead) to 48 KB.

## Local interface

All routes are relative to `/charging/api/`:

| Method | Route | Result |
| --- | --- | --- |
| GET | `status` | Live state and settings, including `reserve_threshold`, `reserve_recovery`, `reserve_active`, planning reasons, connectivity and memory diagnostics. |
| POST | `settings` | Form-encoded partial settings: `capacity_kwh`, `charger_w`, `efficiency`, `reserve_threshold`, `reserve_recovery`, `marginal_adder`, `charge_efficiency`, `discharge_efficiency`, `idle_w`, `fallback_load_w`. |
| POST | `mode` | Form field `mode=auto` or `always`. Both persist without a cutoff. |
| GET | `schedule` | Published price periods and the next committed mains decision. |
| GET | `history` | Bounded 24-hour aggregates, streamed in chunks. |
| GET | `days` | Retained daily summaries, streamed in chunks. |
| GET | `events` | Retained events, streamed in chunks. |

Changes are validated and applied on ESPHome's main loop. Native percentage/time
entities and the custom dashboard share state. These local interfaces retain the existing LAN access model.

## Build and tests

Use ESPHome **2026.10.1** and `uv`:

```sh
./tools/build.sh
uv tool run --from esphome==2026.10.1 esphome upload battery-plug.yaml --device 192.168.1.254
```

The build produces `.esphome/build/athom-smart-plug-v3/build/firmware.ota.bin`.
Upload **the OTA image**, not the factory image, through the web form. The build
helper uses isolated CMake/Ninja because this Mac's system wrappers lack their
underlying executables. It selects public PyPI because the configured company
package mirror was unavailable.

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -g tests/core_test.cpp -o .esphome/core-test
.esphome/core-test
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -g tests/optimizer_test.cpp -o .esphome/optimizer-test
.esphome/optimizer-test
uv tool run --with playwright python tests/dashboard_test.py
uv tool run --with playwright python tests/live_dashboard_test.py
```

The browser scripts use a fresh headless Chrome profile, not your existing
browser session. The fixture test does not operate the plug. The live browser
test is read-only. Native tests also cover optimiser fault precedence, non-expiring modes, fault stops, recovery, partial slots and observation history. Legacy tests cover cheapest-period scheduling, ceiling
failures, manual overrides and stops, negative prices, loads, provisional
horizons, Daly CRC/fragmentation/alarm interpretation, energy/cost integration,
ring rollover, months of daily records, and spring/autumn DST edge cases.

`tools/soak_monitor.py` is a finite, read-only acceptance observer on this computer,
not a logging server or a runtime dependency. It writes samples and a summary
under `.esphome/soak/` and checks the agreed runtime memory limits after 24 hours:

```sh
python3 tools/soak_monitor.py --hours 24
```

The charger can operate independently if this computer sleeps or the observer
stops. Observer failures while the computer sleeps are not proof of a plug fault.
See [VALIDATION.md](VALIDATION.md) for measured hardware checks and soak status.

## Historical backtest

`tests/backtest/se3-fridge-freezer-input.csv` contains the source trace used for
this change: SE3 Nord Pool-derived prices from Elklocka's Gridio provider,
reconstructed historical SEK conversion, and synthetic fridge/freezer demand.
July is prior history; evaluation is 1 August–31 October 2026. This is not a
recording of your appliances or a direct exchange export. Price release is
assumed at 13:15 Stockholm; a 15:00 sensitivity is also tested.

With NumPy and pandas installed:

```sh
python tools/backtest_optimizer.py
python tests/backtest/verify_optimizer.py
```

The backtest compiles the exact production optimiser with the native C++
compiler and writes reports/traces under `.esphome/optimizer-backtest/`.
The default trace has no network/device dependency. Read the committed
`tests/backtest/RESULTS.txt` for assumptions, results and limits.

## Source layout

- `battery-plug.yaml`: explicit Athom hardware, safe relay ownership, shared UI
  entities and local controller wiring.
- `components/charging_controller/`: portable scheduling/history/Daly decoding,
  ESPHome BLE/HTTPS/NVS/web integration and embedded dashboard.
- `vendor/athom-smart-plug-v3.yaml`: the manufacturer's original configuration
  retained as hardware provenance; it is no longer included by the release.

The existing `../daly-read.swift` remains a read-only Mac protocol probe.

## Thirty-day estimated savings

The local dashboard now retains 30 Stockholm calendar-day summaries (including
today), independently of the detailed 24-hour history. Collection begins with
this firmware; older observations cannot provide the missing battery-current
samples. The counter and daily signed bars compare estimated direct mains cost
with the plug's metered grid cost, including the configured variable tariff adder.

Appliance output is estimated from measured AC input and battery DC current:
with mains on, subtract positive battery charging power divided by charging
efficiency; add any battery-assisted output. With mains off, subtract configured
inverter idle power from battery draw, then apply discharge efficiency. Meter
and BMS samples are asynchronous, so this is an operating-cost estimate, not a
separate appliance meter or guaranteed bill saving. Hardware cost, battery wear
and fixed tariff charges are excluded.

Savings = estimated direct-mains cost − metered grid cost + stored-energy change.
The last term integrates signed battery DC energy at one fixed reference value:
the initial lower-quartile available price estimate plus tariff adder, divided by
charging efficiency (current published price fallback, zero floor). The reference
is locked at the first priced observation and never revalues old history. This
avoids assigning free value to initially stored energy or penalising energy still
in the battery. It cancels over equal stored-energy charge/discharge cycles.
The dashboard exposes all three terms; negative savings are retained.

Only published prices and fresh meter/BMS observations are used. Exclude gaps,
restarts, long sampling intervals and the first 20 seconds after relay changes;
show included/excluded hours. DST days have their actual 23/24/25-hour duration.
Tariff/model changes apply only to future observations. Summaries use 972 bytes
of retained RAM, are checksummed in a separate NVS record every five minutes and
at shutdown, and survive firmware updates/restarts. Sudden power loss can lose
up to five minutes. Read-only JSON: `/charging/api/savings`.

Validation: `tests/savings_test.cpp` exercises energy valuation, conversion
losses, signed prices, missing data, restart, rollback, price boundaries, DST and
30-day expiry. `tests/dashboard_test.py` checks counter arithmetic and signed
bars at desktop/mobile widths alongside existing controls. The production build
and existing controller/optimiser tests must pass before deployment.

## Predicted charging windows

The price chart shades the current mains request in solid green and provisional
future mains periods in pale dashed green. It shows only the published-price
window; the optimiser still includes its existing estimated-price extension
when choosing. Shading denotes 15-minute periods that are expected to contain
mains operation; recovery may enable mains for only part of a period. Tooltips
show estimated on-minutes. The next predicted period is also written below the
chart. All forecasts are recomputed with the normal optimiser calculation.

A display-only packed policy table adds 7,575 bytes temporarily during a solve;
only 400 bytes of per-published-slot predicted on-seconds remain afterward.
Forward simulation uses the exact current decision and remaining current-slot
duration, then nearest SOC-grid future choices and the physical recovery model.
Future predictions are approximate: grid rounding and strict off-on ties can
produce different windows from repeatedly solving at exact future SOC. There is
no claimed one-slot timing-error bound. Changing demand/new prices can also move
windows. Current decisions, safety stops and the cost optimisation are unchanged.
Policy allocation failure omits predictions while retaining normal control.
Stale/faulted telemetry, expired calculations and changed prices/settings hide
outdated forecasts. Always charging shows continuous future mains availability.
Forecasts are regenerated after restart; the separate savings history persists.

Validation: `tests/projection_test.cpp` verifies bit bounds, 100 unchanged current
actions and exact Q values with/without capture, a cheap-window scenario,
partial first slots and recovery. It compares the projected path with exact
continuous-state suffix solves (one differing action out of 36 in that scenario).
Dashboard fixtures cover current/future styles and unavailable predictions at
1200 px and 390 px. This scenario is a correctness check, not an accuracy claim
for all consumption patterns.

## Persistent BMS charging diagnostics

The dashboard's BMS charging diagnostics table retains the latest 32 snapshots:
first valid reading after startup, each change of charge permission, and changes
in raw MOS/alarm details while charging is prohibited. Snapshots preserve the
unfiltered four alarm words, raw charge/discharge MOS registers, SOC, pack
voltage/current, cell/temperature min/max, plug power and relay state before
control responds. This distinguishes a switch readback from an alarm condition;
it does not assume that a charging inhibit means battery full.

Snapshots are 60 bytes; the entire ring and tracking state use 1,944 bytes of
retained RAM. Each changed slot is individually versioned/checksummed and
committed immediately after control evaluation. Unchanged 10-second samples do
not cause flash writes. A fixed 32-slot ring overwrites the oldest entry and
survives restart/OTA independently of savings/history. A baseline is recorded
after every startup; time is null/unavailable until the clock is valid, with
uptime retained instead. Events shorter than the BMS polling interval can be
missed. A power cut during a flash commit can lose that latest snapshot.

Read-only JSON: `/charging/api/bms-diagnostics`; `/charging/api/status` also
exposes live `charge_mos`, `discharge_mos` and `charge_allowed`. Existing safety
logic and BMS read-only operation are preserved; a diagnostic transition forces
the existing control evaluation before its new flash write. There is no alarm
masking, MOS switching command, or change to the optimiser.

For a deliberate normal charging observation from a computer with LAN access:

```sh
python tools/observe_charge_inhibit.py --always-charging --minutes 120 \
  --output-dir .esphome/charge-inhibit-test
```

Without `--always-charging`, this tool is read-only. With it, the tool enables
Always charging, observes new diagnostic snapshots and returns to Automatic on
an inhibit, timeout, unhealthy telemetry/storage, interruption or a detected
external mode change. Returning to Automatic resumes normal price/recovery
control; it is not an unconditional charger-off command. Existing protective
stops remain authoritative. No BMS settings/registers are written. Keep the
supervising computer awake and connected; power/network loss can prevent mode
restoration, in which case select Automatic on the dashboard. It writes
`latest.json`, minute samples and a final `result.json` in the output directory.

Validation: native `tests/bms_diagnostics_test.cpp` covers exact raw data, CRC
layout/corruption, change-only capture, overwrite and out-of-order restart
restoration. `tests/observe_charge_inhibit_test.py` checks inhibit/timeout/error
restoration and the read-only default without accessing hardware. Existing
controller/optimiser/savings/projection tests and desktop/mobile checks remain
required, followed by an actual baseline/restart persistence check on the plug.

## Read-only BMS configuration inspection

`GET /charging/api/bms-settings` returns the cached D2 configuration block
`0x0080..0x00A8`, including configured capacity/chemistry, cell and pack voltage
protection values, and balancing start voltage/delta. Raw words are retained
alongside the interpreted fields. This is an inspection endpoint, not a settings
editor; none of these values replaces the controller's protection logic.

After a complete normal status response, the plug sends only a Modbus **read**
(function 0x03), start0x0080 count41. The 87-byte settings reply has an isolated
length/CRC parser. Unsupported reads time out in 1.5 seconds, at most three
attempts spaced at least30 seconds. Normal ten-second status polling takes
priority. Settings replies never mark battery telemetry fresh. The query adds
under300 bytes of settings/parser buffers plus small state and makes no BMS or
NVS settings writes. Configuration is read again on plug startup, so restart
when inspecting BMS settings changed separately in the phone app.

`balancing_status` in live status is the raw *currently active* readback; zero
does not prove balancing is disabled. Balancer enable register0xCF, release
voltage and protection delay are not exposed because their mapping is not
confirmed for this firmware. Read success confirms framing/CRC, not that the
settings are appropriate for the cells. Normal protective stops remain intact.

Mapping provenance: upstream `syssi/esphome-daly-bms` settings decoder and
`docs/dalyModbusProtocol.xlsx`. Native `tests/daly_settings_test.cpp` verifies
fragmentation/CRC, exact mapping, rejection of normal status and exception
frames, reset recovery and the read-only request CRC.

## Full-charge pause until 95%

Automatic mode disconnects mains at 100% SOC, or on the captured Daly
cell-overvoltage charging cutoff (register 0x3A bit 1) above 95% SOC. It then
keeps mains off until a fresh valid BMS reading reaches 95% or lower, regardless
of cheap electricity or subsequent clearing of the BMS cutoff. At 95% the
normal price optimiser resumes; charging is not forced. Other BMS faults are
not treated as full charge. Existing fault stops retain priority.

The pause latch is saved in NVS only when it changes and restored after restart.
It uses one boolean plus a small NVS record, with no new history buffers.
Low-battery recovery and bounded phone Bluetooth handoff retain priority.
Always charging continues to override economic pauses; returning to Automatic
respects the retained latch. The dashboard reason explains the pause, and status
JSON exposes `full_charge_paused` and `full_charge_resume` (95). This reduces
repeated top charging; it does not correct cell imbalance or charger settings.

## Optimiser model matches the full-charge pause — 2026-11-08

`compact-mpc-v3` removes the assumption that Automatic can keep mains connected
at full charge. Its physical model stops mains exactly at full, draws battery
power until 95%, and only then allows another economic mains request. The solver
and dashboard projection both carry the actual saved full-charge pause plus
the independent low-battery recovery latch into future intervals. Recovery keeps
priority, including overlapping configured thresholds. Always charging retains
its separate manual behavior; no BMS writes were added. Early BMS overvoltage
cutoffs remain measured safety events, not predicted cell-voltage behavior.

The 101 energy nodes put 20 intervals in the 95–100% band and include the 95%
boundary exactly. Temporary solver memory is 14,144 bytes and the optional
projection table is 15,150 bytes (together 29,294 bytes, released after a solve).
No additional persistent history or flash writes were added. HTTP fetching and
solving remain serialized. This corrects economic decisions; it does not prohibit
all future top-ups when their modeled total cost is lower.

Historical comparison and live-snapshot decision evidence are saved in
`tests/backtest/full-pause-model-results.json`, with independent verification in
`tests/backtest/full-pause-model-verification.json`. Reproduce the comparison:

```sh
uv tool run --with numpy --with pandas python tools/backtest_optimizer.py --case compact_101 --model mismatch --output .esphome/full-pause-model-backtest/mismatch
uv tool run --with numpy --with pandas python tools/backtest_optimizer.py --case compact_101 --case compact_1001 --case compact_101_float64 --case compact_101_high_load --case compact_101_idle20W --case compact_101_late_publication --model full-pause --output .esphome/full-pause-model-backtest/corrected
uv tool run --with numpy --with pandas python tests/backtest/verify_optimizer.py --artifacts .esphome/full-pause-model-backtest/corrected
```

## Invoice-based electricity charges

The running plug's VAT-inclusive extra variable charges were updated to
**81.60 öre/kWh** on 11 November 2026, using the supplied Ellevio and Kärnfull bills.
See [TARIFF_MODEL.md](TARIFF_MODEL.md) for tax/VAT calculation, excluded fixed
fees, supplier charges, the pending billing-cadence confirmation and how the setting is applied.
