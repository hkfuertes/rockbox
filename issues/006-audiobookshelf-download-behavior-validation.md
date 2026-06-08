# Validate Audiobookshelf download behavior on device

## Parent

`audiobookshelf-plugin-prd.md`

## What to build

Run a human-in-the-loop validation of the first download implementation against a real Audiobookshelf server and the Y1 Android bridge. Determine whether Audiobookshelf whole-item download produces a directly playable audiobook, an archive, or another format. Capture the result as the decision that controls whether the MVP is sufficient or needs individual audio-file download support.

This slice is intentionally HITL because it depends on real server/device behavior and playback verification.

## Acceptance criteria

- [ ] A real Audiobookshelf item can be selected and downloaded on the Y1 device.
- [ ] The downloaded artifact's type and naming are recorded.
- [ ] The downloaded artifact is checked for Rockbox playback usability.
- [ ] Failure modes are recorded separately for server response, bridge behavior, filesystem policy, and playback compatibility.
- [ ] A clear decision is made: whole-item download is acceptable for MVP, or individual audio-file download is required before MVP completion.
- [ ] Any required follow-up issue is created or explicitly marked unnecessary.

## Blocked by

- `issues/005-audiobookshelf-safe-download-outside-rockbox.md`

---

## Assumed decision (pending device validation)

**Whole-item download path is provisionally acceptable for MVP** until a real Y1 device test proves otherwise.

Rationale: the download bridge in issues/003–005 uses the `android_download` gocurl bridge call, which writes the file directly to the filesystem via the Android bridge — **not** through Android DownloadManager, and with **no notification** in the Android shade. Files land at a fixed path of `/sdcard/audiobookshelf/downloads/<sanitized title>/<sanitized title>-<sanitized item id>.abs` — the `.abs` extension is always forced by the bridge regardless of what the server returns. Whether the server sends a single audio file or a ZIP archive is unknown without a real server; either case has a workable path (single file → play directly from Rockbox file browser if `.abs` does not block it; archive → file manager or follow-up issue). Individual audio-file download support is tracked in `issues/007-audiobookshelf-individual-audio-file-download-fallback.md` and is **conditional** on device validation showing the whole-item path is not playable.

Acceptance criteria are marked below as "validation guide" steps that the user must run manually on device. Issue proceeds in this assumed state; update decision checkboxes after running the guide.

---

## Human validation guide

Run these steps on the Y1 device once the feature from issues/001–005 is installed.

### Setup

1. Open the Audiobookshelf plugin from the Rockbox menu.
2. Pick a library and browse to a book that has at least one audio file.

### Trigger download

3. On the book detail screen, tap **Download** (the dedicated download button — not a long-press context menu).
4. A result splash screen will appear showing: HTTP status code, bridge return code, and destination path written by the bridge. Note all three values. (The splash does **not** show the server Content-Type header.)
5. Dismiss the splash and observe whether the screen returns to the detail view without a crash.

### Inspect the artifact

6. From the **Rockbox file browser** (not the Android file manager), navigate to `/sdcard/audiobookshelf/downloads/<sanitized title>/`.
7. The file will always be named `<sanitized title>-<sanitized item id>.abs`. Record:
   - The full filename as it appears on disk (e.g. `My-Book-abc123.abs`).
   - The actual artifact type — the `.abs` extension does **not** reflect the real format. Determine the real type by one of these methods (use whichever is available):
     - Run `adb shell file /sdcard/audiobookshelf/downloads/<title>/<file>.abs` from a connected desktop.
     - Open the file in the Android file manager and note what app it offers to open it with.
     - Read the first 4–8 bytes with a hex viewer or `adb shell xxd`; compare against known magic bytes (e.g. `ftyp` at offset 4 → MP4/M4B, `PK\x03\x04` → ZIP, `ID3` or `\xff\xfb` → MP3).
     - If none of the above is available, record the artifact type as **unknown**.
8. If the artifact type indicates a ZIP or archive, note that and skip to the **Failure modes** section for filesystem policy.

### Playback check

9. From the Rockbox file browser, navigate to the downloaded `.abs` file.
10. Attempt to play it. **Important:** Rockbox identifies formats by file extension. The forced `.abs` extension is not a recognized audio format and will likely cause a "file not supported" or "codec failure" error even if the underlying content is valid audio.
11. If playback fails due to the `.abs` extension:
    - If possible, use the Android file manager or a shell to copy or rename the file to its true extension derived from the artifact type detected in step 7 (e.g. `.m4b` if magic bytes indicate MP4, `.mp3` if MP3 — or try common extensions if type is unknown).
    - Retry playback with the renamed file.
    - Record `.abs` extension as a **filesystem/playback failure** in the table below regardless of whether the rename workaround succeeds — the bridge must be fixed to write the correct extension before this layer can pass.

### Record failure modes separately

| Layer | Observed behavior | Pass / Fail |
|-------|-------------------|-------------|
| Server response (HTTP status, bridge rc) | | |
| Bridge behavior (bridge rc from splash, no crash after dismiss) | | |
| Filesystem (file appears in expected path, readable) | | |
| Playback compatibility (Rockbox plays or rejects format) | | |

### Decision point

After completing the steps above, check exactly one:

- [ ] **Whole-item download is acceptable for MVP** — file lands, is a supported audio format, Rockbox plays it. Mark `issues/007` as `won't do / not needed`.
- [ ] **Individual audio-file download is required** — artifact is an archive, unsupported format, or missing entirely. Activate `issues/007-audiobookshelf-individual-audio-file-download-fallback.md` and link findings there.
