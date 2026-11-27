# Sender firmware (node 2)

This describes the firmware after the serial command layer was added (run plan P0.1, 2026-11-27). The firmware is an ESP32 + CC1101 node that idles until the orchestrator (or a person at a serial monitor) sends it commands. It switches the protocol configuration at run time and sends blocks of synthetic packets to node 1 through `afec::Node::send()`. Everything about the protocol (framing, ECC, level adaptation, logging) lives in the `cc1101-afec` library. See its `docs/`. The library is unchanged by the command layer.

## Files

| File | Role |
| --- | --- |
| `platformio.ini` | `env:esp32dev` pulls the library from GitHub (`embedd-data-intake/cc1101-afec`, unpinned, so it follows the default branch). `env:dev` builds against `../cc1101-afec` through a symlink. Both build with `-D AFEC_LOG_LEVEL=4 -D AFEC_LOG_FORMAT=1 -D AFEC_LOG_SINK=1` (serial). `env:dev-mqtt` is `dev` with `AFEC_LOG_SINK=2`. |
| `src/config.h` | Pins, node IDs, boot radio settings, default TX interval, `SENSOR_TX`, sensor I²C addresses |
| `src/main.cpp` | `setup()`, the command handlers, the `run` state machine, `bench`, and the legacy sensor path |
| `src/afec_cmd.h` | Command layer shared with the receiver: line reader, `key=value` parser, `cfg` parsing, JSON event helper, synthetic payload + CRC32. **Identical copy in the receiver.** |
| `src/afec_platform_arduino.h` | `afec::Platform` glue (`millis`, `random`, `yield`). Identical copy in the receiver. |
| `src/afec_log_sink.h` | Log sink: serial, MQTT or off, chosen with `AFEC_LOG_SINK` (see "Log sink" below). **Identical copy in the receiver.** |
| `src/secrets.example.h` | Template for the gitignored `src/secrets.h` (WiFi + broker), only needed by MQTT builds |

## Configuration

`config.h` only sets the **boot** state:

| Constant | Value | Notes |
| --- | --- | --- |
| `PIN_SCK` / `PIN_MISO` / `PIN_MOSI` / `PIN_CSN` | 19 / 18 / 23 / 27 | SPI at 4 MHz |
| `PIN_GDO0` / `PIN_GDO2` | 26 / 16 | GDO0 is the RX/TX-done interrupt. GDO2 is unused. |
| `NODE_ID` / `DEST_NODE_ID` | 2 / 1 | |
| `RF_FREQ_MHZ` / `RF_SYNC_WORD` / `RF_TX_DBM` | 433.92 / `0x6B1D` / 0 | Must match the receiver. `RF_TX_DBM` is only the boot value; `cfg tx=` changes it. |
| `USE_IRQ` | true | Falls back to polling if `enableInterrupt()` fails |
| `TX_INTERVAL_MS` | 2000 | Default for `run interval=`, and the legacy sensor period |
| `SENSOR_TX` | false | Legacy mode: init the sensors and send their readings every `TX_INTERVAL_MS` while no `run` is active. Off for the campaign. |
| `BMP_390_ADDR` / `AHT10_ADDR` / `ENS160_ADDR` | 0x77 / 0x38 / 0x52 | Only used with `SENSOR_TX`. Only the AHT works; the BMP390 and ENS160 are out of scope and read 0. |

At boot the protocol configuration is the library default: **C5 (AFEC)**, `initialLevel` 2, 3 retries, 400 ms ACK timeout, `downgradeAfter` 2, `nackStep` 2, 10 % duty, 36 s burst. Everything except the radio frequency, sync word, data rate and duty settings can be changed with `cfg`.

## Serial commands

One command per line at 115200 baud, `<name> key=value ...`, separated by spaces. Every reply is a JSON line in the library's log format (`{"t":…,"node":2,"lv":…,"ev":…}`), so it lands in the same log as the protocol events. A rejected command logs `cmd_err` (`cmd`, `err`, `detail`) and changes nothing.

