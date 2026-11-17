# SPV 时序诊断

[English](#english)

## 适用范围

SPV 诊断用于区分面板扫描、像素数据准备和任务调度造成的刷新间隔。目标节拍不代表实际刷新率；显示效果和硬件时序需要分别验证。

## 调用链与观测

- 刷新入口及波形选择位于 [epd_refresh.cc](../main/display/epd_refresh.cc)，LCD 扫描与供数位于 [render_lcd.c](../main/boards/metalio-e-ink4-plus/display/output_lcd/render_lcd.c)。
- 区分普通差分、完整波形、黑白推白与灰阶推白，不将不同模式或相数的结果直接比较。
- 单相间隔包含扫描与准备。准备阶段需区分唤醒、供数收尾、LUT、复位、预填、启动锁及节拍等待；每行样本、双核累计工作量与墙钟耗时含义不同。
- 比较时固定画面、入口、刷新模式、方向、温度、CPU 频率及并发负载，分别观察启动与稳定运行。
- 需要确认真实面板时序时，同时测量 XCL、CKV、XLE 和 SPV，按对应面板规格核对数据窗口、门控和尾部时钟。

## 并发与超时

供数队列的等待超时以实际时间为准，不能用空轮询次数推算。两条缓冲获取路径共用超时处理；仅在队列满时开始计时。错误或取消后，须停止扫描并等待供数线程确认，才能复用队列、LUT 和其他活动资源。

发布两条供数通知时，在本核暂停调度的短区间内完成；不关闭中断，也不在此区间等待扫描。该处理不保证消除所有跨核唤醒延迟。

重叠下一相准备与当前扫描时，必须明确 LUT、队列和计数器的存储及所有权，不覆盖仍被使用的数据。保持既定波形相数，不能仅以减少阶段或延长超时获得表面上的提速。

## 当前限制

长间隔可能来自调度、临界区或 cache/Flash 协调；尚不能归因于某一个模块。普通刷新与首页进入路径的调度不同，单入口优化不代表全部场景的时序保证。调整线程优先级、队列或目标频率前，应先确定实际阻塞阶段。

软件输出等价与时序测量不能替代光学效果、功耗或长期并发稳定性验证。

## English

SPV diagnostics separate panel scanning, pixel preparation and scheduling delays. A target cadence is not a measured refresh rate; optical behavior and hardware timing require separate checks.

### Observation

- Refresh entry and waveform selection are in [epd_refresh.cc](../main/display/epd_refresh.cc); LCD scanning and feeders are in [render_lcd.c](../main/boards/metalio-e-ink4-plus/display/output_lcd/render_lcd.c).
- Distinguish ordinary difference, complete waveforms, black/white reveal and gray reveal. Do not directly compare different modes or phase counts.
- Separate scanning from wakeup, feeder completion, LUT preparation, reset, prefill, startup locking and cadence waits. Per-row samples, summed dual-core work and wall-clock duration are different measurements.
- Hold image, entry point, profile, orientation, temperature, CPU frequency and workload constant. Separate startup from steady operation.
- Measure XCL, CKV, XLE and SPV together and compare data windows, gate timing and tail clocks against the matching panel specification.

### Concurrency and timeouts

Queue waits use elapsed time rather than polling counts. Both buffer-acquisition paths share timeout handling, starting only when the queue is full. On error or cancellation, stop scanning and wait for feeder acknowledgements before reusing live queues, LUTs or other resources.

Publish both feeder notifications within a short local scheduler-suspension interval, with interrupts enabled and no scan wait inside it. This does not guarantee elimination of all cross-core wake delays.

Overlapping preparation with scanning requires explicit storage and ownership for LUTs, queues and counters. Do not overwrite active data, change established waveform phase counts or treat longer timeouts as a performance improvement.

### Limits

Long gaps may involve scheduling, critical sections or cache/Flash coordination; no single blocking module has been established. Home entry and ordinary refresh have different scheduling paths, so a single-entry optimization does not guarantee timing elsewhere. Identify the blocking stage before changing priorities, queues or cadence.

Software equivalence and timing measurements do not replace optical, power or long-term concurrency validation.
