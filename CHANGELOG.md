# Changelog

## 0.4.0 — 2026-10-28

### Fixed

- Adapter errors while monitoring between transmits, and failed periodic messages, no longer fail the next `PassThruReadMsgs` or `PassThruWriteMsgs` with `ERR_FAILED`. They are written to the log file, and an adapter that cannot be recovered makes reads return `ERR_DEVICE_NOT_CONNECTED`.
- An interrupt that crosses the adapter's own prompt no longer corrupts the next command, which made commands such as `STPO` and `STVR 3` fail with `?`.
- After an adapter timeout the driver brings the adapter back to an idle prompt before the next command.

### Tested

- 0.3.0 with FDRS: a DC/DC converter software update completed and was reported successful. An RFA software update failed part-way.

## 0.3.0 — 2026-10-26

### Changed

- Lower per-message latency: adaptive response timing, and an early return once a complete ISO 15765 response is assembled.

### Added

- `register.ps1 -LowLatency` lowers the FTDI latency timer from 16 ms to 2 ms.

## 0.2.0 — 2026-10-19

### Added

- Passive receive between transmits on one bus.

### Fixed

- Reopening the device, failure reporting and timing. A response timeout fails only that message with `ERR_TIMEOUT` and leaves the device usable; only an adapter reset marks it not connected.

## 0.1.0 — 2026-10-15

### Added

- Work-in-progress OBDLink STN pass-thru driver.
- Up to 64 message filters per channel; FDRS vehicle identification sets one flow control filter per module.
- Release builds published from CI.