| Command | Effect |
| --- | --- |
| `hello` | Logs `hello` (`role`, `peer`, `build`) and `cfg_ok`. Use it to identify the port and check the current configuration. Also logged once at boot. |
| `cfg mode=<1..5> init=<0..5> retries=<0..15> ackto=<50..5000> tx=<dBm> dg=<1..15> nackstep=<1..5> rssi_min=<dBm> rssi_max=<dBm> rssi_up=<a,b,c,d,e> hys=<0..15>` | All keys are optional; an absent key keeps its current value, so the C4 policy (`rssi_*`, `hys`) can be set once and stays frozen. `mode` is the configuration number C1..C5. **`mode=3` (C3) is rejected**: it needs the missing fixed-length FEC driver patch. `tx` is −30..10 dBm; the driver rounds down to −30, −20, −15, −10, 0, 5, 7 or 10. `dg` is `downgradeAfter`. On success: `radio.begin()` + `enableInterrupt()` + `node.begin()` (logs `begin`), `resetLink(1)` (logs `link_reset`), then `cfg_ok` with the full effective configuration. |
| `mark run=<id> block=<n> cfg=<Cx>` | Logs `mark`. `block` is required and becomes the block id written into the synthetic payloads. `run` and `cfg` are only echoed. |
| `reset [peer=<id>]` | `resetLink(peer)`, default peer 1. Logs `link_reset`. |
| `run n=<count> len=<12..50> interval=<ms> [block=<n>]` | Starts a block: `n` synthetic packets (default 100) of `len` bytes (default 32), one every `interval` ms (default `TX_INTERVAL_MS`), measured start to start. The first goes out at once. Logs `run_start`, one `app_tx` per packet and `block_end` at the end. Non-blocking: commands are still read between packets. |
| `stop` | Aborts the active run; `block_end` with `aborted=1`. |
| `stats` | Logs the cumulative `afec::Stats` counters as `stats`. They count since boot and are not reset by `cfg`. |
| `bench [reps=<n>] [len=<payload B>]` | Cycle-count benchmark for RQ4, no radio (see below). Blocks the loop for a few seconds. |

While a run is active, only `hello`, `stats`, `mark` and `stop` are accepted; anything else returns `cmd_err` `busy`.

**A typical block, as the orchestrator sends it** (the receiver gets the same `cfg` and `mark` first, because its mode must match):

```
cfg mode=5 init=2
mark run=2026-11-30-Q block=17 cfg=C5
run n=110 len=32 interval=2000
```

Commands are read in `loop()` between sends. `node.send()` blocks for up to (1 + retries) × (airtime + ACK timeout), about 2 s, so a command can take that long to be acted on. The ESP32 serial RX buffer (256 B) holds it in the meantime.

### Notes on re-initialisation

- `cfg` re-runs `radio.begin()`, so the CC1101 is reset and recalibrated at every configuration switch, in every configuration alike.
- `node.begin()` refills the 36 s duty-cycle burst budget, so every block starts with a full budget (run plan, decision 4).
- `node.begin()` does not clear the peer table, the duplicate filter or the counters, and the 8-bit frame `seq` keeps counting. The `resetLink` that `cfg` performs puts the peer back to `initialLevel`.

## Synthetic payload

`len` bytes (12..50; 32 in the campaign so that every level, including RS t=8, fits):

| Bytes | Content |
| --- | --- |
| 0–3 | `aseq`: u32 LE, application packet counter, per boot, never reset |
| 4–5 | `block`: u16 LE, from the last `mark` (or `run block=`) |
| 6–7 | `idx`: u16 LE, index within the run, 0-based. The warm-up packets are `idx < K_w`; the S step points are at known `idx`. |
| 8 … len−5 | Filler: xorshift32 stream seeded from `aseq` |
| len−4 … len−1 | CRC32 (IEEE, reflected, as zlib) over bytes 0 … len−5 |

