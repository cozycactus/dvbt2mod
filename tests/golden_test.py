#!/usr/bin/env python3
"""Compare dvbt2mod with GNU Radio 3.10.12's official DVB-T2 QA vector."""

from __future__ import annotations

import hashlib
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from urllib.request import urlopen


BASE_URL = (
    "https://raw.githubusercontent.com/gnuradio/gnuradio/"
    "v3.10.12.0/gr-dtv/python/dtv/"
)
FIXTURES = {
    "vv009-4kfft.ts": "8592dea053844bc7bcda1a5a2a48ee896ef930b1c4c375d7cba61cf11d73299f",
    "vv009-4kfft.cfile": "b934a6cb653c21d40beb5d48b4a81ecac8a2ab14a4f3722de79839ab513b85b2",
}
EXPECTED_BYTES = 421_888
FLOAT_TOLERANCE = 1e-5


def download(name: str, destination: Path) -> None:
    with urlopen(BASE_URL + name, timeout=30) as response:  # noqa: S310
        data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != FIXTURES[name]:
        raise RuntimeError(f"{name}: SHA-256 {digest}, expected {FIXTURES[name]}")
    destination.write_bytes(data)


def compare_floats(expected_path: Path, actual_path: Path) -> tuple[int, float]:
    expected = expected_path.read_bytes()
    actual = actual_path.read_bytes()
    if len(expected) != EXPECTED_BYTES or len(actual) != EXPECTED_BYTES:
        raise RuntimeError(
            f"size mismatch: expected fixture={len(expected)}, actual={len(actual)}, "
            f"required={EXPECTED_BYTES}"
        )

    maximum = 0.0
    count = 0
    for (expected_value,), (actual_value,) in zip(
        struct.iter_unpack("<f", expected), struct.iter_unpack("<f", actual)
    ):
        if not math.isfinite(actual_value):
            raise RuntimeError(f"non-finite output float at index {count}")
        maximum = max(maximum, abs(expected_value - actual_value))
        count += 1
    if maximum > FLOAT_TOLERANCE:
        raise RuntimeError(
            f"maximum float error {maximum:.9g} exceeds {FLOAT_TOLERANCE}"
        )
    return count, maximum


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} /path/to/dvbt2mod", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1]).resolve()
    if not executable.is_file():
        raise RuntimeError(f"executable not found: {executable}")

    with tempfile.TemporaryDirectory(prefix="dvbt2mod-golden-") as temp:
        directory = Path(temp)
        source = directory / "vv009-4kfft.ts"
        expected = directory / "vv009-4kfft.cfile"
        actual = directory / "actual.cfile"
        download(source.name, source)
        download(expected.name, expected)
        subprocess.run(
            [
                str(executable),
                str(source),
                str(actual),
                "--profile",
                "0",
                "--samples",
                str(EXPECTED_BYTES // 8),
            ],
            check=True,
        )
        count, maximum = compare_floats(expected, actual)

    print(
        f"PASS: {count // 2} complex samples, {count} floats, "
        f"max_abs_error={maximum:.9g}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"golden_test: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1) from error
