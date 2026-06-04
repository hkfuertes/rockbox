# PRD: Audiobookshelf Plugin for Rockbox-Y1

## Status

Draft specification only. Do not implement as part of this document.

Source analysis: consolidated from `../rockbox-audiobookshelf` PRD, plugin scaffold, tests, docs, and issue slices.

## Dependency

This PRD depends on `docs/prd-rockbox-y1-android-network-bridge.md`.

The plugin cannot provide its core online features until Rockbox-Y1 exposes generic Android WiFi, request, and download functions to plugins.

## Decision summary

- Specify the Audiobookshelf plugin alongside the bridge because it is the first concrete consumer and the bridge API cannot be validated meaningfully without it.
- Keep the bridge and plugin as separate PRDs: bridge is reusable infrastructure; Audiobookshelf is product-specific.
- Build the plugin for Rockbox-Y1 specifically, not upstream generic Rockbox.
- Use an offline-first model: browse/list/download over WiFi, then play local files through normal Rockbox playback.
- Do not stream playback from Audiobookshelf in MVP.
- Use API-token authentication only in MVP; username/password login is out of scope.
- Keep JSON parsing/backend logic separated from Rockbox UI menus.

## Problem statement

Innioasis Y1 users running Rockbox-Y1 can use some WiFi-enabled plugins, but there is no first-class Audiobookshelf workflow on the device. Users currently need to download audiobooks elsewhere, copy files to the SD card, organize folders manually, and reconcile listening progress manually.

The desired experience is an offline-first Audiobookshelf client inside Rockbox-Y1: configure server/token, browse libraries/books, download audiobook files to local storage, play through Rockbox, and eventually sync progress back to Audiobookshelf.

## Goals

- Provide a Rockbox plugin that connects to an Audiobookshelf server using the generic Android network bridge.
- Support API-token authentication.
- Browse Audiobookshelf libraries and books with pagination.
- Download audiobook audio files for offline playback.
- Track downloaded books/files in a local index.
- Provide deterministic, sanitized local folder/file naming.
- Provide diagnostics for WiFi, server authentication, downloads, and last error.
- Stage progress sync safely without replacing Rockbox's core playback/bookmark system.

## Non-goals for MVP

- Streaming playback from Audiobookshelf.
- Podcast support from Audiobookshelf.
- Cover image browsing/download/display.
- Full-text search.
- Username/password login.
- Managing Audiobookshelf users, libraries, metadata, or server settings.
- Plain HTTP by default.
- Self-signed certificates.
- Perfect automatic remote resume/seek.
- Automatic progress sync in the first MVP.
- Replacing Rockbox's core playback/bookmark system.
- ZIP download/export as the default path.

## User stories

- As a Y1 user, I can configure my Audiobookshelf server URL and API token on the device/storage.
- As a Y1 user, I can test WiFi and server authentication before browsing.
- As a Y1 user, I can list libraries available to my token.
- As a Y1 user, I can browse books in a library without loading the entire server into memory.
- As a Y1 user, I can download all files for a book.
- As a Y1 user, I can download only missing files for a partially downloaded book.
- As a Y1 user, I can play a downloaded book using Rockbox local playback.
- As a Y1 user, I can delete local files for a downloaded book.
- As a Y1 user, I can see actionable diagnostics when WiFi, authentication, HTTPS, server, or filesystem operations fail.
- As a future enhancement, I can sync listening progress back to Audiobookshelf.

## Configuration

Recommended config file:

```text
/sdcard/.rockbox/audiobookshelf.cfg
```

Required values:

- `server_url`: HTTPS base URL for Audiobookshelf.
- `api_token`: Audiobookshelf API token.

Optional values:

- `download_root`: local root folder for downloaded audiobooks.
- `default_library_id`: when set, skip the library chooser and browse this library directly.
- `page_size`: default around 50.
- `progress_sync_enabled`: default false for MVP.

Config requirements:

