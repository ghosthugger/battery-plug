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

Choose **Automatic** to enable scheduled charging and low-battery recovery.
**Off** prevents automatic starts, including low-battery recovery.

| Mode | Behavior |
| --- | --- |
| Off | Stops the charger and suppresses scheduled starts. |
| Automatic | Minimises estimated electricity cost using published prices and a simple forecast; mains also charges the battery up to full. |
| Charge now | Stops at the manual target/deadline, stale data or a charge-prohibiting fault. Returns to Off on completion. |

Automatic uses a 101-level dynamic programme every 15 minutes, with the actual
low-battery recovery latch included in its model. It has two physical modes:
battery supplies appliances; or mains supplies appliances and charges. It never
assumes that charging can be disabled independently. It has no daily SOC target,
finish time or price ceiling. The target and finish fields apply to Charge now.

Physical estimates are editable: 4 kWh, 400 W AC charger, 90% charging and inverter
efficiency, 10 W inverter idle, 60 W initial appliance estimate. Automatic estimates
appliance consumption from battery DC measurements while mains is off, using a
seven-day weighted mean. It cannot separate appliance/charger power on mains.

Published prices are extended by up to 24 h of flat forecast from the median of
seven days of completed hourly prices. A low-quartile continuation value avoids
emptying the battery merely because published prices end. At startup, existing
published prices supply an initial estimate. Forecasts are not measured prices.
The configurable extra marginal charge defaults to 70 öre/kWh including VAT;
set it to the variable grid/tax/supplier charge on your bill. No savings are
guaranteed; efficiencies, idle draw and consumption are estimates.

Low-battery mains recovery defaults to **5%**. In Automatic or Charge now, SOC
at or below the configurable threshold immediately restores mains and charging,
regardless of price, target, deadline or clock synchronization. Recovery remains
active until SOC reaches the configurable recovery stop percentage (default 10%), then
normal control resumes. The recovery stop must be above the start threshold and
at most 100%. Both settings and the recovery state survive restart. Set
`reserve_threshold` to 0 to disable it (allowed range 0–95%); this also suspends economic Automatic decisions. Explicit Off and
existing BMS, meter, overcurrent and OTA stops remain authoritative. This can buy
expensive electricity; it cannot guarantee continuity if mains or telemetry is
lost, the BMS prohibits charging, or the load exceeds available charging power.

A short physical button press stops an active charge or requests Charge now.
The switch on the diagnostics page uses the same controller; it cannot bypass
BMS/data checks. Automatic mode persists across restarts, but a manual charge
never resumes after reboot. The physical relay always starts off until required
inputs are valid. The generic Bluetooth proxy, BLE provisioning and unused Home
Assistant native API have been removed to save memory. Native OTA on port 3232
and the web OTA form remain available.

## Prices and stopping rules

Gridio's raw EUR/MWh values are converted using Elklocka's calculation:

```text
öre/kWh including VAT = raw EUR/MWh × SEK/EUR × 1.25 ÷ 10
```

Displayed prices and recorded historical costs include spot+VAT only. They
exclude the configurable marginal adder, supplier/grid fixed charges and battery
wear. Automatic includes the marginal adder in its optimisation. The older
ceiling remains in stored/API settings for compatibility but does not constrain
Automatic. Negative prices are supported. Missing current prices stop economic
Automatic starts; published cached intervals remain usable during outages.
Recovery can override unavailable prices, as before.

The controller polls the BMS every ten seconds and stops for a BMS reading older
than 60 seconds, stale/invalid power data (30 seconds), invalid clock outside low-battery recovery, a BMS
charge prohibition or the plug's overcurrent latch. Off, fault and manual target/deadline stops override the two-minute switch cooldown. The BMS's own
settings and MOSFET configuration are never changed. The charger's and BMS's
hardware protections remain responsible for battery protection.

Stockholm daily deadlines use deterministic civil-date calculations, including
23/25-hour days. An ambiguous autumn deadline selects the next real occurrence;
a nonexistent spring deadline moves forward by the missing hour.

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
| POST | `settings` | Form-encoded partial settings: `target`, `finish` (`HH:MM`), `ceiling`, `capacity_kwh`, `charger_w`, `efficiency`, `reserve_threshold`, `reserve_recovery`, `marginal_adder`, `charge_efficiency`, `discharge_efficiency`, `idle_w`, `fallback_load_w`. |
| POST | `mode` | Form field `mode=off`, `auto` or `now`. |
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
test is read-only. Native tests also cover optimiser fault precedence, manual-only limits, recovery, partial slots and observation history. Legacy tests cover cheapest-period scheduling, ceiling
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
