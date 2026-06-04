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

## Direction for a generic gocurl/JNI helper

Interpret user's “para usarlo en cualquier app” as “usable by any Rockbox plugin/app in this Rockbox process”, unless they clarify they mean arbitrary third-party Android apps.

Recommended path:

1. Add plugin API entry:
   - `apps/plugin.h`
   - `apps/plugin.c`

2. Add C/JNI bridge:
   - either extend `firmware/target/hosted/android/connectivity-android.c`
   - or create cleaner file `firmware/target/hosted/android/gocurl-android.c`

3. If creating a new C file, add it to Android block in:
   - `firmware/SOURCES`

4. Add Java method wrapper in:
   - `android/src/org/rockbox/RockboxService.java`

5. Add actual executor/helper in:
   - `android/src/org/rockbox/Helper/Connectivity.java`

Potential shape:

```text
plugin C
  -> rb->android_gocurl(...)
  -> C/JNI bridge
  -> RockboxService.gocurl(...)
  -> Connectivity.gocurl(...)
  -> /data/data/gocurl
```

Design warning: avoid exposing a fully raw `gocurl(args)` if possible, because current execution uses shell/root:

```java
Runtime.getRuntime().exec("su -u root -c " + command)
```

A generic raw args string risks command injection. Prefer structured helpers such as:

```text
android_http_get(url)
android_http_post_json(url, headers/auth, body)
```

or at least carefully quote/sanitize all shell args before passing them to `execShell`.

## Suggested next-session skills

- Use `diagnose` if the local build still fails after switching to Rosetta-backed Colima, or if JNI/plugin loading crashes.
- Use `context-mode`/`ctx_execute` for build logs, logcat, `rg` output, or other large output.

## Immediate next step

Run APK build under the Rosetta Colima profile:

```bash
cd ~/projects/rockbox
rm -rf android/build
./tools/docker-local-build-y1.sh 240p
```

If it succeeds, install/test the APK on device, then begin JNI/plugin changes at the paths above.
