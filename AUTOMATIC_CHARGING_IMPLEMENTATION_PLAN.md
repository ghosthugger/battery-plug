# Automatic battery charging with local monitoring

Agreed implementation plan, 2026-11-01.

## Current state

The Athom Plug V3 ESP32-C3 runs custom ESPHome at 192.168.1.254, supports
mDNS and OTA updates, and exposes manual switching, power measurements and
persistent target/finish-time settings. Manual charging was tested at 400 W.
The defaults are 90% and 07:00 Europe/Stockholm; they do not yet control charging.

The computer read checksum-verified status from Daly DL-401710013BB4 (Bluetooth
40:17:10:01:3B:B4). Elklocka's Gridio SE3 API supplies 15-minute prices. Its
Swedish display converts EUR/MWh to VAT-inclusive öre/kWh using
`raw * SEK exchange rate * 1.25 / 10`. The existing application occupies about
1.63 MB of a 1.84 MB OTA slot; the NVS partition is 448 KB. Runtime memory and
persistent storage must be measured before accepting added features.

The finished system operates entirely on the plug. A separate logging or
monitoring server is explicitly outside this plan.

## Battery and price ingestion

- Implement a small read-only Daly component using the verified D2 request,
  FFF0 service, FFF1 notifications, FFF2 requests and 62 status registers.
- Poll every ten seconds; assemble fragments and check framing/checksum.
  Reconnect with bounded backoff. Read SOC, voltage, signed current, temperatures,
  cell-voltage range and charge permission/faults. Never change BMS settings.
- Remove the generic Bluetooth proxy/provisioning and Home Assistant-dependent
  scanning. The dedicated BMS connection must operate independently.
- Fetch SE3 today/tomorrow prices at startup and every 15 minutes with bounded
  retries; fetch SEK conversion daily. Use verified HTTPS certificates.
- Validate price timestamps, values and coverage. Cache at most 200 intervals;
  use still-valid cached intervals during outages. Unknown/unpublished periods
  remain unknown. Display the published planning horizon.

## Charging controller

Keep target and deadline settings. Add persistent modes Off, Automatic and
Charge now, price ceiling 100 öre/kWh, editable usable capacity 4 kWh,
charger input 400 W and initial efficiency 85%.

- Automatic selects the cheapest eligible intervals to approach the target by
  the next daily deadline. The price ceiling is strict, including if the target
  cannot be reached. Equal-price intervals prefer the earlier interval.
- Charge now overrides price only; target, deadline and fault stops remain.
  Completion returns manual mode to Off. Off suppresses all starts.
- Persist Automatic/Off; never resume manual charging after a reboot. Boot with
  the relay off and wait for valid clock/BMS/meter inputs before any start.
- One controller owns the relay; the existing switch becomes controller-backed.
  Short physical press stops an active charge or requests Charge now.
- Replan every minute and after settings, price and material SOC changes. Use
  capacity/percentage deficit and recent BMS load measurements, with zero initial
  load estimate. Refine expected net charging contribution from measurements.
- Support partial current/final intervals. Stop at target and re-arm top-ups
  only after SOC falls two percentage points below target.
- Account for simultaneous discharge loads. Display estimated completion,
  achievable SOC, target-unlikely and provisional/unpublished-price conditions.
- Finish the current cycle at its deadline and evaluate the next daily cycle;
  never execute an expired schedule. Use Stockholm civil time across DST.
- Stop for BMS older than 60 seconds, invalid SOC, charge-prohibiting BMS faults,
  power-meter older than 30 seconds or invalid clock. Automatic also requires
  a valid current price. Keep the charger/BMS's hardware protections intact.
- Use a two-minute switching interval except immediate Off, fault, target and
  deadline stops. Reconstruct schedules from live inputs after restart.

## Addition: low-battery mains recovery (2026-11-01)

Add a persistent dashboard setting `reserve_threshold`, default 5%, valid from
0 to 95% (0 disables it). In Automatic or Charge now, fresh SOC at or below this
threshold immediately enables mains and charging, overriding the price ceiling,
missing prices, charge target, daily finish time and switching cooldown. It also
works before clock synchronization, since this decision needs no schedule.

Keep recovery active until SOC reaches the configurable `reserve_recovery`
percentage (default 10%, strictly above the start threshold and at most 100%),
then resume the existing controller. Persist both limits and the recovery latch so a reboot at, for example, 7% resumes recovery. Existing settings
must survive the upgrade without a storage-layout migration. Display the active
recovery reason, threshold and release percentage on the control page and API;
record changes through existing decision/relay events.

Explicit Off prevents recovery starts. Existing stale/invalid BMS and meter,
BMS charging prohibition, overcurrent and OTA stops remain authoritative. Recovery
may purchase expensive electricity and cannot prevent an outage if mains is lost,
charging is prohibited, telemetry fails, or the load exceeds available charging
power. Five percent is an editable starting value, not a guarantee of freezer
continuity or a statement of SOC accuracy.

