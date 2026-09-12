# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Linux daemon (`ups-server`) that talks over RS232 serial to a Bicker PSZ-1063 uExtension module
attached to a Bicker UPS, and exposes the UPS telemetry to a small vanilla-JS web UI. On detecting
an input power failure it can trigger a delayed, cancellable system shutdown (`shutdown`/`reboot`).
The same TCP port also speaks an apcupsd-compatible protocol so tools like `apcaccess` or Netdata's
apcupsd plugin can query it.

## Build

```bash
make            # builds server/ups-server (gcc, C18)
make clean
```

Build dependencies (system packages, not vendored): `libpthread-stubs0-dev`, `libwebsockets-dev`,
`libconfig-dev`, `libjson-c-dev`.

There is no separate test target — this is a hardware-driven daemon with no unit test suite.

Debian packaging lives in `debian/`; build a package with `dpkg-buildpackage -b -uc`. Installed
layout is `/opt/ups-server` (binary + `client/` web assets), config at
`/etc/default/ups-server.cfg`.

Run locally against a config file with `server/ups-server --config=<path>` (see `.vscode/launch.json`
for a gdb launch example). Default config template is `server/ups-server.cfg`.

## Client lint

```bash
npm run lint    # eslint (airbnb config) over client/
```

`client/` is intentionally framework-free — no bundler/build step for the JS.

## Architecture

### Server (`server/`, C)

- `ups-server.c` — entry point, libwebsockets context/vhost setup, config parsing (libconfig),
  argp CLI parsing, and all three network-facing pieces:
  - **HTTP mount** serving the web app from `client/` (`/opt/ups-server/client` in the installed
    layout) as the default `/` route.
  - **`callback_broadcast`** — the `broadcast` websocket protocol the web UI connects to. Pushes a
    JSON snapshot of `bicker_ups_status_t` to all connected clients roughly every
    `UPDATE_TIME_SEC`. Also accepts a JSON `{"cmd": "capesr"}` message from a client
    (`handle_client_request`) to trigger a capacitor ESR measurement.
  - **`callback_raw`** — a raw TCP protocol on the *same port*, selected via
    `LWS_SERVER_OPTION_FALLBACK_TO_RAW`, that answers with an apcupsd-style `STATUS` text block
    (built in `apc_update_status`) for compatibility with `apcaccess`/Netdata.
  - Three background threads coordinate through global state guarded by `lock_ups_status`:
    - `ups_read_handler` — polls the serial link continuously via `bicker.c`, updates the shared
      `bicker_ups_status_t`, tracks power-fail counts, calls `log_to_file`/`event_log`, and spawns
      `shutdown_handler` when input power is lost.
    - `shutdown_handler` — waits out the configured shutdown delay (or SOC threshold), and unless
      power returns first (`shutdown_override`), issues the actual system shutdown.
    - main thread runs the `lws_service` event loop.
  - Shutdown policy is configurable: by fixed delay (`shutdownByTime`/`shutdownDelay`) and/or by
    battery state-of-charge threshold (`shutdownBySoc`/`shutdownSocPercent`); at least one must be
    enabled (enforced at startup in `main`).
  - Optional CSV telemetry logging (`logToFile`) and a persistent event log (`eventLog` path,
    service start/stop, power fail/good, shutdown events).

- `bicker.c` / `bicker.h` — the serial protocol layer for the PSZ-1063 module itself: frame
  format (`bicker_data_t`: SOH/size/cmd_index/cmd_list/data, terminated by EOT), the `cmd_list_t`
  command enum (two command index families — `0x01` is native Bicker/UPS registers like voltages,
  currents, SOC, temperature, string identifiers; `0x03` is a passthrough to LTC3350 supercap-
  charger registers, see `server/notes` for the register map from the LTC3350 datasheet), and
  `get_ups_status()` which returns the parsed `bicker_ups_status_t` used throughout
  `ups-server.c`. Register semantics and the apcupsd field mapping are documented in `server/notes`.

- `help.h` — argp option table used by `ups-server.c`.

### Client (`client/`, plain JS/HTML/CSS, no build step)

- `index.html` + `css/bootstrap.min.css` — static Bootstrap-based single page UI (gauges/fields
  bound by element id).
- `js/index.js` — DOM update logic (`UpdateGui`), decodes the JSON snapshot from the server into
  the on-page fields/checkboxes/indicators (bitfields like `deviceStatus`/`chargeStatus`/
  `monitorStatus` are unpacked with bitmasks matching the enums in `bicker.h`).
- `js/ws.worker.js` — a dedicated Web Worker owning the actual `WebSocket` connection to
  `ws://<host>:10024` on the `broadcast` protocol; reconnects automatically after a close
  (10s backoff) and relays parsed messages back to the main thread via `postMessage`. The main
  page never touches the socket directly.

### Configuration (`server/ups-server.cfg`, libconfig format)

Two top-level sections: `server` (network bind, serial device path, daemonize, shutdown policy,
logging paths) and `ups` (nominal ratings for the attached hardware — input/battery voltage,
power-return percent, max backup time, wakeup delay, max amps — used to compute derived values
like nominal output power and to interpret raw readings).
