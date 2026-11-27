# Network tests

**English** | [中文](README_zn.md)

Verify HTTP/HTTPS, WebSocket/WSS, TCP/TLS, fixed-peer UDP, DNS and ICMP Ping through the public ELF network API. Use Service Lab’s Services → Network tests on the computer; no separate echo script is required. The app supports 397 and 470 with a content area of at least 600×1000 and a host network API supporting configurable buffers, idle-disconnect timing, network preparation, DNS and Ping. Update firmware and test ELF together. On entry, the app calls `network_prepare` to attempt networking without sending business requests. If preparation fails, check Wi-Fi settings and retry with Prepare services.

## Use

1. Connect the computer and device to the same trusted LAN. Enable network tests in Service Lab and select Echo.
2. Enter the displayed host and HTTP port in the device app. Select Prepare services. The app reads protocol ports and the local test CA, which is scoped to this app’s HTTPS, WSS and TLS connections; system trust remains unchanged.
3. Run all tests or choose HTTP, WebSocket, TCP or UDP. Results show three cases per page with content verification, byte counts, elapsed time and numeric host/backend error codes on failure. Use Previous and Next to browse.
4. Repeat WS exchanges binary messages over one connection for exit, disconnect and standby checks. Stop ends the test. Standby or leaving revokes handles; prepare the service again after resuming.

Idle timer defaults to OFF: after the first accepted request, the foreground app keeps networking available between requests. Turning it ON applies system idle-disconnect timing when no network workers remain. Turn it OFF again to restore continuous holding without changing system settings or preventing automatic standby. This test ELF requires the host's `network_set_idle_timer` interface. It uses system horizontal-swipe recognition: swiping right performs default back navigation; swiping left has no default action and does not page results. Use Previous and Next for results. The system gesture switch and input fields' own gesture handling still apply.

Preparation fetches the CA over LAN HTTP without authenticating the computer. Use only a known local test service. The device clock must be correct. Re-enable services and reload the CA after changing computer network addresses.

DNS and Ping can run after entering a host without preparing Service Lab. DNS displays the first IPv4 address; IP input checks the literal-address path, so use a resolvable hostname to test name resolution. Ping sends four 32-byte ICMP Echo requests to the entered host, with a 250ms interval and one-second reply timeout. It shows address, replies/sent and average round-trip time; all four valid replies are required to pass. Choose a known ICMP responder such as a responsive gateway. A computer or firewall declining ICMP can fail this check while other protocols still work. The app changes no firewall settings.

## Coverage

- HTTP: fixed GET data, binary POST echo, a 404 response, a 9000-byte chunked response, a delayed timeout and HTTPS.
- WebSocket: text, 1024-byte binary data, an empty message and WSS; verify message type and content.
- TCP: a 3072-byte echo and TLS; compare cumulative bytes without assuming a receive chunk is a complete message.
- UDP: a 128-byte datagram echo; timeout means no response, without retries or delivery guarantees.
- TLS certificate rejection: connecting without the local CA must fail. Interpret this alongside the preceding successful TLS check.
- HTTP cancellation: revoke a delayed request and check that the old handle is inaccessible. Observe actual background cleanup using Service Lab’s active connection count and device logs.
- DNS: obtain the first IPv4 address through `network_resolve` and read managed diagnostics.
- Ping: validate four replies and statistics through `network_ping`; the operating system answers ICMP rather than Service Lab.

Automatic checks expect Echo mode. Delay, no-response and close scenarios exercise individual or continuous tests; expected failures under those scenarios are not a successful full test run. MQTT is excluded.

Exit, standby, wake, repeated entry, network loss, power use and RAM low-water marks require hardware verification. Server writes and app reads do not prove physical display completion.

## Source and build

`network_test.c` owns the UI, foreground state machine, deterministic data checks and connection cleanup. `icon.pbm` is the original icon. The app creates no threads and accesses neither network drivers nor the filesystem directly. It requests an 8 KiB test receive buffer, sets send capacity to each test payload length, and chooses a 1 MiB HTTP response limit. Preparation uses a separate 4 KiB receive buffer. These are test-app choices rather than fixed host networking limits.

From the repository root, activate the matching ESP toolchains and Python. Set `$compiler470` and `$compiler397` to their compiler paths and run separately:

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler470 -AppDirectory ./tools/elf_app/network_test -Target esp32s31 -OutputDirectory ./tools/elf_app/network_test/build/470
./tools/elf_app/build_app.ps1 -Compiler $compiler397 -AppDirectory ./tools/elf_app/network_test -Target esp32s3 -OutputDirectory ./tools/elf_app/network_test/build/397
```

Each directory receives `network_test.elf`. After every build, generate the adjacent `manifest.json` using the [local manifest instructions](../README.md#local-installation-manifest-example), app ID `network_test`, and recalculated size and SHA256. Target is `esp32s3` for 397 and `esp32s31` for 470; ABI is 1. Build directories are not a designated delivery directory. The matching ELF can also be uploaded and published through Service Lab’s existing app store.

See [managed networking](../README.md#managed-networking-xz_app_cap_network) for shared interfaces, limits and lifecycle rules.