Scope: keep all BMS access read-only. Do not include the experimental MOS control,
both-switch test, mains/battery day scheduling, or keeping mains on after the
normal target is reached. Preserve the earlier experiment separately from the
active firmware source. This addition changes only the low-battery override.

Validate threshold/release boundaries, hysteresis, price/deadline/clock overrides,
fault and manual-Off precedence, persistence across restart, disabled recovery,
settings validation and desktop/mobile editing. Test actual operation using a
temporary threshold above the observed SOC, then restore the saved settings.

## Dashboard and bounded history

Serve /charging alongside ESPHome diagnostics, embedding all HTML/CSS/JS and
using browser-rendered SVG charts without external dependencies.

- Display live SOC/target, battery voltage/current, charger watts, relay/mode,
  price/ceiling, deadline, scheduled intervals, completion estimate and explicit
  reasons for waiting, charging or faults. Expose the controller settings.
- Keep 288 five-minute aggregates for 24-hour SOC, power, relay duration,
  price/ceiling, schedule versus actual charging, grid energy and estimated cost.
- Keep a bounded event list for mode/relay changes, faults and missed deadlines.
- Attempt 180 daily summaries: energy, price-based cost, charging duration,
  SOC range and deadline success/miss counts. Chart days and monthly totals.
- Costs include energy price/VAT only, excluding supplier/grid charges. Unknown
  prices produce unknown-price energy, not fabricated zero costs or savings.
- Use bounded binary circular buffers and versioned checksum-protected NVS
  records. Persist completed buckets/summaries, mark gaps and never backfill
  invented data. An unfinished bucket can be lost on power failure.
- History/events/prices must use at most 24 KB RAM; persistent history must use
  at most 48 KB including overhead. Preserve partitions, Wi-Fi and OTA.
- Require 64 KB OTA headroom, 60 KB free heap and a 32 KB largest free block
  during simultaneous BLE/HTTPS/dashboard activity. Remove unused diagnostics
  first; omit monthly retention if it prevents meeting these limits, retaining
  24-hour history. Do not implement a server as a fallback.
- Provide bounded /charging/api/ endpoints for status, validated settings/modes,
  schedule, 24-hour history and daily summaries. Stream history rather than
  allocating a large response. Existing settings and the dashboard share state.

## Implementation and acceptance

1. Read BMS on the plug and compare to the app/computer. Test fragmentation,
   corruption, contention, disconnects and reconnection.
2. Match SE3 prices/units to Elklocka. Test negatives, unpublished tomorrow,
   malformed data, gaps and failed downloads.
3. Test the controller against simulated prices/battery inputs before connecting
   relay actions: ceilings, loads, infeasible targets, top-ups, overrides,
   midnight and DST transitions.
4. Verify dashboard controls and graphs, integration of energy/cost, ring
   rollover, restart recovery and gaps. Simulate months of daily summaries.
5. Install with Automatic disabled. Check telemetry and explanations, then a
   supervised charging session and fault/target stopping before unattended use.
6. Run a 24-hour soak with dashboard usage and connection interruptions. Record
   minimum heap/largest block, image size, reconnects and relay decisions.

Completion means independent BMS reads, SE3 fetches, charging control with these
rules, an explanatory dashboard and retention that meets measured limits.

## Addition: compact cost optimiser — 2026-11-05

This addition supersedes the original Automatic target/deadline/price-ceiling
scheduler. Manual Charge now retains the existing target and finish time. The
old ceiling remains in stored settings/API for compatibility but does not limit
Automatic. Automatic may charge beyond the manual target, up to physical full.
There is no invented ability to supply mains without charging the battery.

Use a native, fixed-memory dynamic programme with 101 energy levels, float32
values and two recovery-latch states. Recalculate every quarter hour and after
settings or published-price changes. Model battery supply versus mains supply
plus automatic full-rate charging; cap charging only when physically full.
Model charge/discharge efficiencies and battery-mode inverter idle draw.
Continue to apply read-only BMS, meter, overcurrent, OTA and manual-Off stops.
Keep the configured recovery start/stop and its persistent latch authoritative.
Automatic requires an enabled reserve threshold above 0%; disabling recovery
also suspends economic Automatic decisions. Manual controls remain available.

Use published SE3 prices plus up to 24 hours of flat forecast. Forecast price is
the median of completed hourly prices retained over the last seven days; stored
energy continuation value uses their low quartile divided by charge efficiency.
Before historical prices exist, use currently published prices as the initial
estimate. Forecast intervals are estimates, never substitutes for a missing
current price. A configurable gross marginal adder (default 70 öre/kWh) accounts
for variable tax/grid/supplier costs; it is an assumption to match to the bill.

