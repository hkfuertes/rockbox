#!/usr/bin/env python3
"""Repo-local verification for issue003 Android bridge return semantics.

This mirrors finalize_request_result() in
firmware/target/hosted/android/request-android.c so the requested scenarios can
be exercised without an Android device or JNI runtime.
"""

ANDROID_REQUEST_OK = 0
ANDROID_REQUEST_JNI_EXCEPTION = -4
ANDROID_REQUEST_TRUNCATED = -5


def copy_to_buffer(dst_len, src):
    src = "" if src is None else src
    if len(src) >= dst_len:
        return src[: dst_len - 1], ANDROID_REQUEST_TRUNCATED
    return src, ANDROID_REQUEST_OK


def finalize_request_result(status_text, response_text, error_text, response_len=64, error_len=128):
    status_out = int(status_text) if status_text is not None else 0
    rc = ANDROID_REQUEST_JNI_EXCEPTION if status_out == 0 and error_text else ANDROID_REQUEST_OK

    response_buf, response_rc = copy_to_buffer(response_len, response_text)
    if response_rc == ANDROID_REQUEST_TRUNCATED:
        rc = ANDROID_REQUEST_TRUNCATED
        if not error_text:
            error_buf, error_rc = copy_to_buffer(error_len, "response truncated to fit caller buffer")
            if error_rc == ANDROID_REQUEST_TRUNCATED:
                rc = ANDROID_REQUEST_TRUNCATED
        else:
            error_buf, error_rc = copy_to_buffer(error_len, error_text)
            if error_rc == ANDROID_REQUEST_TRUNCATED:
                rc = ANDROID_REQUEST_TRUNCATED
    else:
        error_buf, error_rc = copy_to_buffer(error_len, error_text)
        if error_rc == ANDROID_REQUEST_TRUNCATED:
            rc = ANDROID_REQUEST_TRUNCATED

    return rc, status_out, response_buf, error_buf


CASES = [
    {
        "name": "GET 200 success",
        "args": ("200", "ok", ""),
        "expect_rc": ANDROID_REQUEST_OK,
        "expect_status": 200,
        "expect_error": "",
    },
    {
        "name": "HTTP 404 remains success",
        "args": ("404", "missing", "http 404 from server"),
        "expect_rc": ANDROID_REQUEST_OK,
        "expect_status": 404,
        "expect_error": "http 404 from server",
    },
    {
        "name": "HTTP 500 remains success",
        "args": ("500", "upstream failed", "http 500 from server"),
        "expect_rc": ANDROID_REQUEST_OK,
        "expect_status": 500,
        "expect_error": "http 500 from server",
    },
    {
        "name": "timeout becomes bridge failure",
        "args": ("0", "", "gocurl request timed out after 30 seconds"),
        "expect_rc": ANDROID_REQUEST_JNI_EXCEPTION,
        "expect_status": 0,
        "expect_error": "gocurl request timed out after 30 seconds",
    },
    {
        "name": "no output becomes bridge failure",
        "args": ("0", "", "gocurl produced no JSON output (helper process failed)"),
        "expect_rc": ANDROID_REQUEST_JNI_EXCEPTION,
        "expect_status": 0,
        "expect_error": "gocurl produced no JSON output (helper process failed)",
    },
    {
        "name": "malformed JSON becomes bridge failure",
        "args": ("0", "", "gocurl returned malformed JSON"),
        "expect_rc": ANDROID_REQUEST_JNI_EXCEPTION,
        "expect_status": 0,
        "expect_error": "gocurl returned malformed JSON",
    },
    {
        "name": "helper error becomes bridge failure",
        "args": ("0", "", "gocurl request failed: dns resolution failed (exit 6)"),
        "expect_rc": ANDROID_REQUEST_JNI_EXCEPTION,
        "expect_status": 0,
        "expect_error": "gocurl request failed: dns resolution failed (exit 6)",
    },
    {
        "name": "Java capture truncation becomes bridge failure",
        "args": ("0", "", "gocurl response exceeded Java capture limit (stderr truncated)"),
        "expect_rc": ANDROID_REQUEST_JNI_EXCEPTION,
        "expect_status": 0,
        "expect_error": "gocurl response exceeded Java capture limit (stderr truncated)",
    },
    {
        "name": "caller response truncation stays truncated",
        "args": ("200", "x" * 80, ""),
        "kwargs": {"response_len": 16, "error_len": 128},
        "expect_rc": ANDROID_REQUEST_TRUNCATED,
        "expect_status": 200,
        "expect_error": "response truncated to fit caller buffer",
    },
]


def main():
    failures = []
    for case in CASES:
        rc, status, response, error = finalize_request_result(
            *case["args"], **case.get("kwargs", {})
        )
        ok = (
            rc == case["expect_rc"]
            and status == case["expect_status"]
            and error == case["expect_error"]
        )
        print(f"{case['name']}: rc={rc} status={status} error={error!r} response_len={len(response)}")
        if not ok:
            failures.append(case["name"])

    if failures:
        print("FAIL:", ", ".join(failures))
        raise SystemExit(1)

    print(f"PASS: {len(CASES)} cases")


if __name__ == "__main__":
    main()
