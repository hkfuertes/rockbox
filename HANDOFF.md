# Handoff: Rockbox Y1 local APK build + JNI/gocurl work

## Goal

Build the Rockbox Y1 Android APK locally, then add JNI/native support needed so `.rock` plugins can execute correctly on the device. Current user focus has shifted toward understanding the architecture and adding a more generic gocurl/JNI helper usable from Rockbox plugins/apps.

User specifically wants **local builds** because they will make iterative JNI changes. They initially did not want Rosetta2, tried QEMU, then accepted Rosetta2 for speed/stability.

## Current environment decisions

- Host: Apple Silicon macOS (`arm64`).
- Target device/app: Innioasis Y1 / old Android / `armeabi` ARM 32-bit.
- Build host problem: Android NDK r10e uses old x86_64 host toolchain; native ARM64 Docker is not viable.
- QEMU x86_64 Colima was tried and produced very slow/unstable GCC behavior.
- Rosetta2 was installed successfully via:

```bash
softwareupdate --install-rosetta --agree-to-license
```

- Current intended Colima profile:

```bash
colima start --profile rockbox-rosetta \
  --arch aarch64 \
  --vm-type=vz \
  --vz-rosetta \
  --cpu 4 \
  --memory 8 \
  --disk 80

docker context use colima-rockbox-rosetta
docker info --format 'Server={{.Architecture}}/{{.OSType}}'
```

Expected `docker info` output is `Server=aarch64/linux`; that is OK because amd64 containers should be translated through Rosetta by Colima/VZ.

## Files added for local APK-only build

Uncommitted files:

- `Dockerfile.local`
- `tools/docker-local-build-y1.sh`
- `tools/local-build-y1.sh`
- `HANDOFF.md`

Purpose:

- `Dockerfile.local`: creates an Ubuntu 22.04 `linux/amd64` build image with Android SDK/NDK r10e and Java 17.
- `tools/docker-local-build-y1.sh`: wrapper that builds/runs the Docker image and invokes the inner script.
- `tools/local-build-y1.sh`: inner build script. For `240p`/`360p`, builds only the APK; `all`/`update` also build Go helper tools and update zips.

## Build command

For fastest iteration, build only the APK:

```bash
cd ~/projects/rockbox
rm -rf android/build   # only if previous configure/build state is corrupted
./tools/docker-local-build-y1.sh 240p
```

Expected artifact:

```text
android/build/rockbox_240p.apk
```

For 360p:

```bash
./tools/docker-local-build-y1.sh 360p
```

Do **not** use `all` unless update zips are needed; it builds Go tools (`gocurl`, `poddl`) and is slower / needs newer Go.

Important: if Docker is not already running, `tools/docker-local-build-y1.sh` currently starts Colima x86_64/QEMU. Prefer manually starting/switching to the Rosetta profile above before running the script.

## Errors already handled

1. Go version error in `gocurl`:

```text
invalid go version '1.23.0'
unknown directive: toolchain
```

Cause: script was building Go tools even for APK-only build. Fixed by only running `build_go_tools` for `all`/`update`.

2. NDK permission errors:

```text
make-standalone-toolchain.sh: Permission denied
prebuilt-common.sh: Permission denied
```

Cause: NDK files in image were not readable/executable by non-root container user. Fixed in `Dockerfile.local` with:

```dockerfile
chmod -R a+rX "$ANDROID_NDK_PATH"
```

3. GCC segfaults under QEMU/ARM64 Docker:

```text
arm-linux-androideabi-gcc: internal compiler error: Segmentation fault (program cc1)
mmap: Bad file descriptor
```

Cause: old GCC/NDK under amd64 container emulation on ARM64 Docker/QEMU. Direction changed to Rosetta-backed Colima profile.

## Architecture clarified this session

The repo is **not Android/AOSP source** and not a full generic ROM tree. It is Rockbox + Android/Y1 port + build/package scripts.

Runtime shape:

```text
Android app Java layer
  -> RockboxService / RockboxActivity
  -> loads librockbox.so
  -> calls native Rockbox main()
  -> Rockbox C runs and loads plugins
```

`librockbox.so` is the native Rockbox engine/library. Android launches it through the Java service/activity layer.

The APK is intended as a **system app** on the Y1 (`android:sharedUserId="android.uid.system"`) and expects privileged/root/system placement. The `update_*.zip` is not a normal TWRP/CWM flashable ZIP; it is consumed by Rockbox's internal updater and copies APK/libs/scripts/keylayouts into `/system` and `/data/data`.

## Relevant code locations for JNI/plugin work

Plugin loading path:

