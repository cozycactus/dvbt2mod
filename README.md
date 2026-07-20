<p align="center">
  <img src="docs/banner.svg" alt="dvbt2mod — MPEG-TS to DVB-T2 CF32 IQ" width="100%">
</p>

<p align="center">
  <a href="https://github.com/cozycactus/dvbt2mod/actions/workflows/windows.yml"><img src="https://github.com/cozycactus/dvbt2mod/actions/workflows/windows.yml/badge.svg" alt="Windows x64"></a>
  <a href="https://github.com/cozycactus/dvbt2mod/releases/latest"><img src="https://img.shields.io/github/v/release/cozycactus/dvbt2mod?display_name=tag&sort=semver" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/cozycactus/dvbt2mod" alt="GPL-3.0-or-later"></a>
</p>

<p align="center">
  A lightweight native C++ command-line DVB-T2 baseband modulator.<br>
  188-byte MPEG-TS in; headerless CF32LE I/Q out; numeric launch parameters.
</p>

<p align="center"><a href="docs/README.ru.md">Русская документация</a></p>

## Why dvbt2mod?

`dvbt2mod` turns GNU Radio's complete `gr-dtv` DVB-T2 transmit chain into a
small, script-friendly executable. It does not require Python, GNU Radio
Companion, Qt, UHD, SoapySDR, or an SDR device at runtime.

- Physical numeric arguments instead of GNU Radio's internal enum values.
- Presets based on GNU Radio's official DVB-T2 flowgraphs and QA vector.
- Validation of FFT/GI/pilot, code-rate, frame-duration, and FEC-capacity
  combinations before modulation starts.
- Regular-file preflight before an output is created or replaced.
- Canonical MPEG-TS null-packet padding to complete the final DVB-T2 frame.
- Binary stdin/stdout streaming, clean Ctrl+C shutdown, and Unicode paths on
  Windows.
- Automated, auditable Windows x64 build, portable ZIP, checksums, dependency list,
  and third-party licences.

## Windows x64

