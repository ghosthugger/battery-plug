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
