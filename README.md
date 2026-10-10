# ftd2xx-wine-shim

A drop-in replacement for FTDI's `ftd2xx.dll` that lets D2XX-based Windows applications
run under Wine on Linux. Written for **Ottomotive PiggyProg v1.5** talking to a
**"Digital ECU Cheater"** (FT232R, `0403:6001`), but nothing in it is app-specific —
any D2XX client that only needs a UART will work.

## The problem

`PiggyProg.exe` imports `FTD2XX.DLL` and **never opens a COM port**. FTDI's real
`ftd2xx.dll` reaches the chip through the Windows kernel driver `ftdibus.sys`. Wine has
no such driver and cannot load one (it is kernel-mode), so the stock DLL can only ever
answer `FT_DEVICE_NOT_FOUND`. Installing the bundled CDM driver package into the prefix
does not help.

Meanwhile Linux already drives the exact same chip: `ftdi_sio` binds it and exposes
`/dev/ttyUSB0`, which Wine happily surfaces as a normal COM port.

## The fix

PiggyProg uses only **9 D2XX entry points**, all of them plain UART operations:

| Ordinal | Function |
|--------:|----------|
| 1  | `FT_Open` |
| 3  | `FT_Read` |
| 4  | `FT_Write` |
| 7  | `FT_SetBaudRate` |
| 8  | `FT_SetDataCharacteristics` |
| 17 | `FT_SetTimeouts` |
| 21 | `FT_GetStatus` |
| 70 | `FT_CreateDeviceInfoList` |
| 71 | `FT_GetDeviceInfoList` |

No bit-bang, no MPSSE, no EEPROM. So this DLL re-implements the D2XX API on top of the
Win32 serial API and lets Wine + `ftdi_sio` do the actual work. It is a pure PE DLL —
no winelib, no libftdi, no libusb, no root, no udev rules.

**The ordinals matter.** PiggyProg's import table contains no function *names*, only
ordinals, so `ftd2xx.def` reproduces FTDI's original ordinal numbering exactly. Those
numbers came from `winedump -j export` on the shipped DLL. Changing them silently binds
the app to the wrong functions.

## Build and install

```sh
make                # builds ftd2xx.dll (32-bit PE) and shimtest.exe
make check          # print the export table, verify ordinals
make install        # copy into the PiggyProg dir, backing up FTDI's original once
make uninstall      # restore FTDI's original
```

Requires `mingw-w64-gcc`. The original DLL is preserved as `ftd2xx.dll.orig-windows`;
nothing else in the app directory is touched and `PiggyProg.exe` itself is unmodified.

## Run

```sh
./piggyprog                                  # auto-detects the adapter, then runs
./piggyprog --trace --log /tmp/ftd2xx.log    # full protocol hex dump
./piggyprog --locale pl_PL.UTF-8             # override the locale (see below)
```

`piggyprog` does two things before starting Wine:

1. **Finds the adapter.** It walks sysfs (vid:pid + USB product string) and pins COM2 to
   whatever `/dev/ttyUSB*` the device landed on, via `HKLM\Software\Wine\Ports`. Editing
   `~/.wine/dosdevices/com2` directly does not work: Wine's mountmgr regenerates every
   `comN` symlink at prefix startup from that registry key plus its own numbering, so a
   hand-made link is silently replaced (typically with `/dev/ttyS0`).
2. **Sets a Polish locale.** See below — the app does not work without it.

Running `wine PiggyProg.exe` by hand also works, as long as COM2 is pinned to the right tty
(`wine reg add 'HKLM\Software\Wine\Ports' /v COM2 /d /dev/ttyUSB0 /f`) *and* you set
`LC_ALL`. The DLL next to the EXE takes precedence, so no `WINEDLLOVERRIDES` is needed:

```sh
LC_ALL=pl_PL.UTF-8 wine PiggyProg.exe
```

## Locale: why `12,8 is not a valid floating point value` happens

PiggyProg is a Delphi app and every numeric value in its config files uses a **comma**
as the decimal separator:

```
Config/MP/1.5.3.ini      OP=2 12,8        OP=2 2,55      OP=2 255 1 15,04 0 7,35
Config/measurement.ini   MAX_VALUE=8,16   SHORT_TICK=0,2   FORKH=5,0
```

There is not a single dot-decimal in any of them. Delphi's `StrToFloat` takes its
separator from the Windows regional settings (`LOCALE_SDECIMAL` of
`LOCALE_USER_DEFAULT`), so on an en-US prefix each of those raises
`'12,8' is not a valid floating point value`.

A default Wine prefix is created as `00000409` (en-US) and stores that in
`HKCU\Control Panel\International`, which is why the error appears on a fresh Linux
install and never on the developer's Polish Windows box.