Estimate appliance AC draw from read-only battery DC draw while mains is off,
subtracting idle draw and accounting for inverter efficiency. Use a bounded
seven-day hourly weighted mean; initial fallback 60 W. Do not assume the combined
plug meter can separate charging and appliance consumption on mains. Persist
bounded price/load history hourly and on configuration changes/restart. Initial
physical estimates: charge/discharge efficiency 90%, idle 10 W; all editable.

The solver workspace is allocated only during planning, is 7680 bytes on the
native float32 build, and retains no full policy table. Price/load history is
2704 bytes. Avoid overlapping a solve with the HTTPS fetch task. Account for a
partial current quarter-hour. Keep all application partitions and OTA support.

Dashboard: distinguish Automatic from manual-only target/finish settings. Show
forecast price, additional variable charges, estimated appliance draw, next
quarter-hour decision, solver time and workspace. Display published prices and
only the next committed mains decision; do not imply a fixed future schedule.
Existing charts retain spot+VAT cost accounting, explicitly excluding the adder.
Do not claim measured financial savings on the live page.

Offline acceptance: use the existing validated SE3 Aug–Oct 2026 trace and the
same 4 kWh/400 W fridge/freezer scenario. Include the actual 10–20% recovery latch,
finite observability, 101vs201/401/601/1001 energy levels, float32vsfloat64,
late price publication, heavier load and inverter-idle sensitivity. Publish grid
bill, common end-energy settlement, savings and source transfers. Verify energy
balances, fault/manual/recovery precedence, future-price causality and boundary
transitions independently. Confirm desktop/mobile UI and pinned ESPHome build.

Deployment: commit reviewed source and validation before uploading the OTA image.
Retain the pre-change image under `.esphome/pre-optimizer` for rollback. Verify the
installed algorithm identifier, existing settings/history, BMS freshness, price
fetch, decisions, restart behavior and runtime heap/latency. A 24-hour soak is
separate follow-up evidence, not a prerequisite falsely marked complete here.

## Addition: non-expiring operating modes — 2026-11-05

Supersedes all earlier Off/Charge now/manual-target/deadline controls. Expose only
Automatic and Always charging. Always charging keeps mains enabled at any SOC,
including full, without a time/price/target cutoff; the charger regulates charge.
Keep existing BMS/meter/overcurrent/OTA protections. No BMS writes are introduced.

Persist both modes across restart. Migrate legacy Off/Charge now to Automatic;
preserve existing capacity, tariff/model estimates, history and reserve settings.
Disallow a zero reserve threshold; legacy disabled/invalid reserves become5–10%.
Remove manual target/time entities from diagnostics and custom UI. Reject retired
mode/settings requests. Physical button toggles the two supported modes; diagnostics
Always charging switch OFF selects Automatic. Overcurrent is a separate persistent
protective latch, not an operating mode; explicit mode selection acknowledges it.

Verify full battery, missing clock/prices, elapsed days, temporary faults/resume,
mode migration, restart persistence, invalid legacy API requests and non-disableable
recovery. Update desktop/mobile fixtures; build, commit and deploy. Live tests may
briefly select Always charging, verify mains output, restart to check persistence,
then restore the prior supported mode. Record actual results without a soak claim.

## Addition: 30-day estimated savings — 2026-11-05

Add a persistent savings counter and signed daily graph for the last 30 Stockholm
calendar days, including today. Estimate appliance demand from plug AC power and
BMS DC power using configured charge/discharge efficiencies and inverter idle
power. Compare its direct-mains cost with metered grid cost at published spot
prices plus the configured variable charges. Add a fixed-value stored-energy
adjustment so initial battery energy is not free and energy left charged is not
written off. Show the cost terms and included/excluded observation hours. Keep
negative results and mark partial days. Start collection now; no invented backfill.

Use a separate bounded 972-byte savings record, independent of the disabled
180-day detailed history. Persist with checksums every five minutes and on
shutdown. Retain the 30 most recent local calendar days, correctly handling DST,
restarts, missing/unknown-price intervals and backward clock corrections. Only
fresh readings and stable relay intervals qualify; do not extrapolate outages.
Keep the BMS read-only and leave the controller/optimiser behavior unchanged.
No external logging or monitoring server is included.

Validate accounting, energy conversion losses, negative prices, inventory value,
missing data, restart, price/day boundaries, DST and expiry; check desktop/mobile
counter and signed bars, build, commit and deploy. Verify the installed UI, fresh
telemetry, accumulated savings observations and preserved mode/reserve/model
settings. A full 30-day hardware soak is not claimed.

Clarify the price chart title/legend: it shows the current decision, recalculated
every 15 minutes, rather than a complete future charging schedule.
