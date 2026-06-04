#!/usr/bin/env python3
"""Repo-local verification for issue005 Android download bridge semantics.

Mirrors the relevant logic from:
- firmware/target/hosted/android/request-android.c finalize_download_result()
- android/src/org/rockbox/Helper/Connectivity.java performSynchronousDownload()
"""

from __future__ import annotations

import os
import tempfile
from dataclasses import dataclass
from pathlib import Path

ANDROID_REQUEST_OK = 0
ANDROID_REQUEST_JNI_EXCEPTION = -4
ANDROID_REQUEST_TRUNCATED = -5


@dataclass
class ShellResult:
    exit_code: int = 0
    timed_out: bool = False
    stdout_truncated: bool = False
    stderr_truncated: bool = False
    stdout: str = ""
    stderr: str = ""


def copy_to_buffer(dst_len: int, src: str | None):
    src = "" if src is None else src
    if len(src) >= dst_len:
        return src[: dst_len - 1], ANDROID_REQUEST_TRUNCATED
    return src, ANDROID_REQUEST_OK


def finalize_download_result(status_text: str | None, error_text: str | None, error_len: int = 128):
    status_out = int(status_text) if status_text is not None else 0
    rc = ANDROID_REQUEST_JNI_EXCEPTION if error_text else ANDROID_REQUEST_OK
    error_buf, error_rc = copy_to_buffer(error_len, error_text)
    if error_rc == ANDROID_REQUEST_TRUNCATED:
        rc = ANDROID_REQUEST_TRUNCATED
    return rc, status_out, error_buf


def summarize_helper_failure(detail: str, fallback: str):
    normalized = (detail or "").strip().lower()
    if not normalized:
        return fallback
    if "timed out" in normalized or "timeout" in normalized:
        return "request timed out"
    if "no such host" in normalized or "unknown host" in normalized or "resolve" in normalized:
        return "dns resolution failed"
    return fallback


def delete_quietly(path: Path | None):
    if path and path.exists():
        path.unlink()


def move_completed_download_into_place(
    temp_file: Path | None,
    destination: Path,
    simulate_link_failure: bool = False,
    create_destination_during_finalize: bool = False,
):
    if temp_file is None or not temp_file.exists():
        return "filesystem error: temporary download file missing after helper exit"

    if create_destination_during_finalize:
        destination.write_text("raced")

    try:
        if simulate_link_failure:
            raise OSError("simulated link failure")
        os.link(temp_file, destination)
    except FileExistsError:
        delete_quietly(temp_file)
        return "filesystem error: destination already exists"
    except OSError:
        delete_quietly(temp_file)
        return "filesystem error: failed to move completed download into place"

    delete_quietly(temp_file)
    return ""


def perform_synchronous_download_simulated(root: Path, shell_result: ShellResult, status_text: str,
                                           create_temp_file: bool = True,
                                           simulate_link_failure: bool = False,
                                           precreate_destination_before_move: bool = False,
                                           create_destination_during_finalize: bool = False):
    root.mkdir(parents=True, exist_ok=True)
    destination = root / "dest.bin"
    temp_file = root / ".download-dest.bin.part"
    if create_temp_file:
        temp_file.write_bytes(b"payload")

    if shell_result.timed_out:
        delete_quietly(temp_file)
        return ["0", "gocurl download timed out after 30 seconds"], destination, temp_file
    if shell_result.stdout_truncated or shell_result.stderr_truncated:
        delete_quietly(temp_file)
        return ["0", "gocurl download status exceeded Java capture limit"], destination, temp_file

    status = int(status_text) if status_text.strip() else 0
    if shell_result.exit_code != 0:
        delete_quietly(temp_file)
        error_text = "gocurl download failed: " + summarize_helper_failure(shell_result.stderr, "helper process failed")
        error_text += f" (exit {shell_result.exit_code})"
        return [str(status), error_text], destination, temp_file
    if status < 200 or status >= 300:
        delete_quietly(temp_file)
        return [str(status), f"download not saved: HTTP status {status}"], destination, temp_file

    if precreate_destination_before_move:
        destination.write_text("raced")

    error_text = move_completed_download_into_place(
        temp_file,
        destination,
        simulate_link_failure=simulate_link_failure,
        create_destination_during_finalize=create_destination_during_finalize,
    )
    if error_text:
        return [str(status), error_text], destination, temp_file
    return [str(status), ""], destination, temp_file


FINALIZE_CASES = [
    ("HTTP 200 download success stays success", ("200", ""), ANDROID_REQUEST_OK, 200, ""),
    ("HTTP 404 download becomes caller-visible failure", ("404", "download not saved: HTTP status 404"), ANDROID_REQUEST_JNI_EXCEPTION, 404, "download not saved: HTTP status 404"),
    ("helper process failure stays failure", ("0", "gocurl download failed: dns resolution failed (exit 6)"), ANDROID_REQUEST_JNI_EXCEPTION, 0, "gocurl download failed: dns resolution failed (exit 6)"),
]


