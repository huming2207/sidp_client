# SIDP power service (opcodes 0x0200 and up)

The power service streams the target's current and supply voltage over the
same SIDP connection as debugging and the management service, and runs
alongside both. It keeps SIDP's rules: the 12-byte header and CRC, one
outstanding request, and no retry after a timeout.

Requests, responses and the stop event carry protobuf messages from
`proto/power.proto`. Responses still start with the 4-byte status, and carry
no message unless the status is OK. Data frames are not protobuf: they are
`sidp::power_data_t` followed by runs of raw samples (`include/sidp_defs.hpp`),
because they are frequent fixed-size records.

## Hardware model (Rev 8)

Three INA226s sit in series between `VPP_INPUT` and `VPP_OUTPUT` and measure
the HIGH (50 mΩ), MID (4.02 Ω) and LOW (162 Ω) ranges. Comparators move the
range up in hardware; the firmware moves it down. All three INA226 bus inputs
are on `VPP_INPUT`, so Rev 8 measures the input voltage only, and the host
computes the output voltage.

The device sends raw register values. Conversion, zero correction and Vout are
done on the host, with the constants from `POWER_INFO`:

```text
current_a = shunt_code x shunt_lsb_v / ranges[range].shunt_ohm
vin_v     = bus_code x bus_lsb_v
vout_v    = vin_v - current_a x ranges[range].chain_ohm
```

The zero (the reading with the output open, mostly INA226 input bias current)
is measured and stored by the host. With nothing on `VPP_OUTPUT`, it starts a
measurement with `lowest_range` set to LOW, then MID, then HIGH, and stores
each range's average current divided by its average Vin (A/V). It then
subtracts that figure times the present Vin.

## Opcodes

| Opcode | Kind | Name | Request | Response or payload |
|--------|------|------|---------|---------------------|
| 0x0200 | Request | `POWER_INFO` | empty | `PowerInfo` |
| 0x0201 | Request | `POWER_START` | `StartRequest` | `StartResponse` |
| 0x0202 | Request | `POWER_STOP` | empty | `StopResponse` |
| 0x0210 | Log stream | `POWER_DATA` | — | `power_data_t` and runs |
| 0x0220 | Event | `POWER_STOPPED` | — | `Stopped` |

| Status | Meaning |
|--------|---------|
| `STATUS_UNSUPPORTED` | This hardware has no power measurement. |
| `STATUS_INVALID_ARGUMENT` | Bad conversion time, averaging count or range, or a sample period below `min_sample_period_us`. |
| `STATUS_BUSY` | `POWER_START` while measuring; send `POWER_STOP` first. |
| `STATUS_ERROR` | The sensors could not be set up. |

`POWER_STOP` returns OK when nothing is being measured, with the totals of the
last measurement.

## Starting and stopping

The sample period is `conversion_us x averages`, both INA226 settings. The
conversion time must be one that `POWER_INFO` lists, and so must the
averaging count. The period must not be shorter than `min_sample_period_us`,
which is set by how many register reads each sample needs on the 400 kHz I2C
bus.

`lowest_range` is the floor for stepping down. LOW lets the range move freely.
MID or HIGH are for zeroing, or for avoiding LOW's long settling time after a
step down. With `duration_ms` 0, measurement runs until `POWER_STOP` or until
the connection closes.

Measurement ends when:

- the host sends `POWER_STOP` (a response, no event);
- the connection closes (no event);
- the device stops on its own: the duration elapsed, `VPP_INPUT` is missing or
  out of range, a sensor stopped answering, or the range latches misbehaved. It
  then sends `POWER_STOPPED` with the reason.

While nothing is measured, both bypasses stay on, so the target sees only the
50 mΩ shunt.

## Data frames

`POWER_DATA` frames use `KIND_LOG_STREAM` with request ID 0. Like UART and RTT
logs, they go through the transport's bounded, low-priority queue: responses
and events are always sent first, and a frame that does not fit is dropped
instead of blocking debug traffic. A frame is at most a log frame (1028-byte
payload), and the device sends one at least every 20 ms so the host can show
data live.

```text
power_data_t   seq, t_us, run_count
power_run_t    range, encoding, count, settling, t_first_us, t_last_us
               count x power_raw_sample_t   (encoding RAW)
power_run_t    ...
```

- `seq` goes up by one per frame, dropped frames included. A jump tells the
  host which stretch of time is missing.
- A run is back-to-back conversions in one range, with none missed. Its
  samples are evenly spaced from `t_first_us` to `t_last_us`, which are
  microsecond offsets from `t_us`, the device clock.
- Each sample is the average current over its conversion (the INA226
  integrates), stamped at the end of the conversion.
- A range change, a missed conversion or a full frame starts a new run. A gap
  of more than 1.5 sample periods between runs is time that was not measured.
  The conversion that spans a range change is always discarded.
- `settling` counts the samples at the start of a run that follow a step down.
  They are real current drawn from `VPP_INPUT`, output capacitor recharge
  included, so the total charge stays right, but they are not the target's
  current at that moment.
- `encoding` is RAW only for now: 4 bytes per sample, the shunt register
  (int16) and the `VPP_INPUT` bus register (uint16). A delta encoding would get
  a new `encoding` value.

## Damaged frames

A host drops any frame that fails its CRC, size or version check and keeps the
connection, as SIDP requires. For data frames the `seq` jump shows what was
lost. Rev 8 firmware sends roughly one damaged POWER_DATA frame per 1400 at a
588 µs sample period, more at faster settings; the cause on the device is not
found.

## Firmware behaviour (Rev 8)

Not part of the wire contract, but useful when reading data:

- The INA226s convert continuously, never triggered, so there is no dead time.
- The I2C bus runs at 1 MHz (above the parts' rating; verified on one board).
- Comparators move the range up in hardware within about a microsecond. The
  firmware steps down when the average of 8 samples is below 85% of the next
  range's lowest possible trip current, and not within 200 ms of an up-trip.

## Debugging and programming

Measurement runs on its own core and I2C bus, so it neither blocks nor is
blocked by SWD debugging or a programming job. On the host, a debug session on
the same connection must hand `POWER_DATA` and `POWER_STOPPED` to whoever is
measuring, or discard them; it must not treat them as protocol errors.