- `apps/plugin.c` — `plugin_load(...)` calls `lc_open`, validates plugin header, sets plugin API, runs plugin entry.
- `firmware/target/hosted/lc-unix.c` — hosted plugin loader uses `dlopen(...)`, `dlsym(... "__header")`, `dlclose(...)`.
- `apps/plugin.h` — `struct plugin_api`; adding plugin-callable functions requires appending fields here and wiring implementations in `apps/plugin.c`.
- `apps/plugins/plugins.make` — hosted plugin link uses shared objects (`PLUGINLDFLAGS = $(SHARED_LDFLAGS)` when `APP_TYPE` is set).

Android JNI/native locations:

- `firmware/target/hosted/android/system-android.c` — `JNI_OnLoad`, `Java_org_rockbox_RockboxService_main`, global Java VM/env/service refs.
- `firmware/target/hosted/android/button-android.c` — examples of Java→native JNI methods.
- `firmware/target/hosted/android/connectivity-android.c` and `podcast-android.c` — native/plugin API functions calling Android/Java helpers.
- `android/src/org/rockbox/RockboxService.java` — loads `librockbox.so`, extracts `.rockbox` resources, and exposes Java methods called from JNI.
- `android/src/org/rockbox/Helper/Connectivity.java` — runs `gocurl`/`poddl`, wifi helpers, scrobbling/podcast logic.
- `android/src/org/rockbox/RockboxFramebuffer.java` — examples of native declarations/use from Java side.
- `firmware/SOURCES` — add new Android C source files here, near `target/hosted/android/connectivity-android.c` and `podcast-android.c`.

Existing plugin API additions in this fork include functions like:

- `upload_scrobble`
- `android_podcast_get_episode_list`
- `android_podcast_connect_wifi`
- `free_array`

These are declared in `apps/plugin.h`, assigned in `apps/plugin.c`, implemented under `firmware/target/hosted/android/`, and consumed from plugins like `apps/plugins/podcast_downloader.c` and `apps/plugins/lastfm_scrobbler.c`.

## Current gocurl/poddl call chains

### Scrobbling / gocurl

```text
apps/plugins/lastfm_scrobbler.c
  -> rb->upload_scrobble(...)
  -> apps/plugin.h / apps/plugin.c plugin API table
  -> firmware/target/hosted/android/connectivity-android.c
  -> JNI CallBooleanMethod RockboxService.lastfmScrobbler(...)
  -> android/src/org/rockbox/RockboxService.java
  -> android/src/org/rockbox/Helper/Connectivity.java
  -> Runtime.getRuntime().exec("su -u root -c /data/data/gocurl ...")
```

### Podcasts / poddl

```text
apps/plugins/podcast_downloader.c
  -> rb->android_podcast_*(...)
  -> apps/plugin.h / apps/plugin.c plugin API table
  -> firmware/target/hosted/android/podcast-android.c
  -> JNI calls RockboxService methods:
       getPodcastNames, getEpisodeList, getEpisodePath,
       startPodcastDownload, deleteEpisode,
       connectWifi, disconnectWifi
  -> android/src/org/rockbox/RockboxService.java
  -> android/src/org/rockbox/Helper/Connectivity.java
  -> Runtime.getRuntime().exec("su -u root -c /data/data/poddl ...")
```

`gocurl` and `poddl` are external executables copied to:

```text
/data/data/gocurl
/data/data/poddl
```

They are not JNI libraries.

## Implemented generic Android network API for first plugin work

Status: merged into local/remote `main` via PR #1. The old “generic gocurl helper” direction has been implemented as a structured Android plugin API, not as raw shell/gocurl passthrough.

### Plugin-facing functions

Available from any Rockbox plugin via `rb->...` on Android/Y1 builds:

```c
const char *rb->android_connect_wifi(void);
int rb->android_disconnect_wifi(void);

int rb->android_request(const char *method,
                        const char *url,
                        const char *headers,
                        const char *body,
                        char *response_buf,
                        size_t response_len,
                        int *status_out,
                        char *error_buf,
                        size_t error_len);

int rb->android_download(const char *url,
                         const char *headers,
                         const char *destination_path,
                         int *status_out,
                         char *error_buf,
                         size_t error_len);
```

Return codes for request/download are in `apps/plugin.h`:

```c
ANDROID_REQUEST_OK = 0
ANDROID_REQUEST_INVALID_PARAM = -1
ANDROID_REQUEST_JNI_UNAVAILABLE = -2
ANDROID_REQUEST_JNI_METHOD_MISSING = -3
ANDROID_REQUEST_JNI_EXCEPTION = -4
ANDROID_REQUEST_TRUNCATED = -5
```

`status_out` is the HTTP status. A non-zero bridge return means JNI/helper/capture/truncation failure; an HTTP 4xx/5xx with bridge return 0 means the bridge worked and the server returned an error.

