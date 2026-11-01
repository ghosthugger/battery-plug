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