The receiver checks the CRC32 and logs `app_rx ok=0|1`, which gives RER and "delivered = hash matches".

## Firmware events

These come from the firmware, not the library, but use the same JSON line format. `t` and `node` as in the library schema.

| ev | lv | Fields | When |
| --- | --- | --- | --- |
| `boot` | I | `reason` (`esp_reset_reason()`: 1 power-on, 3 software, 4 panic, 7/8 watchdog, …) | Once per boot, before `hello`. Lets `campaign.py` see reboots without the serial `CC1101 ready` line. |
| `hello` | I | `role` (`sender`), `peer`, `build`, `sink` | Boot and `hello` |
| `cfg_ok` | I | `cfg` (`C1`..`C5`), `mode` (0-based, as in `begin`), `init_lvl`, `retries`, `ack_to_ms`, `tx_dbm`, `dg`, `nack_step`, `rssi_min`, `rssi_max`, `rssi_up` (string `"a,b,c,d,e"`), `hys` | Boot, `hello`, successful `cfg` |
| `cmd_err` | W | `cmd`, `err`, `detail` | Rejected command; nothing was changed |
| `cmd_rx` | I | `id`, `cmd`, `src` (`mqtt`) | A command taken from `afec/cmd/<tx\|rx>`, logged just before it runs (`id` 0 if none was given) |
| `cmd_dup` | W | `id`, `cmd` | An MQTT command whose `id` was among the last 8: a QoS 1 redelivery, not run again |
| `radio_err` | E | `err` | `radio.begin()` failed during `cfg`. The node is not usable until the next successful `cfg` or a reboot. |
| `irq_fallback` | W | — | `enableInterrupt()` failed after `cfg`; polling is used |
| `mark` | I | `run`, `block`, `cfg` | `mark` |
| `run_start` | I | `block`, `n`, `len`, `interval_ms`, `aseq0` | `run` accepted |
| `app_tx` | I | `aseq`, `block`, `idx`, `len`, `res` (`OK`, `NO_ACK`, `DUTY_LIMIT`, `TOO_LONG`, `RADIO_ERR`), `lvl_next` (the peer level after this send, 7 = `LEVEL_NONE` in C1/C2), `dur_ms` (time inside `send()`) | After each `send()`. The library's `send`/`tx`/`send_ok` lines for this packet come just before it. |
| `block_end` | I | `block`, `n`, `sent`, `ok`, `no_ack`, `duty_limit`, `other`, `aborted`, `dur_ms` | Run finished or stopped |
| `stats` | I | the `afec::Stats` fields in snake_case | `stats` |
| `bench_start` | I | `cpu_mhz`, `reps`, `plen`, `inner_len` | `bench` |
| `bench` | I | `op`, `lvl`, `s`, `n`, `ok`, `min`, `mean`, `max` (CPU cycles) | One per measured case |
| `bench_end` | I | — | `bench` done |

## `bench` (RQ4)

Runs on one node with nothing else going on. Inner length = payload + 7 (header + CRC-16), 39 B for the default 32 B payload. Per measurement it reports `min`, `mean` and `max` CPU cycles (`ESP.getCycleCount()`) over `reps` repetitions (default 200); interrupts are not disabled, so `min` is the robust figure. Divide by `cpu_mhz` for µs.

| `op` | `lvl` | `s` | `ok` counts |
| --- | --- | --- | --- |
| `enc` | 0–5 | 0 | — (= `n`) |
| `dec` | 0–5 | 0 … t+1 errors injected in the data part (distinct bits for Hamming/BCH, distinct bytes for RS). s = t+1 measures the failure path. | decodes that succeeded, reported `corrected == s` and restored the data |
| `frame_enc` / `frame_dec` | 0–5, 7 | 0 | `frame_dec`: frames that decoded OK |
| `crc16` | 7 | 0 | — |
| `lhs_score` | 7 | 0 | — (random inputs) |

