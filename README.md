# LogMerger

High-performance C++ GNSS log merger for NovAtel OEM7 and Unicore N4 ASCII logs.

## Features

- Merge a large GNSS log (`input.log`) with one auxiliary log (`aux.log`).
- The auxiliary file can contain any combination of:
  - INSPVA
  - NovAtel EPH / ION messages
  - Unicore N4 EPH / ION messages
- Streaming processing suitable for multi-GB logs.
- CRC validation before using timestamps.
- GPST based time matching.

## Usage

```text
merge_aux_into_input.exe input.log aux.log output.log [tolerance_us]
```

Example:

```text
merge_aux_into_input.exe raw.log aux.log raw_merge.log
```

The auxiliary records are inserted before the first valid target record of the same epoch:

- RANGE
- BESTPOS
- BESTVEL
- PSRVEL
- PSRPOS

## Supported auxiliary messages

### NovAtel OEM7

- INSPVA / INSPVAA
- GLOEPHEMERISA
- QZSSEPHEMERISA
- GALEPHEMERISA
- GPSEPHEMA
- BD2EPHEMA
- IONUTCA
- BD2IONUTCA

### Unicore N4

- GPSION
- BD3ION
- BDSION
- GALION
- GPSEPH
- QZSSEPH
- BD3EPH
- BDSEPH
- GLOEPH
- GALEPH
- IRNSSEPH

## Build

Windows + Visual Studio:

```bat
scripts\build_msvc.bat
```

## Design

The program does not load the complete input file into memory. It uses sequential reading and time ordered merging, making it suitable for 10GB+ receiver logs.

Malformed or CRC-invalid auxiliary messages are silently ignored.
