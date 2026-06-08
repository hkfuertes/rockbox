# Audiobookshelf individual audio-file download fallback

**Status: DEFERRED — not needed for MVP**

> Decision (2026-06-08): Issue 006 validation is provisionally treated as passed — whole-item download is acceptable for MVP. This conditional fallback is not implemented. Reactivate only if device testing proves whole-item download is insufficient for Rockbox playback.
>
> **No code changes have been made for this issue.**
>
> **Reactivation criteria:**
> - Device validation (issue 006) confirms that whole-item download produces files that Rockbox cannot play (wrong format, incomplete zip, or inaccessible container).
> - If reactivated: implement per-file download loop using item detail audio-file metadata, then re-open this issue and work through the acceptance criteria below.

## Parent

`audiobookshelf-plugin-prd.md`

## What to build

If whole-item download is not directly useful for playback, add an individual audio-file download flow using the selected item's parsed audio-file metadata. The user should still select one book and choose Download, but the plugin should download each playable audio file into the book's sanitized folder with deterministic filenames.

This slice is conditional and should only be implemented if device validation shows whole-item download is insufficient.

## Acceptance criteria

- [ ] The plugin can identify downloadable audio files from item detail metadata.
- [ ] The download confirmation screen indicates when multiple files will be downloaded.
- [ ] Each audio file is downloaded through the Android download bridge with Audiobookshelf authentication.
- [ ] Each destination filename is sanitized and deterministic.
- [ ] Partial failure is reported with enough detail for the user to know which file failed.
- [ ] Successfully downloaded files remain in the book folder even if a later file fails.
- [ ] The resulting files are usable by Rockbox playback when Audiobookshelf provides compatible audio formats.
- [ ] The flow remains bounded and understandable on the Y1 UI.

## Blocked by

- `issues/006-audiobookshelf-download-behavior-validation.md`
