# Audiobookshelf hardening and regression tests

## Parent

`audiobookshelf-plugin-prd.md`

## What to build

Harden the completed Audiobookshelf plugin by adding focused regression coverage for the deep modules and tightening user-facing failure behavior. This slice should not add new product scope; it should make the implemented browse/download flow safer to modify and easier to debug.

The valuable test targets are config parsing, JSON extraction, path sanitization, and secret redaction. UI and Android/JNI behavior should remain smoke-tested on device rather than unit-tested through implementation details.

## Acceptance criteria

- [x] Config parsing is covered for valid config, missing required fields, comments/blank lines, whitespace, custom download root, and malformed lines.
- [x] JSON extraction is covered with representative Audiobookshelf library, item-list, and item-detail responses.
- [x] JSON extraction fails gracefully for truncated responses, missing fields, and oversized arrays/token sets.
- [x] Path sanitization is covered for slashes, colons, punctuation, empty titles, very long titles, and traversal/base-path checks.
- [x] Token and Authorization header redaction is covered by deterministic checks.
- [x] On-device smoke test notes cover auth, library browsing, item browsing, download success/failure, and WiFi disconnect behavior.
- [x] The plugin's user-facing errors remain concise enough for the target UI while preserving diagnostic value.

## Implementation notes

- Deterministic self-tests now live inside `apps/plugins/audiobookshelf.c` so they execute the production static helpers and JSON parsers directly.
- Self-tests are reachable from the plugin `Diagnostics` menu via `Run Self Tests`.
- Config parsing was minimally refactored so the same line parser/validation logic can be exercised without touching the user's live config file.

## On-device smoke checklist

- [ ] Launch plugin with a valid `server_url` and `token`; confirm browse flow opens without exposing token text.
- [ ] Open `Diagnostics -> Test Auth`; confirm HTTP status is shown and any bridge error is redacted.
- [ ] Open `Browse Library`; confirm accessible libraries load or configured `library_id` skips the picker.
- [ ] Open a library and page through items; confirm previous/next navigation and concise empty/end-of-list behavior.
- [ ] Open an item detail view; confirm title/author/series/narrator metadata renders and `[Download]` is reachable.
- [ ] Trigger a successful download; confirm destination stays under `/sdcard/audiobookshelf/...` and completion status is shown.
- [ ] Trigger a download failure (bad root, bad item, or bridge/server failure); confirm the failure text is concise and token-redacted.
- [ ] Disconnect WiFi before or during auth/list/detail/download requests; confirm the plugin reports WiFi/bridge failure cleanly.
- [ ] Open `Diagnostics -> Run Self Tests`; confirm the summary reports pass/fail counts on device.

## Blocked by

- `issues/005-audiobookshelf-safe-download-outside-rockbox.md`
- `issues/006-audiobookshelf-download-behavior-validation.md`
