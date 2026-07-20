#!/usr/bin/env python3
"""Local CLI, stream-integrity, padding, and signal regression tests."""

from __future__ import annotations

import hashlib
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


FRAME_SAMPLES = 52_736
FRAME_BYTES = FRAME_SAMPLES * 8


def null_packet() -> bytes:
    return bytes((0x47, 0x1F, 0xFF, 0x10)) + bytes((0xFF,)) * 184


def checked_run(arguments: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(arguments, check=False, **kwargs)
    if result.returncode != 0:
        stderr = result.stderr.decode(errors="replace") if result.stderr else ""
        raise RuntimeError(
            f"command failed with {result.returncode}: {' '.join(arguments)}\n{stderr}"
        )
    return result


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} /path/to/dvbt2mod", file=sys.stderr)
        return 2
    executable = str(Path(sys.argv[1]).resolve())

    with tempfile.TemporaryDirectory(prefix="dvbt2mod-cli-") as temp:
        directory = Path(temp)
        source = directory / "one-packet.ts"
        output = directory / "one-packet.cf32"
        packet = null_packet()
        source.write_bytes(packet)

        checked_run(
            [executable, str(source), str(output), "--profile", "0"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if output.stat().st_size != FRAME_BYTES:
            raise RuntimeError("finite-file null padding did not produce one full frame")

        piped = checked_run(
            [executable, "-", "-", "--profile", "0"],
            input=packet,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if len(piped.stdout) != FRAME_BYTES:
            raise RuntimeError("stdin/stdout conversion did not produce one full frame")

        malformed = directory / "malformed.ts"
        packets = bytearray(packet * 200)
        packets[91 * 188] = 0
        malformed.write_bytes(packets)
        protected_output = directory / "bad.cf32"
        protected_output.write_bytes(b"KEEP-ME")
        bad = subprocess.run(
            [
                executable,
                str(malformed),
                str(protected_output),
                "--profile",
                "0",
                "--overwrite",
                "1",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if bad.returncode == 0 or b"sync" not in bad.stderr:
            raise RuntimeError("late MPEG-TS sync corruption was not rejected")
        if protected_output.read_bytes() != b"KEEP-ME":
            raise RuntimeError("bad regular input truncated an existing output")

        partial = subprocess.run(
            [executable, "-", str(directory / "partial.cf32"), "--profile", "0"],
            input=packet[:-1],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if partial.returncode == 0 or b"incomplete 188-byte" not in partial.stderr:
            raise RuntimeError("partial stdin MPEG-TS packet was not rejected")

        before = hashlib.sha256(source.read_bytes()).digest()
        with source.open("rb") as stdin:
            same = subprocess.run(
                [
                    executable,
                    "-",
                    str(source),
                    "--profile",
                    "0",
                    "--overwrite",
                    "1",
                ],
                stdin=stdin,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        if same.returncode == 0 or hashlib.sha256(source.read_bytes()).digest() != before:
            raise RuntimeError("stdin/output same-file protection failed")

        idle = subprocess.Popen(
            [
                executable,
                "-",
                str(directory / "idle.cf32"),
                "--profile",
                "0",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        time.sleep(0.15)
        idle.send_signal(signal.SIGINT)
        idle.wait(timeout=3)
        if idle.returncode != 128 + signal.SIGINT:
            raise RuntimeError(f"idle-stdin SIGINT returned {idle.returncode}")
        if idle.stdin:
            idle.stdin.close()
        if idle.stderr:
            idle.stderr.close()

        blocked = subprocess.Popen(
            [executable, str(source), "-", "--profile", "0"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        time.sleep(0.15)
        blocked.send_signal(signal.SIGINT)
        blocked.wait(timeout=3)
        if blocked.returncode != 128 + signal.SIGINT:
            raise RuntimeError(f"blocked-stdout SIGINT returned {blocked.returncode}")
        blocked_bytes = blocked.stdout.read() if blocked.stdout else b""
        blocked_log = blocked.stderr.read() if blocked.stderr else b""
        summary = re.search(
            rb"complex_samples=(\d+).*output_bytes=(\d+)", blocked_log
        )
        if not summary:
            raise RuntimeError("blocked-stdout run did not report output counters")
        reported_samples, reported_bytes = map(int, summary.groups())
        if reported_bytes != len(blocked_bytes) or reported_samples != len(blocked_bytes) // 8:
            raise RuntimeError("blocked-stdout counters do not match delivered bytes")

        broken = subprocess.Popen(
            [executable, str(source), "-", "--profile", "0"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if broken.stdout:
            broken.stdout.close()
        broken.wait(timeout=3)
        broken_log = broken.stderr.read() if broken.stderr else b""
        if broken.returncode == 0 or b"write(output)" not in broken_log:
            raise RuntimeError("closed stdout was not reported without hanging")

        invalid_rate = subprocess.run(
            [
                executable,
                str(source),
                str(directory / "invalid.cf32"),
                "--profile",
                "9",
                "--frame-size",
                "16200",
                "--rate-num",
                "1",
                "--rate-den",
                "3",
                "--check",
                "1",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if invalid_rate.returncode == 0:
            raise RuntimeError("reserved DVB-T2 v1.1.1 code rate was accepted")

        help_as_value = subprocess.run(
            [executable, str(source), str(output), "--fft", "--help"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if help_as_value.returncode == 0 or b"base-10 integer" not in help_as_value.stderr:
            raise RuntimeError("--help was accepted in place of a numeric value")

    print("PASS: CLI padding, TS integrity, overwrite, and signal checks")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"cli_test: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1) from error