- Reject missing server URL or token with clear diagnostics.
- Prefer/require HTTPS in normal configuration.
- Normalize base URL by trimming trailing slashes.
- Never display or log full token value.
- Example config should use placeholders only.

## Product flow

### First-run / main menu

Recommended menu entries:

1. Browse libraries / Browse configured library.
2. Downloads / Local library.
3. Diagnostics.
4. Settings/help text.

### Diagnostics menu

Required actions:

- Test WiFi.
- Test Audiobookshelf authentication.
- Test full connectivity.
- View last error.

Recommended diagnostic statuses:

- `success`
- `config_error`
- `connection_error`
- `auth_error`
- `server_error`
- `transport_error`
- `filesystem_error`
- `not_found`
- `truncated`

### Browse flow

- If `default_library_id` is set, enter that library directly.
- Otherwise, list libraries from Audiobookshelf.
- List books page-by-page, recommended page size ~50.
- UI receives simple list entries, not raw JSON.
- Book entries should include stable ID, title, author/series display when available, downloaded state, and progress summary when available.

### Book actions

Required actions:

- Download all.
- Download missing.
- Play if downloaded.
- Delete local.
- Show details/status.

Download behavior:

- Download per audio/media file, not whole-item ZIP.
- Multi-file audiobooks are tracked file-by-file.
- Interrupted downloads can be resumed by downloading missing files only.
- Destination files appear as complete only after bridge-level success.

## Audiobookshelf API usage

The exact endpoint paths should be verified against the target Audiobookshelf version during implementation, but the plugin needs these backend capabilities:

- Authenticate/test token with a lightweight server/user endpoint.
- List libraries.
- List/search books within a library with pagination.
- Fetch item/book metadata including media files.
- Download individual audio files with bearer-token headers.
- Upload progress in a later stage using POST/PATCH.

Headers:

```text
Authorization: Bearer <api_token>
Accept: application/json
Content-Type: application/json
```

Downloads use the same `Authorization` header format through `android_download`.

## Architecture

Recommended modules:

- `config`: load/validate config and normalize values.
- `backend_interface`: stable backend contract used by UI.
- `backend_http`: Audiobookshelf HTTP implementation using `rb->android_request` and `rb->android_download` indirectly.
- `library_cache`: cache libraries/book pages where useful.
- `downloader`: book-level download orchestration.
- `local_index`: persistent mapping between local files and Audiobookshelf IDs.
- `local_state`: downloaded/deleted/progress state helpers.
- `path_sanitizer`: deterministic safe path generation.
- `rockbox_progress`: read local playback/bookmark/progress signals where possible.
- `progress_sync`: manual progress sync implementation.
- `auto_progress_sync`: later staged automatic sync.
- `error_store`: last error storage/redaction.
- `ui_libraries`, `ui_books`, `ui_diagnostics`: Rockbox menu/UI layers.
- `plugin`: entrypoint and top-level menu wiring.

Separation requirements:

- UI must not parse raw Audiobookshelf JSON.
- Backend must return simple tables/structs/lists with stable fields.
- Downloader must not know UI internals.
- Path sanitizer must be reusable and unit-tested independently.

## Local storage and index

Recommended download layout:

```text
<download_root>/<sanitized-library>/<sanitized-author-title>/<nnn-sanitized-filename>
<download_root>/<sanitized-library>/<sanitized-author-title>/.audiobookshelf.json
```

Index fields:

- server/base URL identity or hash.
- library ID and library name.
- item/book ID.
- media file ID.
- local path.
- original filename/track ordering.
- downloaded state.
- size/duration when available.
- last known local progress.
- last known remote progress.
- last sync timestamp/status.

Path requirements:

- Sanitize path separators, control characters, reserved names, leading/trailing whitespace, and overly long components.
- Keep deterministic naming so repeated downloads map to the same path.
- Use numeric prefixes for ordered media files.
- Prevent path traversal and accidental writes outside `download_root`.

## Progress sync staging

### MVP