Flash and RAM figures come from the `pio run` output.

## Runtime behaviour

1. **`setup()`**: serial at 115200, `radio.begin()` (halts with `CC1101 not found` if the chip doesn't answer), `enableInterrupt()`, a plain-text `CC1101 ready` line, sensor init only with `SENSOR_TX`, `node.begin()`, then `hello` and `cfg_ok`.
2. **`loop()`**: `node.poll()`, then one complete command line if there is one, then either the next packet of an active run (when its interval has elapsed) or, with `SENSOR_TX`, the legacy sensor send.
3. **Effective period** = max(`interval`, `send()` time). Under heavy loss, `send()` can take about 2 s and packets go back to back.

## Serial output

This section applies to the serial sink (`AFEC_LOG_SINK=1`, the default).


- **Serial TX buffer: 8 KB** (`Serial.setTxBufferSize(8192)` before `begin()`). With the default buffer every log line blocked ~15 ms on the UART, which inflated RTT unevenly across configurations (2026-11-27 hardware test: C5 109 ms vs C2 53 ms; with the buffer 37 vs 34 ms, the gap being the ACK airtime). DBG logging averages about 1 KB/s, well under the UART's 11.5 KB/s.
- JSON Lines from the library (all events at DBG) and from the firmware (table above). Filter on lines starting with `{`. The library schema is in `cc1101-afec/docs/afec_log_schema.md`.
- Plain-text lines from the firmware: the boot `CC1101 ready` / `CC1101 not found` / IRQ fallback messages.
- `debugPrint` (library lines) formats into a **192 B buffer**. A longer line is cut to 191 B **with no newline**, which would merge it with the next line. The longest observed line is 177 chars (`tx`). Firmware events use a separate 256 B field buffer; the longest, `cfg_ok`, is about 230 chars.

## Log sink (serial / MQTT / off)

Added 2026-11-27 (PLAN M6). `src/afec_log_sink.h` decides where every log line goes, both library lines (`debugPrint`) and firmware events (`cmd::ev`). It is chosen at build time with `-D AFEC_LOG_SINK=<bitmask>`:

| Value | Sink | Notes |
| --- | --- | --- |
| `0` | off | `node.printDebug` stays null, so the library doesn't even format its lines; `cmd::ev` returns at once. For runs where logging must not cost anything. |
| `1` | serial | Default (`esp32dev`, `dev`). Same output as before; what `campaign.py` reads today. No WiFi code is linked. |
| `2` | mqtt | `env:dev-mqtt`. One MQTT message per line, no trailing newline, QoS 1, topic **`afec/<run_id>/<tx|rx>/log`** (`tx` = sender, `rx` = receiver). |
| `3` | both | Serial and MQTT; for checking one against the other. |

**Commands always come in over serial**, whatever the sink. With `AFEC_LOG_SINK=2` the replies (`cfg_ok`, `mark`, …) only appear on MQTT; `campaign.py --mqtt` reads them there.

**Commands over MQTT (untethered nodes).** MQTT builds also take commands from **`afec/cmd/<tx|rx>`** (QoS 1, subscribed on every connect), unless built with `-D AFEC_MQTT_COMMANDS=0`. Serial keeps working alongside.
- The esp-mqtt task only copies the message into an 8-deep queue (max 191 chars; longer, fragmented or overflowing messages are counted as `cmd_lost`). `loop()` takes one per pass with `cmd::pollRemote()` and runs it through the same `handle()` as a serial line, so parsing and the protocol are untouched.
- A command may carry **`id=<n>`** (decimal, 32-bit). It is stripped before parsing and logged in `cmd_rx`. If the same id was among the last 8, the command is a QoS 1 redelivery: `cmd_dup` is logged and it is **not** run again (a second `cfg` would re-initialise the radio). `campaign.py --mqtt-commands` always sends an increasing id. A malformed `id` stays in the line and the command is rejected as an unknown key.
- Latency is WiFi latency (tens of ms): fine between blocks, but the interferer's S-phase steps should stay on serial.
- If WiFi or the broker drops there is no control channel; serial still works when a cable is attached.

**How MQTT publishing works.** The protocol runs in `loop()` on core 1. `logsink::write()` copies the line into a 32 KB FreeRTOS ring buffer (`AFEC_MQTT_RING_BYTES`) and returns; it never blocks. A publisher task pinned to **core 0**, together with the WiFi driver and lwIP (pinned there by the Arduino core's sdkconfig), takes lines out and calls `esp_mqtt_client_publish` (ESP-IDF's built-in esp-mqtt, no extra library). WiFi stays on during the runs. The report's buffered-upload design (§2.3.1) does not fit, because one block at DBG level is 127–230 KB, more than the RAM left next to the WiFi stack. Caveat: esp-mqtt's own task (keepalive, PUBACKs) is not pinned in the prebuilt sdkconfig, so it can occasionally run on core 1.

- **Timestamps** (`t`) are `millis()` when the line was logged, so publish delay doesn't move them.
- **`<run_id>`** is the `run=` of the last `mark` (`/`, `+` and `#` become `_`), and `boot` until the first `mark`. The `mark` line itself goes to the new run's topic.
- **Lost lines are counted, not hidden.** If the ring is full the line is dropped and counted. Once the ring is half free again, the publisher sends `{"ev":"log_drop","dropped":<since last report>,"total":<n>}` (level W). While the broker is unreachable the publisher holds its current line and retries every 100 ms, so new lines queue and then drop.
- **`stats`** additionally logs `log_stats` (`sink`, `enq`, `dropped`, `pub`, `retries`, `conns`, `connected`, `cmd_rx`, `cmd_lost`). Check `pub` against the number of messages the broker side received from that node (§2.3.2: uploaded records are checked against node counters).
- **`hello`** carries `"sink":"serial|mqtt|serial+mqtt|off"`.
- **Credentials:** `src/secrets.h` is gitignored. Copy `src/secrets.example.h` and set `AFEC_WIFI_SSID`, `AFEC_WIFI_PASS` and `AFEC_MQTT_URI` (e.g. `mqtt://192.168.1.10:1883`), plus optionally `AFEC_MQTT_USER`/`AFEC_MQTT_PASS`. An MQTT build without `secrets.h` stops with an `#error` that says so. Client id: `afec-node<id>`.
- **Footprint:** an MQTT build uses about 600 KB more flash and 24 KB more static RAM, plus the 32 KB ring on the heap. Take the RQ4 flash/RAM figures from a serial (or off) build.

## Known issues (recorded, not fixed)

- **The BMP390 and ENS160 don't work, and are out of scope.** Only relevant with `SENSOR_TX`.
- **Legacy sensor path:** `msg[32]` with `len` from `snprintf` is a latent over-read that can't trigger with p = m = 0; `ens160.wait()` blocks the loop; sensor init failures are silent. None of this runs in the campaign (`SENSOR_TX = false`).

## Build, flash, capture

```sh
pio run -e dev                                     # build against ../cc1101-afec
pio run -e dev -t upload --upload-port /dev/cu.usbserial-5   # ask before flashing
pio device monitor -b 115200 -p /dev/cu.usbserial-5          # type commands here
```

`pio device monitor` sends each line on Enter, so the commands can be tried by hand. On the current bench Mac, node 2 is on `/dev/cu.usbserial-5`. Both boards are CP2102 with identical serial numbers, so identify them with `hello` (`role`, `node`).

Note: `env:esp32dev` pulls the library from GitHub. The command layer only uses the existing library API, so both environments work.

If PlatformIO fails to download packages behind the TLS-intercepting proxy, set `REQUESTS_CA_BUNDLE` and `SSL_CERT_FILE` to `~/.config/certs/macos-bundle.pem`.
