# Validation record

2026-11-01. Target: Athom Plug V3 `14:63:93:C4:16:F8` and Daly
`DL-401710013BB4`, SE3.

## Completed

- ESPHome configuration validation and firmware builds, with the existing 4 MB
  partition layout and at least 64 KB OTA headroom.
- AddressSanitizer/UndefinedBehaviorSanitizer native tests for the portable
  controller, Daly decoder, price-object parsing and history accounting.
- Explicit 23-hour, 25-hour, missing spring time and repeated autumn time tests.
- Bounded history tests with more than one day of bucket rollover and 220 days
  of simulated deadline summaries.
- Desktop (1200 px) and mobile (390 px) browser tests with all five SVG graphs,
  settings edits, mode buttons and 180 days of fixture data; no JavaScript errors
  or page-wide horizontal overflow.
- Read-only desktop/mobile browser checks against the actual plug.
- Live BMS readings: approximately 91.7%, 13.2 V, valid cell/temperature/current
  data, zero alarm bits. Valid live SE3 prices via verified HTTPS.
- Actual charger test: with a temporary zero ceiling, Automatic remained off
  with “Price above limit”. Charge now overrode price and drew **395 W**; the
  BMS reported approximately **25 A charging current**. Restoring the reached
  90% target stopped the relay and returned mode to Off. Original settings were
  restored in a `finally` cleanup.

## Bring-up findings resolved in source

- BLE node loops can run before the controller's component setup: guarded until
  initialization is complete.
- Avoided libc timezone/mktime dependencies; Stockholm conversions are portable,
  explicit, and covered by DST tests.
- Moved the large status JSON buffer off the small HTTP-task stack; compile-time
  `-Wstack-usage=2048` checks guard custom handlers.
- Replaced the full-array JSON tree and duplicate response string with per-object
  parsing, and removed the unused native Home Assistant API to improve margins.
- Kept long-term cost accumulation in 64 bits and bounded parser/history buffers
  with static assertions.

## Final deployment checks

Final runtime memory, restart persistence, release size and the 24-hour observer
status are recorded here after the optimized release is installed. A 24-hour
soak is not considered complete merely because short acceptance tests pass.
## Low-battery recovery addition — 2026-11-01

- Added configurable reserve start and stop percentages (defaults 5% and 10%;
  a start threshold of 0 disables recovery). Stop must exceed start and be at
  most 100%. Earlier installations retain their previous start + 5% release
  setting when the new stop setting is absent.
  BMS access remains read-only; the earlier MOS test is not in the active source.
- Sanitized native tests pass for both history configurations, including reserve
  boundaries, hysteresis, price/deadline/clock overrides, restored recovery state,
  manual Off, invalid SOC and BMS/meter/fault stops.
- Desktop/mobile browser fixtures pass, including threshold editing and saving.
- ESPHome compilation passes with about 249 KB application-slot headroom.
- Live test temporarily raised the threshold to 95% at SOC 90.4%, target 90%,
  price 188.753 ore/kWh and ceiling 100. The relay enabled with the low-battery
  reason, demonstrating the target and price overrides. The BMS intermittently
  prohibited charging and the existing fault stop operated. Charger input stayed
  below 20 W; normal charging power was **not verified**. No BMS settings were
  changed. The original 5% threshold was restored and the relay was off afterward.
- Evidence and restart verification: `.esphome/low-battery-recovery/`.
- This is not a freezer-continuity guarantee: existing fault stops retain priority.
  The BMS charging-permission issue needs checking before relying on recovery.
