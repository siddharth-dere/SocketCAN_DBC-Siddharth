# AI-Assisted DBC Generation & Engineering Validation Report

## 1. Purpose

This report documents a model-assisted engineering workflow for constructing a small automotive CAN network using a Vector DBC file, Linux SocketCAN, and a Virtual CAN interface named `vcan0`.

The six required telemetry signals are distributed across three standard 11-bit CAN identifiers:

| CAN ID | Message | Signals |
|---|---|---|
| `0x100` / `256` | `VehicleStatus` | `VehicleSpeed`, `EngineRPM` |
| `0x101` / `257` | `ThermalFuel` | `CoolantTemperature`, `FuelLevel` |
| `0x102` / `258` | `PowerStatus` | `BatteryVoltage`, `AmbientTemperature` |

The engineering goal is not merely to produce text that looks plausible. The DBC, encoder, and decoder must agree at the bit level.

## 2. AI Tool Selection & Rationale

A practical workflow may use Claude 3.5 Sonnet and/or ChatGPT-4o for:

1. generating an initial DBC structure;
2. proposing modular C abstractions;
3. generating documentation;
4. explaining SocketCAN APIs;
5. identifying inconsistencies found during human review.

The important verification principle is that AI-generated code is treated as a draft, not as authority. The final result must be checked against the DBC specification and tested using actual CAN frames.

> Historical-use note: this report describes a reproducible engineering workflow. It should not be presented as a transcript of tool calls that did not actually occur.

## 3. Multi-Stage Prompt Engineering Strategy

### Stage A — DBC specification prompt

The specification should explicitly provide:

- message names;
- hexadecimal and decimal CAN IDs;
- DLC;
- signal start bits;
- signal lengths;
- little-endian byte order;
- factors;
- offsets;
- engineering ranges;
- units.

The output should then be manually inspected before being copied into `vehicle.dbc`.

### Stage B — SocketCAN implementation prompt

The prompt should require:

- `PF_CAN`;
- `SOCK_RAW`;
- `CAN_RAW`;
- interface lookup using `SIOCGIFINDEX`;
- `bind()` to `vcan0`;
- standard 11-bit identifiers;
- eight-byte frames;
- explicit little-endian packing;
- inverse decoder equations.

### Stage C — validation prompt

The model can be asked to calculate raw values for boundary and midpoint examples.

Examples:

- `VehicleSpeed = 100.00 km/h` → raw `10000`;
- `EngineRPM = 3000 rpm` → raw `3000`;
- `CoolantTemperature = 80 degC` → raw `(80 - (-40))/0.1 = 1200`;
- `FuelLevel = 50%` → raw `50/0.5 = 100`;
- `BatteryVoltage = 14.20 V` → raw `14.20/0.01 = 1420`;
- `AmbientTemperature = 25 degC` → raw `25 - (-40) = 65`.

## 4. Raw AI Output Review & Identified Failure Modes

Typical failure modes in this class of task include:

### 4.1 Hexadecimal versus decimal DBC identifiers

The DBC `BO_` syntax here uses decimal identifiers. Therefore:

- `0x100` = `256`;
- `0x101` = `257`;
- `0x102` = `258`.

A generated DBC that places `0x100` directly into `BO_` is not following the requested convention.

### 4.2 Incorrect little-endian interpretation

For the selected layout:

- start bit `0`, length `16` occupies bytes `0` and `1`;
- start bit `16`, length `16` occupies bytes `2` and `3`;
- start bit `16`, length `8` occupies byte `2`.

Therefore the C implementation writes each 16-bit field in low-byte-first order.

### 4.3 Incorrect temperature offsets

The decoding equation is:

`Physical = Raw × Factor + Offset`

For coolant:

`Coolant = Raw × 0.1 - 40`

Therefore:

`Raw = (Coolant + 40) / 0.1`

For ambient temperature:

`Ambient = Raw - 40`

Therefore:

`Raw = Ambient + 40`

The sign of the offset must not be lost when converting between raw and physical values.

### 4.4 Range and raw-value mismatch

The factor and offset determine the raw range. For example:

`CoolantTemperature = -40 degC` → raw `0`

`CoolantTemperature = 120 degC` → raw `1600`

The DBC range and C clamping must reflect the same physical limits.

## 5. Manual Mathematical Verification

### Vehicle Speed

Factor = `0.01`, offset = `0`.

`Raw = Physical / 0.01`

For `72.50 km/h`:

`Raw = 72.50 / 0.01 = 7250`

Decode:

`7250 × 0.01 = 72.50 km/h`

### Engine RPM

Factor = `1`, offset = `0`.

For `3200 rpm`:

`Raw = 3200`

Decode:

`3200 × 1 = 3200 rpm`

### Coolant Temperature

Factor = `0.1`, offset = `-40`.

For `85 degC`:

`Raw = (85 - (-40))/0.1 = 1250`

Decode:

`1250 × 0.1 - 40 = 85 degC`

### Fuel Level

Factor = `0.5`, offset = `0`.

For `63%`:

`Raw = 63/0.5 = 126`

Decode:

`126 × 0.5 = 63%`

### Battery Voltage

Factor = `0.01`, offset = `0`.

For `13.80 V`:

`Raw = 1380`

Decode:

`1380 × 0.01 = 13.80 V`

### Ambient Temperature

Factor = `1`, offset = `-40`.

For `30 degC`:

`Raw = 30 - (-40) = 70`

Decode:

`70 - 40 = 30 degC`

## 6. Human Verification Checklist

Before publishing the repository, verify:

- every message ID matches between DBC and C;
- every signal's start bit and length match the packing offsets;
- every factor and offset has the same value in DBC and C;
- every DBC physical range matches the intended application range;
- the encoder and decoder are exact inverses;
- `vcan0` exists and is `UP`;
- `candump vcan0` shows frames for all three IDs;
- the dashboard numbers follow the transmitter values.

## 7. Lessons Learned

The most important lesson is that a CAN project can appear correct while still being semantically wrong. A frame can be transmitted successfully even when its byte order, scaling, offset, or identifier interpretation is incorrect.

The reliable workflow is therefore:

**Specification → DBC → Encoder → Raw frame capture → Decoder → Physical-value cross-check**

The human engineer remains responsible for the final validation.
