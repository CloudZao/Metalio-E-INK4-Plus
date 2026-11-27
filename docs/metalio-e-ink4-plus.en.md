# Metalio E-Ink4 Plus: low power and Bluetooth audio

[中文](metalio-e-ink4-plus.md)

## Low power

Standby entry, restoration, timer wakes, failure protection and S31 SDK compatibility are maintained in the [power policy documentation](../main/sysmgmt/power/policy/README.en.md). Domestic and International variants share the hardware sequence; board configuration owns pins and power parameters.

## Audio initialization

A background worker resets the audio supply, waits for stabilization, sends mode configuration and waits for an explicit mode acknowledgment. These operations do not block the main event loop. A successful UART write or unrelated reply does not establish readiness.

The worker retries up to three times after the first failure, with one-second gaps. Exhausted retries remain failed for that power epoch. Restoring power invalidates prior readiness. AI, recording and aging operations wait in background before acquiring I2S resources. Leaving audio releases I2S channels and DMA. Replacing the Bluetooth page data callback does not remove the initialization observer.

Failures record the initialization stage and attempt count.