- Restart verification passed: original mode, target, finish time, ceiling and
  5% reserve persisted. Live desktop/mobile dashboard checks pass with the new
  setting and release explanation. Runtime minimum heap after restart was
  59,316 bytes (below the earlier plan's 60 KiB target); minimum largest block
  was 49,152 bytes. No extra server or BMS write endpoint is installed.
- Configurable recovery stop: sanitized controller tests cover 30% and 100%
  release boundaries. Desktop/mobile fixtures save a 30% stop and reject a stop
  at the start threshold. Live API checks reject below/equal/out-of-range/NaN
  values. A 30% stop persisted across an actual restart; original 5%/10% limits
  were restored. No charging was triggered by this configuration test.

## Compact cost optimiser — 2026-11-05

- Native production core backtested on the previously validated SE3 Aug–Oct
  quarter-hour trace and synthetic fridge/freezer load: **44.666 SEK saving**
  versus a 231.125 SEK direct-mains baseline. The old controller with the user's
  latest 100% target/100-öre ceiling saved 9.407 SEK under the same assumptions.
  These are modeled costs, not measured household savings.
- Models the actual two modes and configured 10–20% recovery latch; no mains-only
  action or BMS MOS writes. On-device-feasible seven-day price/load estimates,
  off-only consumption observations, float32/101 energy levels. 1001 levels
  gave 44.421 SEK; float64 at 101 levels gave the identical base trajectory.
- Independent verifier: 502 physical transitions, 516 solver cases, future-price
  causality tests and 53,016 saved trace intervals passed. Native ASan/UBSan tests
  pass with both 0/180-day core history configurations and cover Automatic/manual
  differences, fault/Off precedence, recovery, partial slots and bounded history.
- Desktop/mobile browser fixtures pass with all charts and new editable model
  settings; no JS errors or horizontal overflow. Plan and README updated.
- ESPHome 2026.10.1 final build: OTA image 1,600,416 bytes; application slot
  1,835,008 bytes, leaving 234,592 bytes. Transient solver workspace 7,680 bytes;
  bounded observation history 2,704 bytes. Solver and HTTPS fetch do not overlap.
- Detailed source/data hashes, results and independent verification are committed
  under `tests/backtest/`. The old OTA image is retained locally under
  `.esphome/pre-optimizer/firmware.ota.bin`.
- Deployment succeeded using the normal Terminal process because the detached
  background session could not reach the LAN. Source commit `f8167da`; native
  ESPHome OTA confirmed success and `/charging/api/status` reports
  `compact-mpc-v1`. The image SHA256 is recorded in `tests/backtest/deployment.json`.
- Existing Automatic mode, target 100%, finish 07:00, capacity 4 kWh, charger 400 W,
  legacy efficiency/ceiling, recovery 10–20% and disabled long-term history all
  survived the OTA restart. Existing 24-hour history remains visible. BMS access
  is still read-only; live alarms are zero.
- Short observation: 25 healthy post-startup BMS samples; fresh meter, valid prices,
  healthy storage, no unexpected reboot. Initial solve 397 ms; a same-value settings
  save caused a 371 ms replan and updated the learned load from 60 W to 174.27 W. The
  save did not change any settings. New optimiser settings/history writes passed;
  their survival through a separate later power cycle was not tested here.
- Live desktop 1200 px/mobile 390 px dashboard tests passed, including installed
  algorithm/model settings, all charts, no JavaScript errors or horizontal overflow.
- Minimum reported heap 56,940 B; minimum free heap seen in status 56,060 B; minimum
  largest free block after live UI 43,008 B. The previous 60 KiB free-heap goal is still
  unmet during initial/TLS activity, while the 32 KiB largest-block goal is met.
  No allocation failures occurred; steady free heap was around 80 KB. These are
  short-run measurements, not a 24-hour soak or a freezer-continuity guarantee.
- No 24-hour soak or overnight charging cycle is claimed complete. Detailed
  before/after data and upload log are under `.esphome/optimizer-*`; the committed
  deployment summary is `tests/backtest/deployment.json`.

## Non-expiring Automatic / Always charging modes — 2026-11-05

- Removed Off/Charge now and manual percentage/finish controls from dashboard,
  diagnostics entities and mode/settings API. Always charging ignores time, price
  and percentage cutoffs. Both modes persist; old disabled/manual states migrate
  to Automatic. Recovery cannot be disabled; existing 10–20% limits are preserved.
- Physical button toggles the two supported modes. Diagnostics Always charging
  switch OFF means Automatic. Existing data/BMS/OTA protections are retained;
  overcurrent now persists as its own protective latch, independently of mode.
- Sanitized native tests passed for legacy regression and non-expiring modes,
  full SOC, elapsed days, absent clock/prices, fault/resume, mode migration,
  mandatory recovery, observation history and partial slots. Desktop/mobile
  fixtures passed with no retired controls or JavaScript errors.
- Pinned ESPHome build and OTA upload passed: source `344ea81`, algorithm
  `compact-mpc-v2`, image 1,589,488 B with 245,520 B application-slot headroom.
- Live API rejects Off, Charge now, reserve 0 and retired target/finish settings.
  Existing model/capacity/reserve/history settings survived the update. Target,
  ceiling and manual finish fields are absent from the live status/control API.
- Always charging enabled mains (477.622 W observed in the final test), survived
  an actual restart, and resumed mains with fresh telemetry. Automatic was then
  restored with the original 10–20% recovery limits. No BMS settings were written.
- Live desktop 1200 px/mobile 390 px checks passed with only the two supported modes,
  no target/time fields, all charts and no JS errors/overflow. Detailed deployment
  evidence is `tests/backtest/nonexpiring-modes-deployment.json`. Verification used
  the case-sensitive native Restart route after a harmless 404 from the initial
  lower-case test route. No 24-hour soak is claimed.

## Full-charge pause — 2026-11-06

- Automatic now pauses mains after 100% SOC or the captured Daly cell-overvoltage
  cutoff above 95%, until fresh valid SOC reaches 95%. Cheap prices cannot clear
  the latch. Always charging, low-battery recovery, phone handoff and existing
  fault stops retain their established priority. No BMS commands/settings added.
- Latch changes are persisted in NVS; restore loads the same latch. Native tests
  cover repeated economic requests, exact release boundary, invalid/stale SOC,
  early full-charge cutoff, fault priority, restored latch, mode changes,
  unrelated inhibition and reserve priority. All eight native test executables
  passed with ASan/UBSan; desktop/mobile dashboard fixtures passed.
- ESPHome 2026.10.1 compilation passed: application 1,603,328 of 1,835,008 bytes
  (231,680 bytes headroom), static RAM 162,214 bytes. No new history allocation.
- Deployed source `cb9258f` through macOS Terminal using the web `/update`
  endpoint; response `Update Successful!`. Direct session networking and the
  Python native OTA client could not reach the plug; Terminal curl succeeded.
  OTA image 1,603,696 bytes, SHA256
  `c87d2ca05b1ffe32fa55fe2f72cf807f2a53c29d1d06fd2304a256961692807f`.
- At uptime 26 seconds, Automatic resumed with fresh BMS/meter, connected BLE,
  valid prices, ready optimiser, healthy storage, SOC 95.4% and zero BMS alarms.
  Existing 10–20% reserve, capacity and optimiser settings survived. New status
  fields report `full_charge_resume:95` and `full_charge_paused:false`.
  Free heap 76,092 bytes; largest block 51,200 bytes; minimum heap 62,408 bytes.
- Full-to-95% live operation and latch persistence across a later restart remain
  unobserved; native tests cover controller behavior with a restored latch.
  Local deployment logs/status evidence are under `.esphome/full-pause-*`.

## Full-charge physical model correction — 2026-11-08

- Corrected both solver transitions and projected schedule to include the actual
  full/95% latch, with recovery independent and authoritative. Actual starting
  pause state is passed into the solver. Full is a modeled mains-disconnect event,
  including a partial interval; there is no sustained mains-only state at full.
- All nine native executables passed ASan/UBSan, including 400 independently
  sampled physical comparisons, zero-load/full and high-reserve cases.
  Independent Python checks passed 502 plant cases, 516 exhaustive short-horizon
  solver cases, price/load causality checks, and all 53,016 historical intervals
  across six corrected sensitivity traces. Desktop/mobile fixtures passed.
- Same 92-day saved price/load fixture and inventory settlement: old optimiser
  on paused plant saved 36.22685 SEK; corrected 101-node model saved 40.39763 SEK.
  Within-interval transfers fell from 258 to 32; total transfers from 834 to 824.
  1001-node saving 40.32618 SEK; float64 reproduced the base trajectory.
  These are synthetic-load modeled savings, not measured household savings.
- Replay of the captured 8 November 11:23 snapshot (95.7%, predicted 73.6 W,
  price 193.0928 ore/kWh) changed the first request from charge to wait.
- A first uniform-grid candidate exceeded the existing exhaustive-regret limit
  near full; the final grid concentrates resolution above 95%, keeping the
  predeclared tolerances and same 101-node budget. Final worst first-action
  regret was 0.03249 SEK in the 516 finite short-horizon test cases.
- Pinned ESPHome build passed; static RAM 162,214 bytes. Solver/projection
  temporary memory is now 29,294 bytes combined. Deployment checks follow.

- Deployed source `56b5e25` with Terminal curl `/update`, response
  `Update Successful!`. `compact-mpc-v3` is confirmed live. At uptime 51 seconds,
  fresh BMS/meter, zero alarms, connected BLE, ready optimiser and healthy NVS;
  Automatic chose waiting/mains off at SOC 97.7%. Existing settings were checked
  unchanged. Solve 553 ms, free heap 74,920 bytes, recovered largest block
  45,056 bytes; minimum heap 43,504 bytes and minimum block 15,872 bytes during
  startup/temporary allocations. No allocation failure observed in this sample.
- Live headless Chrome still fails with ERR_ADDRESS_UNREACHABLE, even launched
  through Terminal; live verification used Terminal curl. Desktop/mobile fixture
  tests passed. No complete post-deployment full-to-95% cycle or long soak is
  claimed. Image hash and before/after evidence are committed in
  `tests/backtest/full-pause-model-deployment.json`.

## Invoice-based tariff update — 2026-11-11

- Supplied Ellevio invoice unit rates are already VAT-inclusive: transmission
  26.00 + energy tax 45.00 = 71.00 öre/kWh. Independently checked equivalent
  pre-VAT calculation `(20.80 + 36.00) × 1.25`. Existing spot conversion already
  includes VAT. Fixed 740 kr/month is excluded from marginal charging decisions.
- Applied only `marginal_adder=71` through the settings API; successful response
  and live readback confirm 71.00, healthy NVS and ready optimiser. Operating
  mode, capacity, charger, efficiencies, reserve thresholds and stored-energy
  reference were checked unchanged. No firmware build, upload or restart needed.
- Source inspection confirms the same add-on feeds both solver and savings
  sample pricing; no second VAT multiplication is applied to this setting.
  Raw before/after status and savings remain locally under
  `.esphome/invoice-tariff-*`. Rate derivation is documented in TARIFF_MODEL.md.