def main():
    failures = []

    for name, args, expect_rc, expect_status, expect_error in FINALIZE_CASES:
        rc, status, error = finalize_download_result(*args)
        print(f"{name}: rc={rc} status={status} error={error!r}")
        if (rc, status, error) != (expect_rc, expect_status, expect_error):
            failures.append(name)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        result, destination, temp_file = perform_synchronous_download_simulated(
            root / "timeout", ShellResult(timed_out=True), "0"
        )
        print(f"timeout cleanup: result={result} temp_exists={temp_file.exists()} dest_exists={destination.exists()}")
        if temp_file.exists() or destination.exists() or result[1] != "gocurl download timed out after 30 seconds":
            failures.append("timeout cleanup")

        (root / "process").mkdir()
        result, destination, temp_file = perform_synchronous_download_simulated(
            root / "process", ShellResult(exit_code=6, stderr="Could not resolve host"), "0"
        )
        print(f"process cleanup: result={result} temp_exists={temp_file.exists()} dest_exists={destination.exists()}")
        if temp_file.exists() or destination.exists() or "dns resolution failed" not in result[1]:
            failures.append("process cleanup")

        (root / "http").mkdir()
        result, destination, temp_file = perform_synchronous_download_simulated(
            root / "http", ShellResult(exit_code=0), "404"
        )
        print(f"HTTP failure cleanup: result={result} temp_exists={temp_file.exists()} dest_exists={destination.exists()}")
        if temp_file.exists() or destination.exists() or result != ["404", "download not saved: HTTP status 404"]:
            failures.append("HTTP failure cleanup")

        (root / "linkfail").mkdir()
        result, destination, temp_file = perform_synchronous_download_simulated(
            root / "linkfail", ShellResult(exit_code=0), "200", simulate_link_failure=True
        )
        print(f"link failure blocks success: result={result} temp_exists={temp_file.exists()} dest_exists={destination.exists()}")
        if temp_file.exists() or destination.exists() or result != ["200", "filesystem error: failed to move completed download into place"]:
            failures.append("link failure blocks success")

        (root / "race").mkdir()
        result, destination, temp_file = perform_synchronous_download_simulated(
            root / "race", ShellResult(exit_code=0), "200", precreate_destination_before_move=True
        )
        print(f"pre-existing destination blocks overwrite: result={result} temp_exists={temp_file.exists()} dest_exists={destination.exists()}")
        if temp_file.exists() or not destination.exists() or result != ["200", "filesystem error: destination already exists"]:
            failures.append("pre-existing destination blocks overwrite")

        (root / "race-during-finalize").mkdir()
        result, destination, temp_file = perform_synchronous_download_simulated(
            root / "race-during-finalize", ShellResult(exit_code=0), "200", create_destination_during_finalize=True
        )
        print(f"destination race during finalize blocks overwrite: result={result} temp_exists={temp_file.exists()} dest_exists={destination.exists()} contents={destination.read_text()!r}")
        if temp_file.exists() or not destination.exists() or destination.read_text() != "raced" or result != ["200", "filesystem error: destination already exists"]:
            failures.append("destination race during finalize blocks overwrite")

        (root / "symlink").mkdir()
        symlink_target = root / "symlink-target.txt"
        symlink_target.write_text("protected")
        symlink_destination = root / "symlink" / "dest.bin"
        symlink_destination.symlink_to(symlink_target)
        symlink_temp = root / "symlink" / ".download-dest.bin.part"
        symlink_temp.write_bytes(b"payload")
        symlink_error = move_completed_download_into_place(symlink_temp, symlink_destination)
        print(f"symlink destination blocks overwrite: error={symlink_error!r} temp_exists={symlink_temp.exists()} link_exists={symlink_destination.exists()} target_contents={symlink_target.read_text()!r}")
        if symlink_temp.exists() or not symlink_destination.exists() or symlink_target.read_text() != "protected" or symlink_error != "filesystem error: destination already exists":
            failures.append("symlink destination blocks overwrite")

        (root / "success").mkdir()
        result, destination, temp_file = perform_synchronous_download_simulated(
            root / "success", ShellResult(exit_code=0), "200"
        )
        print(f"success after final move: result={result} temp_exists={temp_file.exists()} dest_exists={destination.exists()} dest_bytes={destination.read_bytes()!r}")
        if temp_file.exists() or not destination.exists() or destination.read_bytes() != b"payload" or result != ["200", ""]:
            failures.append("success after final move")

    if failures:
        print("FAIL:", ", ".join(failures))
        raise SystemExit(1)

    print("PASS: issue005 download bridge cases")


if __name__ == "__main__":
    main()
