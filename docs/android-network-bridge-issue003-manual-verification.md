# Android network bridge issue003 manual verification

Status: partial verification completed.

- Repo-local validation was run in this environment.
- Android APK/device execution was **not** available here, so on-device HTTP/header/body coverage remains explicitly unverified.

## Scope

Issue003 follow-up items requiring evidence:

- bridge/transport failures reported by Java must return non-zero from `android_request()`
- HTTP `4xx`/`5xx` results must still return `rc=0` when `status_out` is a real HTTP status
- caller-buffer truncation must still return `ANDROID_REQUEST_TRUNCATED`
- returned bridge failure text must stay sanitized

## Repo-local verification actually run

Command run from repo root:

```text
python3 tools/test-android-request-bridge-issue003.py
```

Observed output:

```text
GET 200 success: rc=0 status=200 error='' response_len=2
HTTP 404 remains success: rc=0 status=404 error='http 404 from server' response_len=7
HTTP 500 remains success: rc=0 status=500 error='http 500 from server' response_len=15
timeout becomes bridge failure: rc=-4 status=0 error='gocurl request timed out after 30 seconds' response_len=0
no output becomes bridge failure: rc=-4 status=0 error='gocurl produced no JSON output (helper process failed)' response_len=0
malformed JSON becomes bridge failure: rc=-4 status=0 error='gocurl returned malformed JSON' response_len=0
helper error becomes bridge failure: rc=-4 status=0 error='gocurl request failed: dns resolution failed (exit 6)' response_len=0
Java capture truncation becomes bridge failure: rc=-4 status=0 error='gocurl response exceeded Java capture limit (stderr truncated)' response_len=0
caller response truncation stays truncated: rc=-5 status=200 error='response truncated to fit caller buffer' response_len=15
PASS: 9 cases
```

## What the repo-local check proves

The scripted logic check mirrors `finalize_request_result()` in `firmware/target/hosted/android/request-android.c` and confirms:

- `status_out=404/500` keeps `rc=0`
- timeout / no-output / malformed-JSON / helper-error / Java-capture-truncation all produce non-zero `rc`
- Java-capture-truncation text stays sanitized as `gocurl response exceeded Java capture limit ...`
- caller response-buffer truncation still returns `ANDROID_REQUEST_TRUNCATED (-5)`

## What is still not verified here

Because no Android runtime/device was available in this environment, the following still need on-device/manual execution:

- HTTPS GET against a real endpoint
- newline-separated header pass-through (`Authorization`, `Content-Type`)
- `POST` and `PATCH` raw body handling
- HTTP `401` / `403` exact end-to-end behavior on device
- real helper timeout/process-startup/capture-limit behavior through JNI and Java process management

## Suggested on-device follow-up matrix

| Scenario | Example request inputs | Expected result |
| --- | --- | --- |
| HTTPS GET | `GET https://.../get` | `rc=0`, valid `status_out`, response body copied into caller buffer |
| Bearer + Content-Type headers | headers string contains `Authorization: Bearer ...\nContent-Type: application/json` | both headers arrive at server exactly once; no JNI failure |
| POST raw body | `POST` with JSON body | `rc=0`, expected HTTP status, server receives raw body unchanged |
| PATCH raw body | `PATCH` with JSON body | `rc=0`, expected HTTP status, server receives raw body unchanged |
| HTTP 401 | endpoint returns 401 | `rc=0`, `status_out=401`, body/error text returned as HTTP result, not JNI failure |
| HTTP 403 | endpoint returns 403 | `rc=0`, `status_out=403`, body/error text returned as HTTP result, not JNI failure |
| HTTP 404 | endpoint returns 404 | verified repo-local: `rc=0`, `status_out=404`; still needs device run |
| HTTP 5xx | endpoint returns 500/502/etc. | verified repo-local: `rc=0`, `status_out=5xx`; still needs device run |
| Timeout | endpoint sleeps longer than Java timeout | verified repo-local logic: non-zero bridge failure with timeout text; still needs device run |
| Process failure | break `/data/data/gocurl` startup | verified repo-local logic: non-zero bridge failure with sanitized text; still needs device run |
| Response truncation | endpoint body exceeds caller response buffer | verified repo-local logic: `ANDROID_REQUEST_TRUNCATED`; still needs device run |
| Java capture truncation | helper stdout/stderr exceeds Java capture limit | verified repo-local logic: non-zero bridge failure with sanitized capture-limit text; still needs device run |
