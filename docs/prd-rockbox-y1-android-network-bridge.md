# PRD: Rockbox-Y1 Generic Android Network Bridge

## Status

Draft specification only. Do not implement as part of this document.

Source analysis: consolidated from `../rockbox-audiobookshelf` PRDs, bridge patches, issues, docs, and plugin scaffold.

## Decision summary

- Specify and implement a reusable Rockbox-Y1 Android network bridge in this repo.
- Keep the bridge independent from the existing podcast downloader, while preserving all podcast APIs for compatibility.
- Expose bridge capabilities through Rockbox's plugin API (`rb->android_*`), not direct plugin JNI calls.
- Use the existing Android-hosted Y1 architecture: Rockbox plugin UI -> Rockbox plugin API -> hosted Android JNI -> Java -> `/data/data/gocurl` with `/data/data/cacert.pem`.
- Treat Audiobookshelf as the first required consumer and acceptance driver, but keep the bridge generic for future plugins.
- Prefer synchronous APIs for MVP because Rockbox plugins are menu-driven and simpler with caller-owned buffers.

## Problem statement

Rockbox-Y1 already has Android-hosted internet functionality, but the exposed plugin API is ad hoc and podcast-specific. Plugins can reuse the podcast WiFi path indirectly, and the podcast downloader has custom Java/JNI functions for feed and episode behavior, but other plugins do not have a reusable way to perform authenticated HTTPS requests or file downloads.

Audiobookshelf requires authenticated API requests, metadata browsing, progress upload, and offline file downloads over HTTPS. Those operations should not be implemented directly inside `.rock` plugin code because WiFi, TLS, certificates, process execution, and Android filesystem details belong in the hosted Android layer.

The Y1 Android environment is old enough that platform TLS/certificate behavior is risky for modern HTTPS. Existing Y1 update/network tooling already uses `/data/data/gocurl --cacert /data/data/cacert.pem`; the generic bridge should standardize on that path.

## Goals

- Add generic Android WiFi setup functions to the plugin API.
- Add a generic synchronous HTTP request API for small/medium API calls.
- Add a generic synchronous authenticated file download API for writing remote resources to local files.
- Preserve existing podcast plugin behavior and public API.
- Support bearer-token and arbitrary-header workflows needed by Audiobookshelf.
- Return HTTP status, transport errors, filesystem errors, and truncation information clearly enough for user diagnostics.
- Avoid logging tokens, request headers, request bodies, or response bodies by default.
- Provide probe plugins and smoke-test documentation for device validation.

## Non-goals

- Rewriting the existing podcast downloader on top of the new bridge.
- Generic upstream Rockbox support beyond the Y1 Android-hosted fork.
- Streaming playback from remote URLs.
- Full async/background transfer manager in MVP.
- ZIP download/extraction in MVP.
- Self-signed certificate support in MVP.
- Plain HTTP support by default.

## Proposed plugin API

The exact C ABI should be finalized during implementation, but the bridge should expose these capabilities through `struct plugin_api` on Android builds.

### Generic WiFi

```c
const char* (*android_connect_wifi)(void);
int (*android_disconnect_wifi)(void);
```

Semantics:

- `android_connect_wifi` is a generic alias for the existing Y1 WiFi setup path.
- Existing `android_podcast_connect_wifi` and `android_podcast_disconnect_wifi` remain available unchanged.
- The return/error semantics should match current podcast WiFi behavior for compatibility.

### Generic request

Recommended signature:

```c
int (*android_request)(const char *method,
                       const char *url,
                       const char *headers,
                       const char *body,
                       char *response_buf,
                       size_t response_len,
                       int *status_out,
                       char *error_buf,
                       size_t error_len);
```

Parameters:

- `method`: HTTP method such as `GET`, `POST`, `PATCH`, `DELETE`.
- `url`: absolute HTTPS URL.
- `headers`: newline-separated HTTP header lines, e.g. `Authorization: Bearer ...\nAccept: application/json`.
- `body`: raw request body string; may be `NULL` or empty.
- `response_buf`/`response_len`: caller-owned UTF-8 response body buffer.
- `status_out`: HTTP status code output. `0` means no HTTP status was obtained.
- `error_buf`/`error_len`: caller-owned diagnostic error buffer.

Return semantics:

- Return `0` when the bridge executed and copied outputs successfully, including HTTP 4xx/5xx responses.
- Return non-zero for bridge/JNI/transport/copy/truncation failures.
- HTTP 4xx/5xx are not bridge failures by themselves; callers inspect `status_out` and response/error text.
- Buffer truncation must be detectable. Recommendation: return a distinct non-zero code or encode a stable error message while preserving `status_out` where possible.

### Generic download

Recommended signature:

```c
int (*android_download)(const char *url,
                        const char *headers,
                        const char *destination_path,
                        int *status_out,
                        char *error_buf,
                        size_t error_len);
```

Parameters:

