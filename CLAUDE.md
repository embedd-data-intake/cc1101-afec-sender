# CLAUDE.md

This is the sender firmware (node 2) for the AFEC thesis. It runs on ESP32 + CC1101, takes serial commands from the campaign orchestrator, and sends blocks of synthetic packets through `afec::Node` from the `cc1101-afec` library. It is a submodule of the thesis repo `afec`. Read `docs/firmware.md` first. For protocol internals and the log schema, see `../cc1101-afec/docs/`.

## Status: frozen

The firmware is considered finished. **Do not refactor or add tests.** Only change it for a real bug found during measurement, or for something the user asks for. The serial command layer and synthetic payload (`../docs/07-run-plan.md`, P0.1) were added on 2026-11-27 at the user's request.

`docs/firmware.md` lists known issues as records. Do not fix them unasked. The BMP390 and ENS160 sensors don't work and are **out of scope**: pressure and eCO2 always read 0. Don't debug them or flag them.

## Layout

- `src/config.h`: pins, node IDs (2 → 1), boot radio settings, default `TX_INTERVAL_MS`, `SENSOR_TX` (legacy sensor mode, off), sensor addresses.
- `src/main.cpp`: setup, command handlers (`hello`, `cfg`, `mark`, `reset`, `run`, `stop`, `stats`, `bench`), the `run` loop and the legacy sensor path. Boots in C5 (library defaults); `cfg` switches the configuration at run time.
- `src/afec_cmd.h`: command parser, `cfg` parsing, JSON event helper, synthetic payload + CRC32. Keep it identical to the receiver's copy.
- `src/afec_platform_arduino.h`: `afec::Platform` glue. Keep it identical to the receiver's copy.
- `src/afec_log_sink.h`: log sink chosen with `-D AFEC_LOG_SINK=` (0 off, 1 serial, 2 MQTT, 3 both). Keep it identical to the receiver's copy. MQTT builds (`env:dev-mqtt`) need the gitignored `src/secrets.h` (template: `secrets.example.h`); never commit it.
- `platformio.ini`: `esp32dev` uses the GitHub library; `dev` uses the `../cc1101-afec` symlink. Both use `AFEC_LOG_LEVEL=4` and `AFEC_LOG_FORMAT=1`, which the log parser needs, and `AFEC_LOG_SINK=1` (serial). `dev-mqtt` = `dev` with `AFEC_LOG_SINK=2`.

## Commands

```sh
pio run -e dev                                   # build (allowed without asking)
pio run -e dev -t upload --upload-port <port>    # flashing touches hardware: ask first
```

- The node 2 port on the bench Mac is `/dev/cu.usbserial-5`. Confirm it by the `node` field in the serial log.
- If PlatformIO fails to download packages, pass `REQUESTS_CA_BUNDLE=~/.config/certs/macos-bundle.pem SSL_CERT_FILE=~/.config/certs/macos-bundle.pem` (TLS-intercepting proxy).

## Conventions

- **Keep radio settings and `NodeConfig` in step with the receiver:** frequency, sync word, data rate, mode. A mismatch silently breaks the link or the comparison. At run time this means sending the same `cfg` to both nodes.
- **Firmware events** (`cfg_ok`, `app_tx`, `block_end`, …) use the library's JSON line format and are listed in `docs/firmware.md`. If you change one, update the parser too.
- **Firmware output that isn't JSON** must not start with `{`. The parser keeps only lines that start with `{`.
- **Committing:** this is its own repo. Commit here first, then commit the updated submodule pointer in the parent `afec` repo. When firmware changes, record the commit in the run's `meta.json` and update `docs/` here and `../docs/06-status-and-open-items.md`.
