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
- GPSCNAVEPH
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

The build script can be launched from a normal Command Prompt or PowerShell. It first uses `cl.exe` when it is already available; otherwise it automatically locates Visual Studio through `vswhere.exe`, initializes the x64 MSVC environment with `vcvars64.bat`, and builds `merge_aux_into_input.exe` in the repository root. If `vswhere.exe` is unavailable, the script falls back to the standard Visual Studio 2022/2019/2017 Community, Professional, Enterprise, and Build Tools installation paths.

Visual Studio must include the **Desktop development with C++** workload.

## Design

The program does not load the complete input file into memory. It uses sequential reading and time ordered merging, making it suitable for 10GB+ receiver logs.

Malformed or CRC-invalid auxiliary messages are silently ignored.

## Related GNSS tools

This repository is part of a set of focused public GNSS engineering tools:

- [`gnss-data-simulator`](https://github.com/zengxianghang/gnss-data-simulator) — deterministic GNSS receiver-data simulation and RTKLIB-based validation.
- [`gnss-data-parser`](https://github.com/zengxianghang/gnss-data-parser) — streaming Python/MATLAB parsing and cross-language validation for receiver logs.
- [`FastExtractor`](https://github.com/zengxianghang/FastExtractor) — high-performance GPST-window extraction for large NovAtel/Unicore logs.
- [`RTKLIB`](https://github.com/zengxianghang/RTKLIB) — the RTKLIB fork used by simulator integration and validation work.

## License

LogMerger is licensed under the [MIT License](LICENSE).