### Header/body format

`headers` is either `NULL` or newline-separated header lines:

```text
Authorization: Bearer TOKEN
Accept: application/json
Content-Type: application/json
```

`body` is either `NULL` or the exact request body string. For JSON POST/PATCH, pass `Content-Type: application/json` in `headers` and JSON in `body`.

### Minimal request example

```c
char response[2048];
char error[256];
int http_status = 0;
int rc;

rb->android_connect_wifi();

rc = rb->android_request("GET",
                         "https://example.com/api/me",
                         "Accept: application/json\nAuthorization: Bearer REPLACE_ME",
                         NULL,
                         response,
                         sizeof(response),
                         &http_status,
                         error,
                         sizeof(error));

rb->android_disconnect_wifi();

if (rc == ANDROID_REQUEST_OK && http_status >= 200 && http_status < 300) {
    /* parse response */
} else {
    /* show rc/http_status/error; redact tokens */
}
```

### Minimal download example

```c
char error[256];
int http_status = 0;
int rc;

rb->android_connect_wifi();

rc = rb->android_download("https://example.com/file.mp3",
                          "Authorization: Bearer REPLACE_ME",
                          "/sdcard/.rockbox/audiobookshelf/downloads/file.mp3",
                          &http_status,
                          error,
                          sizeof(error));

rb->android_disconnect_wifi();
```

Download destination paths are intentionally constrained by the Java helper to Rockbox-controlled storage. Use `/sdcard/.rockbox/...` paths for plugin downloads.

### Implementation call chain

```text
plugin C
  -> rb->android_connect_wifi / android_request / android_download
  -> apps/plugin.h / apps/plugin.c plugin API table
  -> firmware/target/hosted/android/wifi-android.c or request-android.c
  -> JNI call into RockboxService
  -> android/src/org/rockbox/RockboxService.java
  -> android/src/org/rockbox/Helper/Connectivity.java
  -> /data/data/gocurl with structured, shell-quoted arguments
```

Important source files:

- `apps/plugin.h` — plugin API signatures and `enum android_request_status`.
- `apps/plugin.c` — plugin API table wiring.
- `firmware/target/hosted/android/wifi-android.c` — generic WiFi aliases delegating to existing Y1/podcast WiFi path without modifying podcast downloader.
- `firmware/target/hosted/android/request-android.c` / `.h` — JNI bridge for request/download.
- `android/src/org/rockbox/RockboxService.java` — Java methods called from JNI.
- `android/src/org/rockbox/Helper/Connectivity.java` — command building, gocurl execution, output parsing, download path safety.
- `apps/plugins/android_request_probe.c` and `android_download_probe.c` — small examples/smoke tests.

### On-device caveats discovered during validation

- TLS requires a sane Y1 system clock. Device was stuck in 2022 and gocurl failed certificate validation until fixed with BusyBox date.
- `/data/data/cacert.pem` must exist; current helper passes `--cacert /data/data/cacert.pem` to gocurl.
- Rockbox Android lists plugins from internal app storage: `/data/data/org.rockbox/app_rockbox/rockbox/rocks/apps`. The updater now copies packaged `.rockbox/rocks` there automatically so new probe/plugin `.rock` files appear under `Plugins -> Applications` after update.
- Do not expose raw `gocurl` args from a plugin. Keep using `android_request` / `android_download`; Java side shell-quotes structured fields.

### Smoke-test plugin locations

After a correct update, probes should appear in:

```text
Plugins -> Applications -> android_request_probe
Plugins -> Applications -> android_download_probe
```

Their source is in `apps/plugins/`. The download probe reads config from:

```text
/sdcard/.rockbox/android_download_probe.cfg
```

## Suggested next-session skills

- Use `diagnose` if the local build still fails after switching to Rosetta-backed Colima, or if JNI/plugin loading crashes.
- Use `context-mode`/`ctx_execute` for build logs, logcat, `rg` output, or other large output.

## Immediate next step

Start the first real plugin (likely Audiobookshelf) on top of the implemented generic Android API:

1. Create plugin scaffold under `apps/plugins/` and register it in `apps/plugins/SOURCES` + `apps/plugins/CATEGORIES`.
2. Load plugin config from `/sdcard/.rockbox/audiobookshelf.cfg`.
3. Use `rb->android_connect_wifi()` then `rb->android_request()` for diagnostics/auth/list APIs.
4. Use `rb->android_download()` for file downloads into `/sdcard/.rockbox/audiobookshelf/...`.
5. Always redact API tokens in UI/errors and remember TLS depends on valid Y1 date + `/data/data/cacert.pem`.
