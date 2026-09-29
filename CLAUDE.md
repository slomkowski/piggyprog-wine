# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A drop-in replacement for FTDI's `ftd2xx.dll` that re-implements the D2XX API on top of the
**Win32 serial API**, so D2XX-based Windows apps run under Wine. Target app: Ottomotive
PiggyProg v1.5 talking to a "Digital ECU Cheater" (FT232R, `0403:6001`).

The whole design rests on one fact: the real DLL needs FTDI's kernel driver `ftdibus.sys`,
which Wine cannot load; but Linux's `ftdi_sio` already exposes the same chip as
`/dev/ttyUSB*`, which Wine surfaces as a COM port. So the shim is a **pure PE DLL** —
no winelib, no libftdi/libusb, no Unix calls. It must stay that way.

Not a git repository. Read `README.md` first; it is the design document.

## Build / test

```sh
make                 # ftd2xx.dll (32-bit PE), shimtest.exe, localeprobe.exe
make check           # winedump -j export ftd2xx.dll — verify the ordinal map
make install         # copy into PIGGYDIR, backing up FTDI's original once
make uninstall       # restore the original
```

Requires `mingw-w64-gcc` (`i686-w64-mingw32-gcc`). `PIGGYDIR` in the Makefile is a hardcoded
absolute path to the user's Wine prefix.

There is no unit-test framework. The test is `shimtest.exe`, which resolves every entry point
**by ordinal** (exactly as PiggyProg does) and does one write + one read — it separates a
broken ordinal map from a wiring problem:

```sh
FTD2XX_SHIM_LOG=/tmp/t.log FTD2XX_SHIM_TRACE=1 wine ./shimtest.exe 115200
```

End-to-end run: `./piggyprog --trace --log /tmp/ftd2xx.log`.

## The two things that break silently

**1. Export ordinals.** `PiggyProg.exe`'s import table contains no function names, only
ordinals. `ftd2xx.def` reproduces FTDI's original numbering verbatim (obtained via
`winedump -j export` on the shipped DLL). Changing or renumbering an entry binds the app to
the *wrong function* with no error. When adding an export, look up its real FTDI ordinal —
never assign the next free number. Note the def uses `Name = Name@N @ordinal` form, so the
stdcall byte count must match the signature.

**2. Locale.** PiggyProg is Delphi; every numeric in `Config/*.ini` uses a comma decimal
(`12,8`). Delphi's `StrToFloat` reads the separator from the Windows regional settings, and
Wine derives those from `LC_ALL` at process start. Under a default en-US prefix every load
raises `'12,8' is not a valid floating point value`. Fix is per-process: `LC_ALL=pl_PL.UTF-8`
(the `piggyprog` script does this, and warns if the locale isn't generated on the host).
`localeprobe.exe` prints the LCID/`sDecimal` Wine hands the app.

## Layout

- `ftd2xx_shim.c` — the entire DLL. Single global state block (`g_port`, `g_cfg_*`, a
  `CRITICAL_SECTION`), config loaded once in `DllMain`, then the D2XX entry points.
- `ftd2xx.def` — the ordinal map. See above.
- `shimtest.c` — ordinal-resolving self-test.
- `localeprobe.c` — prints Wine's regional settings.
- `piggyprog` — launcher: finds the adapter by walking sysfs (vid:pid + USB product string),
  repoints `$WINEPREFIX/dosdevices/com2` at the right `/dev/ttyUSB*`, exports
  `FTD2XX_SHIM_*`, sets the locale, then `exec wine PiggyProg.exe`. It exists because Wine's
  COM symlinks are static and go stale on re-enumeration.

## Behaviour contracts worth preserving

These are load-bearing; several were arrived at by debugging against the real app.

- The port is opened `FILE_FLAG_OVERLAPPED` and every read/write gets its own `OVERLAPPED`
  (`sync_io`). PiggyProg polls `FT_GetStatus` from the UI thread while a worker blocks in
  `FT_Read`; a non-overlapped handle would serialize them.
- `FT_SetTimeouts` maps to a **total** `COMMTIMEOUTS`, not inter-character. `0` means "wait
  forever" in both APIs. A short read is a timeout: return `FT_OK` with a reduced count, not
  an error.
- PiggyProg never calls `FT_Close`, so `FT_Open` recycles an already-open handle; otherwise a
  reconnect hits a sharing violation.
- `FT_CreateDeviceInfoList` probes by open/close with a 1-second cache so polling clients
  don't thrash the tty.
- The handle is a constant sentinel (`SHIM_HANDLE`), only ever compared, never dereferenced.
- EEPROM / bit-bang / MPSSE return `FT_NOT_SUPPORTED` — they cannot work while `ftdi_sio`
  owns the chip. Supporting them would mean unbinding the kernel driver and going through
  `libftdi1`, i.e. abandoning the pure-PE design.
- Description, serial, and VID/PID reported by `FT_GetDeviceInfoList` are configurable
  (`FTD2XX_SHIM_DESC` / `_SERIAL` / `_VIDPID`) because clients filter their device list on
  them. Full env var table is in `README.md`.