- `url`: absolute HTTPS URL to download.
- `headers`: same newline-separated header format as `android_request`.
- `destination_path`: final local path where the downloaded file should appear only after successful completion.
- `status_out`: HTTP status code output; `0` if no HTTP status was obtained.
- `error_buf`/`error_len`: caller-owned diagnostic error buffer.

Required behavior:

- Use the same header format as request.
- Create or validate destination parent directories before invoking transfer.
- Download to a temporary file in the destination filesystem and atomically rename on success where possible.
- Do not report success unless the full file was downloaded and moved to its final path.
- Surface HTTP failures, TLS/process failures, and filesystem failures distinctly enough for UI diagnostics.
- Prefer true streaming to a temp file via `gocurl` output options. Do not base64-load large file bodies into Java memory.

## Android/Java transport requirements

- Execute network operations from hosted Android Java, reached via JNI from Rockbox C code.
- Use `/data/data/gocurl` with `/data/data/cacert.pem` by default.
- Use a bounded connect timeout and total process timeout.
- Build command arguments defensively. If the Y1 `su -c` path forces shell strings, every dynamic value must be shell-quoted.
- Request execution should separate HTTP status from body robustly, preferably through `gocurl --json-output` for requests or status sidecar files for downloads.
- Download execution should stream directly to a temporary file, not return file bytes to Rockbox or Java as a whole response body.
- Java should return stable machine-readable fields to JNI: status, body or no body, error text.
- JNI should validate all caller parameters, initialize output buffers, clear Java exceptions where needed, and never leak local references in loops.

## Security requirements

- Do not log request headers, tokens, request bodies, response bodies, or full URLs containing secrets by default.
- Require HTTPS by default in the Audiobookshelf consumer. The bridge may technically execute arbitrary URLs, but docs should warn consumers against plain HTTP.
- Document that Y1 is an old Android/rooted device and API tokens should be scoped/rotated accordingly.
- Self-signed certificate support is out of MVP; add later only with explicit CA bundle configuration.
- ZIP extraction, if added later, must run on Android side, validate against zip-slip, handle temp space, and clean partial extraction.

## Diagnostics requirements

- Plugins must be able to show:
  - WiFi setup result.
  - HTTP status code.
  - Transport/JNI/process error text.
  - Filesystem error text for downloads.
  - Truncation indication when response/error buffers are too small.
- Probe plugins should exist for request and download validation.
- Probe configs should live under `/sdcard/.rockbox/` and avoid hardcoded secrets in source.

## Build and validation requirements

- The PRD does not require vendoring a second source tree.
- For this repo, implementation should be direct changes or clearly reviewable patch commits, not opaque generated artifacts.
- A patched Android APK must be buildable with documented SDK/NDK prerequisites.
- Device smoke test should cover:
  - Build patched APK.
  - Replace `/system/app/org.rockbox.apk` on rooted Y1 only with explicit opt-in.
  - Launch Rockbox and confirm service startup.
  - Run request probe against `https://example.com/` or configured endpoint.
  - Run download probe to a safe `/sdcard/.rockbox/...` path.
- Existing podcast downloader and lastfm WiFi usage should be regression-tested enough to confirm compatibility.

## Acceptance criteria

- [ ] Plugins can call `rb->android_connect_wifi()` and `rb->android_disconnect_wifi()` on Android/Y1 builds.
- [ ] Existing podcast WiFi functions still compile and behave as before.
- [ ] Plugins can call `rb->android_request()` with method, URL, newline-separated headers, optional body, response buffer, status output, and error buffer.
- [ ] Authenticated GET/POST/PATCH requests through bearer-token headers work against a modern HTTPS server.
- [ ] HTTP 401/403/404/5xx return an HTTP status and readable diagnostics without being confused with JNI failure.
- [ ] Response buffer truncation is detectable by caller.
- [ ] Plugins can call `rb->android_download()` with URL, headers, destination path, status output, and error buffer.
- [ ] Authenticated downloads use the same header format as requests.
- [ ] Downloads use `/data/data/gocurl` and `/data/data/cacert.pem`.
- [ ] Downloads do not leave a corrupt final destination file after failed/interrupted transfers.
- [ ] Filesystem, HTTP, TLS/process, timeout, and JNI failures are distinguishable enough for user diagnostics.
- [ ] Request/download probes can be built, installed, configured, and manually verified on Y1.
- [ ] No default logs expose tokens, headers, or bodies.

## Open implementation notes

- The sibling patch prototype used `PLUGIN_API_VERSION` bumps 273 -> 274 -> 275. Actual versioning in this repo must be reconciled with current `apps/plugin.h`.
- The sibling download patch demonstrated API shape but used `gocurl --json-output` body decoding for downloads. Final implementation should improve this to stream to a temp file.
- Exact return-code enum should be decided during implementation; PRD requires stable distinction between success, bridge failure, truncation, and invalid parameters.
