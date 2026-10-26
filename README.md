# OBDLink STN J2534 driver

> [!WARNING]
> **Work in progress. Not recommended for use. Use entirely at your own risk.**
>
> **Using this driver can permanently damage vehicle modules, adapters and other connected devices, and leave them unrecoverable.**
>
> This driver has only been tested with an OBDLink EX and a single vehicle: diagnostics with FORScan, and diagnostics, service procedures and module configuration with Ford FDRS (see [Tested with FDRS](#tested-with-fdrs)). It has **not** been tested for module programming or flashing. A failed or interrupted module write can leave a module unusable and the vehicle inoperable.
>
> The software is provided as is, without warranty of any kind. The authors accept no liability for damage to vehicles, modules, adapters or anything else resulting from its use.

> [!CAUTION]
> **No CAN FD.** OBDLink STN adapters only support classic CAN: the OBDLink Family Reference and Programming Manual (rev F, section 8.6) lists ISO 11898 and ISO 15765 presets with at most 8 data bytes per frame and no CAN FD mode.
>
> - Do not use this driver for modules or vehicle networks that require CAN FD. Use an interface with CAN FD support for those.
> - The registry entry advertises no CAN FD protocols, and `PassThruConnect` rejects every protocol other than `CAN`, `ISO15765`, `CAN_PS` and `ISO15765_PS` with `ERR_INVALID_PROTOCOL_ID`.

A SAE J2534-1 v04.04 pass-thru DLL for OBDLink adapters with STN chips, connected as a COM port. It provides CAN and ISO 15765 channels on OBD pins 6/14 and, through the pin-switched protocols, on pins 3/11.

ISO-TP segmentation, flow control pacing and UDS response-pending (`7F xx 78`) handling run in the STN firmware.

## How this differs from a dedicated J2534 device

A purpose-built J2534 interface is designed around the pass-thru API. This project is different: the OBDLink is a serial diagnostic adapter, and this driver translates the J2534 calls into the adapter's own command set. Most of the real work — splitting long messages into frames, pacing them, waiting out a module that says "still working" — happens inside the adapter's firmware, so the driver can only do what that firmware allows. In practice that means:

- **One bus at a time.** A dedicated device can keep several channels live at once. This adapter serves one CAN bus at a time and switches between the two pin pairs (6/14 and 3/11) before sending on the other one. Switching takes a moment, more so when many message filters are set.
- **It mostly listens while it is sending.** The adapter collects replies in the window right after it sends a request. Between requests it can passively watch one bus, but it is not a full always-on receiver: a long multi-part reply that only begins after that window arrives incomplete. Dedicated hardware receives continuously.
- **It is slower per message.** Every message crosses a serial link to the adapter and back, which adds overhead a built-in device does not have. It is quick enough for diagnostics, but not as fast as purpose-built hardware.
- **Some settings are fixed or unavailable.** Block size and separation time (the module's own values are always used), extended addressing, programming voltage and CAN FD are fixed or not supported. Most tools accept this and carry on, but the behaviour is not identical to a full device.
- **Not proven for programming.** It has been used for diagnostics, service procedures and module configuration, but never to flash firmware. A flash sends far more data over a much longer time and erases the module before writing — and a flash that is interrupted can leave a module unusable. Treat flashing as untested and risky.

## Build

Every push and pull request is built and tested by the GitHub Actions workflow in `.github/workflows/build.yml`. Each push to `main` with a new version in `CMakeLists.txt` publishes a release `v<version>` with the installable zip and its SHA-256 checksum.

The same steps run locally in Docker:

```sh
docker compose run --rm test      # unit tests against an emulated STN
docker compose run --rm win32     # build/win32/obdstn32.dll
docker compose run --rm win64     # build/win64/obdstn64.dll
docker compose run --rm package   # dist/obdlink-stn-j2534-<version>.zip
```

## Install on Windows

1. Install the FTDI virtual COM port driver for the adapter and note its COM port (Device Manager → Ports).
2. Download `obdlink-stn-j2534-<version>.zip` from the GitHub releases and extract it to a permanent folder, for example `C:\OBDLinkJ2534`. The registry points at the DLLs in that folder.
3. In an elevated PowerShell in that folder:

   ```powershell
   .\register.ps1 -ComPort COM5 -LogFile C:\OBDLinkJ2534\trace.log
   ```

   `.\register.ps1 -Unregister` removes the registration.

The script registers the 32-bit DLL in the 32-bit registry view (`HKLM\SOFTWARE\WOW6432Node\PassThruSupport.04.04`) and the 64-bit DLL in the native view.

### Faster response timing (optional)

FTDI COM ports ship with a 16 ms latency timer that delays every reply from the adapter by up to that long, which dominates per-message round-trip time. Adding `-LowLatency` lowers it to 2 ms for the adapter's port and reloads the port so the change applies without unplugging it:

```powershell
.\register.ps1 -ComPort COM5 -LowLatency
```

This is off by default and opt-in, because it changes a Windows setting on the FTDI port device. The same value can be set by hand in Device Manager → the port → Properties → Port Settings → Advanced → Latency Timer.

## Settings

Values under `HKLM\SOFTWARE\PassThruSupport.04.04\OBDLink STN` (32-bit view for the 32-bit DLL):

| Value | Type | Default | Meaning |
|---|---|---|---|
| `ComPort` | string | — | Adapter COM port. A non-empty name passed to `PassThruOpen` overrides it. |
| `HostBaudRate` | DWORD | 115200 | UART rate the adapter starts at. |
| `AdapterBaudRate` | DWORD | 2000000 | UART rate to switch to after opening. A lower value limits the rate; 0 keeps `HostBaudRate`. |
| `CommandTimeoutMs` | DWORD | 1000 | Wait for the adapter to answer a configuration command. |
| `ResetTimeoutMs` | DWORD | 3000 | Wait for the adapter to finish a reset. |
| `BusyProbeMs` | DWORD | 300 | Wait when resynchronizing a possibly busy adapter. |
| `LogFile` | string | empty | Appends every adapter command and response line with a microsecond timestamp. |

## Behaviour and limits

- Supported protocols: `CAN`, `ISO15765`, `CAN_PS` and `ISO15765_PS` (`J1962_PINS` 0x060E or 0x030B), 11- and 29-bit IDs.
- After each transmit the adapter receives responses with adaptive timing: it returns once the gap since the last frame exceeds the learned inter-frame time, bounded by its response timeout (the adapter's default, 102 ms) as the maximum.
- Between transmits the adapter monitors one bus that has channels with receive filters. The adapter sends no flow control while monitoring, so a multi-frame response received this way delivers only its first frame.
- Flow control and consecutive frame timeouts are the ISO 15765-2 N_Bs and N_Cr limits (1000 ms).
- The device is declared lost when the adapter stays silent during a transmit longer than ISO 14229-2 P2*Server_max (5000 ms) plus N_Bs and the maximum STmin (127 ms) for each consecutive frame sent.
- `PassThruWriteMsgs` completes each transmission before returning; its timeout argument is not used.
- One CAN bus is active at a time; channels on different pins switch the adapter between buses before each transmit.
- Up to 64 message filters per channel.
- No ISO 15765 extended addressing and no programming voltage (`PassThruSetProgrammingVoltage` accepts only `VOLTAGE_OFF`).
- `BS_TX`, `STMIN_TX` (ECU values are always used), `ISO15765_WFT_MAX` (0), `BIT_SAMPLE_POINT` (80), `SYNC_JUMP_WIDTH` (15) and `CAN_MIXED_FORMAT` (0) are fixed and accept only those values.
- A message that the adapter does not answer in time returns `ERR_TIMEOUT` for that call and leaves the device usable, so the application can retry. Only an adapter reset reports `ERR_DEVICE_NOT_CONNECTED` until the device is closed and reopened.

## Tested with FDRS

Tested with Ford FDRS 50.5.2, the 32-bit DLL, an OBDLink EX and one vehicle.

| Operation | Result |
|---|---|
| Vehicle identification and network test | Works |
| Reading DTCs | Works |
| Service procedures | Works |
| Module configuration | Works |
| Module programming | Not tested |

FDRS logs errors for removing flow control filters and for the unsupported `CAN_MIXED_FORMAT` setting, and continues without that setting.

## Status

Work in progress. Diagnostics work with FORScan and FDRS, and service procedures and module configuration work with FDRS, on the tested adapter and vehicle. Module programming is untested. CAN FD and ISO 15765 extended addressing are not supported.

## Trademarks and affiliation

This is an independent project. It is not affiliated with, endorsed by or supported by OBD Solutions, Ford Motor Company, the FORScan developers or any other company or product named in this document.

OBDLink, STN, Ford, FDRS, FORScan and all other product and company names are trademarks or registered trademarks of their respective owners. They are used only to identify the hardware and software this driver works with.

## License

MIT; see [LICENSE](LICENSE). The software is provided as is, without warranty.