- Preserve normal Rockbox playback/bookmark behavior.
- Store Audiobookshelf mapping sidecars/index for downloaded files.
- Show downloaded/local state and, if easy, local progress readouts.
- Provide manual diagnostics around whether progress sync is configured/available.

### Later stage

- Manual "sync progress now" action.
- Upload local progress for downloaded items with known Audiobookshelf IDs.
- Handle auth/server/connection failures without losing local progress.

### Future stage

- Optional automatic progress sync.
- Conflict policy between local and remote progress.
- Remote resume assistance, if it can be done without fighting Rockbox playback.

Recommended conflict policy for later design:

- Never overwrite newer local progress silently.
- Prefer explicit user action when remote progress is ahead by a meaningful threshold.
- Upload only when item mapping is unambiguous.

## Security and privacy

- API token must never be printed in logs, diagnostics, screenshots, or errors.
- Headers/bodies must not be logged by default.
- Diagnostics should redact tokens and sensitive URL query parameters.
- Docs must warn that Y1 is old/rooted and token scope should be limited.
- HTTPS required/recommended; self-signed cert support deferred.

## Testing requirements

Unit/module tests should cover:

- Config parsing and validation.
- HTTPS URL normalization/rejection behavior.
- Auth header construction with token redaction in diagnostics.
- Library JSON conversion to simple UI entries.
- Book/media-file JSON conversion.
- Pagination parameters and empty page handling.
- Downloader: download all, download missing, partial failure, retry.
- Local index read/write/update and corrupted index recovery.
- Path sanitizer edge cases and traversal prevention.
- Diagnostics status mapping.
- Progress sync calculations and skip cases.
- UI menu behavior using fake backend responses.

Integration/device validation should cover:

- WiFi test on Y1.
- Auth test against a real Audiobookshelf server.
- Library list with valid token.
- Auth failure with invalid token.
- Download a small test audiobook file.
- Resume partial/missing multi-file book.
- Play downloaded file through Rockbox.
- Delete local download.

## Acceptance criteria

- [ ] Plugin builds for Rockbox-Y1 after the generic Android bridge exists.
- [ ] Plugin can load config with server URL and API token from `.rockbox` storage.
- [ ] Diagnostics can test WiFi and show success/failure.
- [ ] Diagnostics can test Audiobookshelf authentication and distinguish invalid token from connection/server failures.
- [ ] Plugin can list libraries, unless a default library ID is configured.
- [ ] Plugin can list books in a library with pagination.
- [ ] Plugin can show simple book entries without exposing raw JSON to UI code.
- [ ] Plugin can download all files for a book using `android_download` and bearer-token headers.
- [ ] Plugin can download missing files for a partially downloaded book.
- [ ] Plugin maintains a local index mapping downloaded files to Audiobookshelf IDs.
- [ ] Plugin uses deterministic sanitized paths and prevents traversal outside download root.
- [ ] Plugin can play a downloaded file/book through normal Rockbox local playback.
- [ ] Plugin can delete local files for a book and update local index.
- [ ] Last-error diagnostics are available and redact token/header/body data.
- [ ] MVP does not require streaming, ZIP extraction, username/password login, covers, search, or automatic sync.

## Recommended implementation slices

1. Bridge API lands and request/download probes pass on Y1.
2. Plugin scaffold builds with fake backend and UI tests.
3. Config + diagnostics using real bridge.
4. Auth + library list.
5. Book listing with pagination.
6. Download all/missing for single-file books.
7. Multi-file book local index and resume missing.
8. Play/delete local downloads.
9. Manual progress sync spike.
10. Automatic progress sync only after manual sync is reliable.

## Open implementation notes

- The sibling repo contains Lua-like plugin scaffold/tests; actual integration into this C-based Rockbox tree must be designed carefully.
- If the final plugin is C-only, retain the same module boundaries conceptually.
- If a Lua/plugin runtime is intended, specify and validate that runtime separately before committing to plugin implementation.
- Audiobookshelf endpoint details must be verified against the server version selected for testing.
