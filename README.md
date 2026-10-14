# OBDLink STN J2534 driver

> [!WARNING]
> **Work in progress. Not recommended for use. Use entirely at your own risk.**
>
> **Using this driver can permanently damage vehicle modules, adapters and other connected devices, and leave them unrecoverable.**
>
> This driver has only been tested for **reading sensor values and scanning modules** with FORScan 2.3.71, an OBDLink EX (STN2231 v5.10.3) and a single vehicle. It has **not** been tested for module programming, flashing, configuration writes or any other operation that changes a vehicle module. A failed or interrupted module write can leave a module unusable and the vehicle inoperable.
>
> The software is provided as is, without warranty of any kind. The authors accept no liability for damage to vehicles, modules, adapters or anything else resulting from its use.

> [!CAUTION]
> **No CAN FD.** OBDLink STN adapters only support classic CAN: the OBDLink Family Reference and Programming Manual (rev F, section 8.6) lists ISO 11898 and ISO 15765 presets with at most 8 data bytes per frame and no CAN FD mode. This is the same limitation as lower-cost pass-thru interfaces such as the VXDIAG VCX Nano and VCX SE, which VXDIAG lists without CAN FD support.
>
> - Do not use this driver for modules or vehicle networks that require CAN FD. Use an interface with CAN FD support for those.
> - The registry entry advertises no CAN FD protocols, and `PassThruConnect` rejects every protocol other than `CAN`, `ISO15765`, `CAN_PS` and `ISO15765_PS` with `ERR_INVALID_PROTOCOL_ID`.

A SAE J2534-1 v04.04 pass-thru DLL for OBDLink adapters with STN chips, connected as a COM port. It provides CAN and ISO 15765 channels on OBD pins 6/14 and, through the pin-switched protocols, on pins 3/11.

ISO-TP segmentation, flow control pacing (the ECU's STmin) and UDS response-pending (`7F xx 78`) waits run in the STN firmware; the DLL drives the adapter with `STPX` and reassembles received frames.

## Build

Everything builds in Docker:

```sh
docker compose run --rm test    # Linux unit tests against an emulated STN
docker compose run --rm win32   # build/win32/obdstn32.dll
docker compose run --rm win64   # build/win64/obdstn64.dll
```

## Install on Windows

1. Install the FTDI virtual COM port driver for the adapter and note its COM port (Device Manager → Ports).
2. Copy `obdstn32.dll`, `obdstn64.dll` and `scripts/register.ps1` into one folder.
3. In an elevated PowerShell in that folder:

   ```powershell
   .\register.ps1 -ComPort COM5 -LogFile C:\obdlink\trace.log
   ```

   `.\register.ps1 -Unregister` removes the registration.

The script registers the 32-bit DLL in the 32-bit registry view (`HKLM\SOFTWARE\WOW6432Node\PassThruSupport.04.04`) and the 64-bit DLL in the native view.

## Settings

Values under `HKLM\SOFTWARE\PassThruSupport.04.04\OBDLink STN` (32-bit view for the 32-bit DLL):

| Value | Type | Default | Meaning |
|---|---|---|---|
| `ComPort` | string | — | Adapter COM port. A non-empty name passed to `PassThruOpen` overrides it. |
| `HostBaudRate` | DWORD | 115200 | UART rate the adapter starts at. |
| `AdapterBaudRate` | DWORD | 0 | Rate to switch to with `STSBR` after opening; 0 keeps `HostBaudRate`. |
| `ResponseTimeoutMs` | DWORD | 200 | `STPX t:` wait for the first response frame. |
| `TransferSilenceTimeoutMs` | DWORD | 10000 | Adapter silence after which the device is declared lost; each transmit adds 127 ms (the adapter's separation-time ceiling) per consecutive frame it has to pace. |
| `CommandTimeoutMs` | DWORD | 1000 | Wait for AT/ST command prompts. |
| `ResetTimeoutMs` | DWORD | 3000 | Wait for `ATZ`. |
| `BusyProbeMs` | DWORD | 300 | Wait when resynchronizing a possibly busy adapter. |
| `LogFile` | string | empty | Appends every adapter command and response line with a microsecond timestamp. |

## Behaviour and limits

- Supported protocols: `CAN`, `ISO15765`, `CAN_PS` and `ISO15765_PS` (`J1962_PINS` 0x060E or 0x030B), 11- and 29-bit IDs.
- The adapter receives only while a transmit runs: responses arriving later than `ResponseTimeoutMs` without a preceding `7F xx 78` are not received.
- `PassThruWriteMsgs` completes each transmission before returning; its timeout argument is not used.
- One CAN bus is active at a time; channels on different pins switch the adapter preset before each transmit.
- No ISO 15765 extended addressing and no programming voltage (`PassThruSetProgrammingVoltage` accepts only `VOLTAGE_OFF`).
- `BS_TX`, `STMIN_TX` (ECU values are always used), `ISO15765_WFT_MAX` (0), `BIT_SAMPLE_POINT` (80), `SYNC_JUMP_WIDTH` (15) and `CAN_MIXED_FORMAT` (0) are fixed and accept only those values.
- After an adapter timeout or reset the device reports `ERR_DEVICE_NOT_CONNECTED` until it is closed and reopened; no transmission is retried.

## License

MIT; see [LICENSE](LICENSE). The software is provided as is, without warranty.