Wine reads the Windows locale from `LC_ALL` **at process start**, so this is a
per-process fix that leaves the prefix and every other app in it untouched:

| `LC_ALL` | `GetUserDefaultLCID()` | `sDecimal` |
|---|---|---|
| `en_US.UTF-8` | `0x0409` | `.` &nbsp;← breaks |
| `pl_PL.UTF-8` | `0x0415` | `,` &nbsp;← works |

`localeprobe.exe` prints that table for the current environment if you need to check.

The locale must be **generated on the host**, not merely requested. On Arch, uncomment
`pl_PL.UTF-8 UTF-8` in `/etc/locale.gen` and run `locale-gen`; `piggyprog` warns loudly
if it is missing instead of silently starting a broken app. Any locale with a comma
decimal separator works — Polish just also gives the app its native UI encoding.

If you launch from a desktop shortcut and cannot set the environment, the alternative is
a prefix-wide override (affects every app in that prefix):

```sh
wine reg add "HKCU\Control Panel\International" /v sDecimal /t REG_SZ /d "," /f
```

## Self-test

`shimtest.exe` resolves every entry point **by ordinal**, exactly as PiggyProg does, then
opens the port and does one write + one read. It is the fastest way to tell a broken
ordinal map apart from a wiring problem:

```sh
FTD2XX_SHIM_LOG=/tmp/t.log FTD2XX_SHIM_TRACE=1 wine ./shimtest.exe 115200
```

## Configuration

All optional, read once when the DLL loads.

| Variable | Default | Meaning |
|---|---|---|
| `FTD2XX_SHIM_PORT`   | `COM2,COM1,COM3,COM4` | Comma-separated port list; first one that opens wins |
| `FTD2XX_SHIM_DESC`   | `Digital ECU Cheater` | Description reported by `FT_GetDeviceInfoList` |
| `FTD2XX_SHIM_SERIAL` | `A7BGR844`            | Serial number reported by `FT_GetDeviceInfoList` |
| `FTD2XX_SHIM_VIDPID` | `0x04036001`          | `ID` field of the info node |
| `FTD2XX_SHIM_DTR`    | `1`                   | Assert DTR on open |
| `FTD2XX_SHIM_RTS`    | `1`                   | Assert RTS on open |
| `FTD2XX_SHIM_LOG`    | *(off)*               | Trace log path |
| `FTD2XX_SHIM_TRACE`  | `0`                   | Also hex-dump every read/write payload |

The description and serial are reported verbatim to the app, so if a client filters its
device list by name, set them to match.

## Implementation notes

* The port is opened `FILE_FLAG_OVERLAPPED` and every read/write is wrapped in its own
  `OVERLAPPED`. A blocking read on one thread therefore never stalls a write on another,
  which is how real D2XX behaves and which PiggyProg relies on (it polls `FT_GetStatus`
  from the UI thread while a worker reads).
* `FT_SetTimeouts` maps to a **total** `COMMTIMEOUTS`, not an inter-character one, and
  `0` means "wait forever" in both APIs. A short `FT_Read` is a timeout and returns
  `FT_OK` with a reduced count — not an error.
* PiggyProg never calls `FT_Close`, so `FT_Open` recycles any handle still open. Without
  this, a reconnect would hit a sharing violation.
* `FT_CreateDeviceInfoList` probes by opening and closing the port, with a 1-second
  result cache so a polling client does not thrash the tty.
* EEPROM and bit-bang calls return `FT_NOT_SUPPORTED` — they cannot work while the
  kernel driver owns the chip. If an app needs those, it needs the libusb route instead
  (unbind `ftdi_sio`, drive the chip with `libftdi1`), which is a much bigger job.

## Verified behaviour

With this shim, PiggyProg v1.5 under Wine 11.18:

* enumerates the device as `Digital ECU Cheater` / `A7BGR844`;
* shows a green **connected** status and reads **firmware 1.5.3.17** off the ECU;
* sets **111000 baud** (non-standard — Wine accepts it via a custom divisor) 8N1;
* exchanges the full `02 … ff 03` framed protocol: firmware query, map-table download
  (`02 02 4d …` page reads), and continuous ~41-byte telemetry frames every ~46 ms;
* runs for thousands of transfers with zero I/O errors or timeouts;
* reports 0 devices cleanly, without crashing, when the adapter is absent;
* under `LC_ALL=pl_PL.UTF-8`, loads the ECU definitions without the
  `'12,8' is not a valid floating point value` error, and renders Polish text
  correctly (`Projekt już istnieje` rather than the mangled `Projekt juz istnieje`
  you get from an en-US prefix).
