# SPV timing diagnostics

[中文](epd-spv-investigation.md)

## Scope

SPV diagnostics separate panel scanning, pixel preparation and scheduling delays. A target cadence is not a measured refresh rate; optical behavior and hardware timing require separate checks.

## Observation

- Refresh entry and waveform selection are in [epd_refresh.cc](../main/ui/common/drawing/adapter/epdiy/epd_refresh.cc); LCD scanning and feeders are in [render_lcd.c](../main/boards/common/display/epdiy/output_lcd/render_lcd.c).
- Distinguish ordinary difference, complete waveforms, black/white reveal and gray reveal. Do not directly compare different modes or phase counts.
- Separate scanning from wakeup, feeder completion, LUT preparation, reset, prefill, startup locking and cadence waits. Per-row samples, summed dual-core work and wall-clock duration are different measurements.
- Hold image, entry point, profile, orientation, temperature, CPU frequency and workload constant. Separate startup from steady operation.
- Measure XCL, CKV, XLE and SPV together and compare data windows, gate timing and tail clocks against the matching panel specification.

## Concurrency and timeouts

Queue waits use elapsed time rather than polling counts. Both buffer-acquisition paths share timeout handling, starting only when the queue is full. On error or cancellation, stop scanning and wait for feeder acknowledgements before reusing live queues, LUTs or other resources.

Publish both feeder notifications within a short local scheduler-suspension interval, with interrupts enabled and no scan wait inside it. This does not guarantee elimination of all cross-core wake delays.

Overlapping preparation with scanning requires explicit storage and ownership for LUTs, queues and counters. Do not overwrite active data, change established waveform phase counts or treat longer timeouts as a performance improvement.

## Limits

Long gaps may involve scheduling, critical sections or cache/Flash coordination; no single blocking module has been established. Home entry and ordinary refresh have different scheduling paths, so a single-entry optimization does not guarantee timing elsewhere. Identify the blocking stage before changing priorities, queues or cadence.

Software equivalence and timing measurements do not replace optical, power or long-term concurrency validation.
