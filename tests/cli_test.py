#!/usr/bin/env python3
"""Local CLI, stream-integrity, padding, and signal regression tests."""

from __future__ import annotations

import hashlib
import re
import signal
import subprocess
import sys
import tempfile
import threading
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


def check_interrupts(executable: str, directory: Path, source: Path) -> None:
    """Exercise interruptible stdin/stdout on POSIX and Windows console groups."""
    creationflags = 0
    interrupt_signal = signal.SIGINT
    expected_returncode = 128 + signal.SIGINT
    signal_name = "SIGINT"
    allocated_console = False

    if sys.platform == "win32":
        # GenerateConsoleCtrlEvent requires caller and child to share a console.
        # A service-hosted CI process may not have one, so attach a private
        # console before creating the child's new process group.
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetConsoleProcessList.argtypes = [
            ctypes.POINTER(wintypes.DWORD),
            wintypes.DWORD,
        ]
        kernel32.GetConsoleProcessList.restype = wintypes.DWORD
        kernel32.AllocConsole.restype = wintypes.BOOL
        kernel32.FreeConsole.restype = wintypes.BOOL
        process_ids = (wintypes.DWORD * 1)()
        if kernel32.GetConsoleProcessList(process_ids, 1) == 0:
            if not kernel32.AllocConsole():
                error = ctypes.get_last_error()
                print(
                    f"SKIP: cannot allocate a console for CTRL_BREAK_EVENT ({error})"
                )
                return
            allocated_console = True
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
        interrupt_signal = signal.CTRL_BREAK_EVENT
        expected_returncode = 128 + signal.SIGBREAK
        signal_name = "SIGBREAK"

    def interrupt_and_wait(process: subprocess.Popen[bytes], label: str) -> bytes:
        if process.stderr is None:
            raise RuntimeError(f"{label} has no stderr readiness stream")

        ready = threading.Event()
        stderr_bytes = bytearray()

        def drain_stderr() -> None:
            assert process.stderr is not None
            for line in iter(process.stderr.readline, b""):
                stderr_bytes.extend(line)
                if line.startswith(b"profile="):
                    ready.set()

        reader = threading.Thread(target=drain_stderr, daemon=True)
        reader.start()

        if not ready.wait(timeout=10):
            if process.poll() is None:
                process.kill()
            process.wait(timeout=3)
            reader.join(timeout=3)
            raise RuntimeError(
                f"{label} did not install its signal handler: "
                f"{bytes(stderr_bytes).decode(errors='replace')}"
            )

        # The handler is installed before the profile line. Give graph startup
        # a short deterministic window so the source/sink reaches blocked I/O.
        time.sleep(0.5)
        try:
            process.send_signal(interrupt_signal)
            process.wait(timeout=8)
        except BaseException:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=3)
            reader.join(timeout=3)
            raise
        reader.join(timeout=3)
        if reader.is_alive():
            raise RuntimeError(f"{label} stderr reader did not finish")
        process.stderr.close()
        if process.returncode != expected_returncode:
            raise RuntimeError(
                f"{label} {signal_name} returned {process.returncode}: "
                f"{bytes(stderr_bytes).decode(errors='replace')}"
            )
        return bytes(stderr_bytes)

    try:
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
            creationflags=creationflags,
        )
        interrupt_and_wait(idle, "idle-stdin")
        if idle.stdin:
            idle.stdin.close()

        blocked = subprocess.Popen(
            [executable, str(source), "-", "--profile", "0"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=creationflags,
        )
        blocked_log = interrupt_and_wait(blocked, "blocked-stdout")
        blocked_bytes = blocked.stdout.read() if blocked.stdout else b""
        summary = re.search(
            rb"complex_samples=(\d+).*output_bytes=(\d+)", blocked_log
        )
        if not summary:
            raise RuntimeError("blocked-stdout run did not report output counters")
        reported_samples, reported_bytes = map(int, summary.groups())
        if (
            reported_bytes != len(blocked_bytes)
            or reported_samples != len(blocked_bytes) // 8
        ):
            raise RuntimeError("blocked-stdout counters do not match delivered bytes")
    finally:
        if allocated_console:
            kernel32.FreeConsole()


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

        unicode_directory = directory / "кириллица"
        unicode_directory.mkdir()
        unicode_source = unicode_directory / "вход.ts"
        unicode_output = unicode_directory / "выход.cf32"
        unicode_source.write_bytes(packet)
        checked_run(
            [
                executable,
                str(unicode_source),
                str(unicode_output),
                "--profile",
                "0",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if unicode_output.stat().st_size != FRAME_BYTES:
            raise RuntimeError("Unicode input/output paths produced a truncated frame")

        with unicode_output.open("ab") as existing_output:
            existing_output.write(b"STALE-TAIL")
        checked_run(
            [
                executable,
                str(unicode_source),
                str(unicode_output),
                "--profile",
                "0",
                "--overwrite",
                "1",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if unicode_output.stat().st_size != FRAME_BYTES:
            raise RuntimeError("overwrite did not truncate the Unicode output safely")

        unicode_before = hashlib.sha256(unicode_source.read_bytes()).digest()
        with unicode_source.open("rb") as stdin:
            unicode_same = subprocess.run(
                [
                    executable,
                    "-",
                    str(unicode_source),
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
        if (
            unicode_same.returncode == 0
            or hashlib.sha256(unicode_source.read_bytes()).digest() != unicode_before
        ):
            raise RuntimeError("Unicode stdin/output same-file protection failed")

        piped = checked_run(
            [executable, "-", "-", "--profile", "0"],
            input=packet,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        expected_stream = output.read_bytes()
        if piped.stdout != expected_stream:
            raise RuntimeError(
                "stdin/stdout conversion produced "
                f"{len(piped.stdout)} bytes, expected {FRAME_BYTES}; "
                f"stderr={piped.stderr.decode(errors='replace')!r}"
            )

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

        check_interrupts(executable, directory, source)

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