Download `dvbt2mod-0.1.0-windows-x64.zip` from the
[latest release](https://github.com/cozycactus/dvbt2mod/releases/latest),
unpack it, and keep the included DLLs beside `dvbt2mod.exe`. Neither MSYS2 nor
an installed copy of GNU Radio is required for the portable package.

```powershell
.\dvbt2mod.exe input.ts output.cf32 --profile 1
```

The executable is currently unsigned, so Windows SmartScreen may show its
standard warning. Verify the downloaded ZIP against the accompanying
`.zip.sha256` file:

```powershell
(Get-FileHash .\dvbt2mod-0.1.0-windows-x64.zip -Algorithm SHA256).Hash.ToLower()
```

The release also carries a separately checksummed
`dvbt2mod-0.1.0-windows-x64-corresponding-source.zip` containing the exact
project revision and complete MSYS2 source archives for every bundled runtime
DLL package.

## Quick start

Profile 1 is the default full-size 32K/256-QAM configuration:

```console
dvbt2mod input.ts output.cf32 --profile 1
```

An existing output is protected unless overwrite is explicitly enabled with a
numeric value:

```console
dvbt2mod input.ts output.cf32 --profile 1 --overwrite 1
```

Profile 9 is a faster 4K configuration suitable for a quick smoke test:

```console
dvbt2mod input.ts output.cf32 --profile 9
```

Use `-` for stdin or stdout. Diagnostics always go to stderr:

```console
dvbt2mod - - --profile 9 < input.ts > output.cf32
```

All modulation options accept base-10 numbers. To expand profile 9 explicitly:

```console
dvbt2mod input.ts output.cf32 \
  --profile 9 \
  --bandwidth-khz 8000 \
  --fft 4096 \
  --t2gi 0 \
  --qam 64 \
  --rate-num 2 --rate-den 3 \
  --guard-num 1 --guard-den 32 \
  --pilot 7 \
  --fec-blocks 31 \
  --ti-blocks 3 \
  --data-symbols 100 \
  --frame-size 64800 \
  --l1-qam 16 \
  --t2-frames 2 \
  --rotation 1 \
  --extended 0 \
  --high-efficiency 0 \
  --papr-tr 0 \
  --repeat 0 \
  --overwrite 0
```

Run `dvbt2mod --help` for the complete option reference. Use `--check 1` to
validate the values and construct the graph without running modulation.

## Profiles

| ID | Upstream basis | FFT | QAM | Code rate | GI | PP |
|---:|---|---:|---:|---:|---:|---:|
| 0 | GNU Radio `qa_dtv.py` golden vector | 4096 | 64 | 2/3 | 1/32 | 7 |
| 1 | GNU Radio `vv001-cr35` | 32768 T2-GI | 256 | 3/5 | 1/128 | 7 |
| 9 | GNU Radio `vv009-4kfft` | 4096 | 64 | 2/3 | 1/32 | 7 |

Profiles 1 and 9 reproduce GNU Radio 3.10.12 reference flowgraphs. Profile 0
reproduces the compact upstream regression vector and enables PAPR Tone
Reservation for deterministic testing.

## I/O contract

Input must contain complete 188-byte MPEG-TS packets. Named input files are
fully checked for sync bytes before the output is opened. Stdin is validated as
it arrives. A finite input is completed with PID `0x1FFF` null packets and only
the synthetic last packet is cut at the exact BB/T2-frame boundary.

Output is interleaved little-endian `float32 I, float32 Q`: eight bytes per
complex sample, with no header or metadata. The I/Q rate is `bandwidth * 8 / 7`
for 5/6/7/8/10 MHz channels and `131e6 / 71` for 1.7 MHz. File output runs as
fast as the CPU permits; it is not paced as a real-time RF sink.

`--samples N` is a low-level raw-IQ capture limit and may cut a DVB-T2 frame.
For a whole-frame capture, use a normal finite run or a multiple of the reported
frame size (profile 1: 1,983,488; profile 9: 441,344 complex samples).

## Build from source

The build requires CMake 3.20+, a C++17 compiler, and GNU Radio 3.10.x with
`gr-dtv`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows, build in the MSYS2 UCRT64 shell with the UCRT64 GCC, CMake, Ninja,
Python, and GNU Radio packages. The exact CI recipe is in
[`windows.yml`](.github/workflows/windows.yml), and
[`package_windows.sh`](scripts/package_windows.sh) assembles and audits the
portable runtime.

The executable itself is deliberately thin and dynamically linked to GNU Radio.
The portable Windows ZIP adds its exact transitive runtime DLL set; a normal
source build expects ABI-compatible GNU Radio libraries on the host.

## Verification

```sh
ctest --test-dir build --output-on-failure
python3 tests/golden_test.py build/dvbt2mod
```

The golden test downloads two pinned fixtures from GNU Radio `v3.10.12.0`,
checks their SHA-256 values, and compares every output float. The reference
result is 52,736 complex samples / 421,888 bytes with a maximum absolute error
of `5.96046448e-7` at a tolerance of `1e-5`.

## Scope

The current implementation targets DVB-T2 v1.1.1, SISO, one PLP, one RF, and
PAPR off or Tone Reservation. MISO, T2-Lite, multiple PLPs/RFs, mux-rate
conversion, and an SDR hardware sink are outside its present scope.

## References

- [GNU Radio DVB-T2 Modulator block documentation](https://wiki.gnuradio.org/index.php?title=DVB-T2_Modulator)
- [GNU Radio 3.10.12 DVB-T2 QA vector and reference graph](https://github.com/gnuradio/gnuradio/blob/v3.10.12.0/gr-dtv/python/dtv/qa_dtv.py#L28-L158)
- [ETSI EN 302 755 V1.1.1](https://www.etsi.org/deliver/etsi_EN/302700_302799/302755/01.01.01_60/EN_302755v010101p.pdf)

## Licence

`dvbt2mod` is licensed under
[GPL-3.0-or-later](LICENSE), matching the GNU Radio DVB-T2 blocks it uses. The
Windows bundle records the exact runtime packages, bundled files, licences,
checksums, and build revision; the release includes the complete corresponding
source archive separately so the runtime download stays compact.
