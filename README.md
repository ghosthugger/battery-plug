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
