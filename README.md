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
merge_aux_into_input.exe input.log aux.log [output.log [tolerance_us]]
```

Example:

```text
merge_aux_into_input.exe input.log aux.log
merge_aux_into_input.exe raw.log aux.log raw_merge.log
```

The first example produces `input-aux.log`; the second produces the explicitly named `raw_merge.log`.

If `output.log` is omitted, the executable automatically writes to
`<input-stem>-<aux-stem><input-extension>` in the **input log's directory**.
For example, `C:\data\range.log` and `D:\nav\eph.log` produce
`C:\data\range-eph.log`. The input file's extension is preserved,
even if the auxiliary file uses a different extension. An explicit output
path continues to take precedence. The optional `tolerance_us` argument
is supported when an explicit output path is provided.

The eligible target records are:

- RANGE
- BESTPOS
- BESTVEL
- PSRVEL
- PSRPOS

Insertion behavior differs by auxiliary record type:

- `INSPVA` is epoch-matched to the first valid target within `tolerance_us`.
- EPH/ION records are state-bearing navigation data. A CRC-valid EPH/ION record that is earlier than the next valid target is preserved and inserted before that target instead of being discarded. Records at the target epoch (or within `tolerance_us`) are also inserted before the target.
- This rule also applies to the first target in the input file, so EPH/ION records whose header GPST is earlier than the first RANGE/other eligible target are retained.

## Supported auxiliary messages

### NovAtel OEM7

- INSPVA / INSPVAA
- GLOEPHEMERISA
- QZSSEPHEMERISA
- GALEPHEMERISA
- GPSEPHEMA
- GPSL1CEPHEMA
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
