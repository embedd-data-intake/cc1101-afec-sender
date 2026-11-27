# Sender firmware (node 2)

This describes commit `80f9af9`. The firmware is an ESP32 + CC1101 node that reads three I²C sensors every `TX_INTERVAL_MS` and sends the readings to node 1 through `afec::Node::send()`. Everything about the protocol (framing, ECC, level adaptation, logging) lives in the `cc1101-afec` library. See its `docs/`.

## Files

| File | Role |
| --- | --- |
| `platformio.ini` | `env:esp32dev` pulls the library from GitHub (`embedd-data-intake/cc1101-afec`, unpinned, so it follows the default branch). `env:dev` builds against `../cc1101-afec` through a symlink. Both build with `-D AFEC_LOG_LEVEL=4 -D AFEC_LOG_FORMAT=1`. |
| `src/config.h` | Pins, node IDs, radio settings, TX interval, sensor I²C addresses |
| `src/main.cpp` | `setup()`: radio, IRQ, sensors, `node.begin()`. `loop()`: poll, then read the sensors and send on the interval. |
| `src/afec_platform_arduino.h` | `afec::Platform` glue (`millis`, `random`, `yield`). Identical copy in the receiver. |

## Configuration (`config.h`)

| Constant | Value | Notes |
| --- | --- | --- |
| `PIN_SCK` / `PIN_MISO` / `PIN_MOSI` / `PIN_CSN` | 19 / 18 / 23 / 27 | SPI at 4 MHz |
| `PIN_GDO0` / `PIN_GDO2` | 26 / 16 | GDO0 is the RX/TX-done interrupt. GDO2 is unused. |
| `NODE_ID` / `DEST_NODE_ID` | 2 / 1 | |
| `RF_FREQ_MHZ` / `RF_SYNC_WORD` / `RF_TX_DBM` | 433.92 / `0x6B1D` / 0 | Must match the receiver. TX power must be one of −30, −20, −15, −10, 0, 5, 7, 10 dBm. |
| `USE_IRQ` | true | Falls back to polling if `enableInterrupt()` fails |
| `TX_INTERVAL_MS` | 2000 | |
| `BMP_390_ADDR` / `AHT10_ADDR` / `ENS160_ADDR` | 0x77 / 0x38 / 0x52 | Only the AHT works; the BMP390 and ENS160 are out of scope and read 0. The fitted part is an AHT20 at the same address; the report says AHT20. I²C uses the default `Wire` pins (ESP32: SDA 21, SCL 22). |
| `MMHG_TO_PA` | `uint8_t` = 133 | Unused. The `uint8_t` type truncates 133.322. |

**Protocol configuration is not in `config.h`.** `main.cpp` builds `afec::NodeConfig nc` with only `id` and `bitrateBps = 4800` set. Everything else is a library default:
- mode **C5 (AFEC)**, `initialLevel` 2;
- 3 retries, 400 ms ACK timeout;
- `downgradeAfter` 2, `nackStep` 2;
- 10 % duty, 36 s burst budget.

To run a different configuration today you have to edit `main.cpp` and reflash. The run plan (`../docs/07-run-plan.md`, P0) replaces this with a serial command layer, which has **not** been implemented yet.

## Runtime behaviour

1. **`setup()`**:
   1. Serial at 115200, then `radio.begin()`. If the chip doesn't answer it prints `CC1101 not found` and halts.
   2. `enableInterrupt()` and a `CC1101 ready, VERSION=…` line.
   3. Sensor init. Return values are not checked.
   4. `node.begin()` last, which logs the `begin` event. Expect about 1.5 s of boot delay before that.
2. **`loop()`**: `node.poll()` runs every iteration. When the interval has elapsed:
   1. `lastTx = millis()`.
   2. `ens160.wait()` blocks until the ENS160 has a new sample.
   3. Read AHT (temperature, humidity), write the compensation to the ENS160, read BMP pressure and ENS160 eCO2.
   4. Format the ASCII payload `t:<°C×100>;h:<%RH×100>;p:<Pa×100>;m:<ppm×100>;` into `char msg[32]`.
   5. `node.send(DEST_NODE_ID, msg, len)`, which blocks. The result is ignored; the outcome is only visible in the log.
3. **Effective period** = max(`TX_INTERVAL_MS`, sensor wait + `send()` time). `send()` can block for up to (1 + 3) × (airtime + 400 ms), which is about 2 s at the strongest level. So under heavy loss, sends can run back to back.

## Serial output

- JSON Lines from the library (all events at DBG). Filter on lines starting with `{`. The schema is in `cc1101-afec/docs/afec_log_schema.md`.
- Plain-text lines from the firmware: boot messages and IRQ fallback.
- `debugPrint` formats into a **192 B buffer**. A longer line is cut to 191 B **with no newline**, which would merge it with the next line. The longest observed line is 177 chars (`tx`). The worst case in the campaign, with a 9-digit `t`, is about 185. The margin is small, so keep it in mind if fields are added.

## Known issues (code is frozen; these are recorded, not fixed)

- **The BMP390 and ENS160 don't work, and are out of scope.** Pressure and eCO2 always read 0, so the payload is `t:<4 digits>;h:<4–5 digits>;p:0;m:0;` = 22–23 B.
- **Latent buffer overflow, harmless here.** `msg` is 32 B and `len` comes from `snprintf`'s return value, which is the untruncated length. With working sensors the string would reach about 33 chars, and `send()` would read past the buffer. With p = m = 0 the string can't exceed 23 B, so this never triggers. The campaign uses a synthetic payload anyway.
- **The payload length changes slightly with the temperature and humidity values (22–23 B)**, and so does the airtime. That's another reason for the synthetic payload in the campaign.
- **`ens160.wait()` blocks the loop**, including `node.poll()`. While it blocks, the node neither receives nor relays.
- **Sensor init failures are silent.** A missing sensor yields 0 or garbage values, not an error.

## Build, flash, capture

```sh
pio run -e dev                                     # build against ../cc1101-afec
pio run -e dev -t upload --upload-port /dev/cu.usbserial-5   # ask before flashing
pio device monitor -b 115200 -p /dev/cu.usbserial-5
```

On the current bench Mac, node 2 is on `/dev/cu.usbserial-5`. Both boards are CP2102 with identical serial numbers, so identify them by the `node` field in the log. For capturing and parsing logs, use `cc1101-afec-logs-parser/parse.py --mode serial` (see the parent repo).

If PlatformIO fails to download packages behind the TLS-intercepting proxy, set `REQUESTS_CA_BUNDLE` and `SSL_CERT_FILE` to `~/.config/certs/macos-bundle.pem`.
