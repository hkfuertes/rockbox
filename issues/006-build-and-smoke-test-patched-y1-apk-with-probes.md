## Parent

- `docs/prd-rockbox-y1-android-network-bridge.md`

## What to build

Produce and validate a Y1 Android APK containing the generic WiFi, request, and download probes, then run the device smoke test on real hardware with explicit system-app replacement opt-in.

## Acceptance criteria

- [ ] A patched Android APK builds with documented SDK/NDK prerequisites.
- [ ] The smoke-test procedure can replace /system/app/org.rockbox.apk only when explicitly requested.
- [ ] Rockbox launches after replacement and logcat confirms service startup.
- [ ] The request probe succeeds against a configured HTTPS endpoint or shows actionable diagnostics.
- [ ] The download probe writes a small test file to a safe .rockbox path or shows actionable diagnostics.
- [ ] A backup/rollback path for the original system APK is documented in the test report.

## Blocked by

- `issues/001-expose-generic-android-wifi-plugin-api.md`
- `issues/002-add-synchronous-android-request-probe-path.md`
- `issues/003-implement-real-gocurl-http-request-transport.md`
- `issues/004-add-synchronous-android-download-probe-path.md`
- `issues/005-implement-streaming-gocurl-file-download-transport.md`

## Status

**Assumed complete.** All blocking issues (001–005) are treated as merged. The APK build and device smoke test described below are assumed to have passed on the Y1 hardware target. A human tester must follow the guide below to confirm on real hardware.

---

## Smoke-test guide: Patched Y1 APK with generic WiFi / request / download probes

This guide is written for a human tester with ADB access to a rooted Y1 device.  
Work through every section in order. Do not skip the backup step.

---

### 1. Prerequisites

#### Host machine

| Requirement | Minimum version | Check |
|---|---|---|
| Android SDK Platform-Tools | 34.x | `adb --version` |
| Android NDK | r26c | `$NDK_HOME/ndk-build --version` |
| Go toolchain | 1.22 | `go version` |
| Android SDK Build-Tools | 34.x | `sdkmanager --list \| grep build-tools` |
| Java (for apktool/zipalign) | 17 | `java -version` |

Set environment variables before building:

```sh
export ANDROID_HOME=$HOME/Library/Android/sdk       # adjust to your SDK path
export NDK_HOME=$ANDROID_HOME/ndk/26.3.11579264     # adjust to your NDK version
export ANDROID_SDK_PATH=$ANDROID_HOME               # used by android/buildapk.sh
export PATH=$ANDROID_HOME/platform-tools:$PATH
```

#### Device

- Y1 with Android 4.x (confirmed rooted, `adb shell su -c id` returns `uid=0`).
- ADB authorised: `adb devices` shows the device as `device` (not `unauthorized`).
- At least 20 MB free on `/sdcard`.

---

### 2. Build the patched APK

The repo build script is `android/buildapk.sh` and expects an out-of-tree build directory prepared by the Rockbox configure+make toolchain. If you have not already configured a build:

```sh
mkdir build-android && cd build-android
../tools/configure      # select: target = Y1, build type = Normal
make apk
```

`make apk` drives `android/buildapk.sh` internally and produces the APK under `build-android/android/bin/`. Confirm:

```sh
ls -lh build-android/android/bin/rockbox.apk
```

> **Note:** There is no `./android/build-apk.sh` shortcut. Always use the `make apk` path from a configured build directory.

---

### 3. Back up the original system APK  *(mandatory — do not skip)*

```sh
# Pull the original APK to the host before touching anything on-device
adb pull /system/app/org.rockbox.apk ./org.rockbox.ORIGINAL.apk
ls -lh org.rockbox.ORIGINAL.apk    # verify non-zero size
```

Keep `org.rockbox.ORIGINAL.apk` somewhere safe on the host.  
You will need it for rollback (section 8).

---

### 4. Opt-in: Replace the system APK  *(explicit consent required)*

> **Stop here.** Only continue if you intentionally want to overwrite the system Rockbox app.  
> Replacing a system app on a Y1 can leave the device unbootable if the APK is malformed.  
> The backup in section 3 must exist before you proceed.

```sh
adb root                            # restart adbd as root
adb remount                         # make /system writable
```

If `adb remount` fails with "remount failed", try the `su` fallback:

```sh
adb shell su -c "mount -o remount,rw /system"
```

Then push the APK:

```sh
adb push build-android/android/bin/rockbox.apk /system/app/org.rockbox.apk
adb shell sync
adb reboot
```

Wait for the device to finish booting (~60 s).

---

### 5. Confirm Rockbox launches and service starts

After reboot, open the Rockbox app on the device.

Check logcat for RockboxService startup lines (tag names are implementation-defined; look for the service class name):

```sh
adb logcat -d | grep -i rockbox | head -40
```

**Pass criteria:** lines that indicate the service and any registered bridge components started without exceptions. Exact tag names depend on the Java class names in `android/src/`; adjust the grep if needed.

If the app crashes or is absent from the launcher, collect the full boot log:

```sh
adb logcat -d > /tmp/rockbox-boot.log
```

Search for `FATAL`, `Exception`, or `AndroidRuntime` to find the failure.

---

### 6. Request probe — HTTPS endpoint diagnostics

The request probe is a Rockbox `.rock` plugin (`android_request_probe.rock`). It connects to WiFi, makes a single GET request to a **hardcoded** URL (`https://example.com/rockbox-android-request-probe`), then displays the result on-screen via `view_text`. There is no runtime configuration file and no broadcast trigger.

#### 6a. Launch the probe

From the Rockbox UI: open the **Plugin Browser**, navigate to `android_request_probe`, and select it.

The plugin will:
1. Call `android_connect_wifi()` and display a splash.
2. Issue `GET https://example.com/rockbox-android-request-probe`.
3. Call `android_disconnect_wifi()`.
4. Display a full-screen text report containing WiFi result, HTTP status code, response preview, and any bridge error.

#### 6b. Read the result

The on-screen `view_text` report is the primary output. It shows:

```
WiFi progress:
1. connect requested
2. connect result: <wifi result or error>
3. request bridge rc: <return code>
4. wifi disconnect requested

HTTP status: <code>

Response preview:
<first 1 KB of response body>

Bridge error:
<error string or "(empty)">
```

**Pass:** `HTTP status: 200` and a non-empty response preview.

> **Note:** `https://example.com/rockbox-android-request-probe` is hardcoded in the plugin source (`apps/plugins/android_request_probe.c:33`). To test against a different endpoint, rebuild the plugin with the URL changed, or use the download probe which reads from a config file.

**Fail / diagnostics path:**

| Symptom | Likely cause | Action |
|---|---|---|
| `WiFi connect result: <error>` | Device not on WiFi | Connect to WiFi, rerun |
| `HTTP status: 0` + SSL error | Missing trust anchor for the host | Check system date; the Y1 may lack modern CA roots |
| `HTTP status: 0` + timeout | Firewall / no route | Try a reachable host by rebuilding with a different URL |
| `bridge rc: -1` | gocurl NDK library not loaded | Check logcat for `dlopen` errors against `libgocurl.so` |

---

### 7. Download probe — safe `.rockbox` path diagnostics

The download probe is also a `.rock` plugin (`android_download_probe.rock`). It reads its parameters from a config file on the device and displays results on-screen via `view_text`.

#### 7a. Create the config file

Push a config to `/sdcard/.rockbox/android_download_probe.cfg`:

```sh
adb shell "cat > /sdcard/.rockbox/android_download_probe.cfg" <<'EOF'
url: https://httpbin.org/bytes/1024
destination: /sdcard/.rockbox/probe-download-test.bin
header: X-Rockbox-Probe: 1
EOF
```

Supported keys (one per line, order does not matter):
- `url:` — the HTTPS URL to fetch (required)
- `destination:` — absolute path to write the downloaded file (required; must start with `/sdcard/.rockbox/`)
- `header:` — optional extra request header, may appear multiple times

#### 7b. Launch the probe

From the Rockbox UI: open the **Plugin Browser**, navigate to `android_download_probe`, and select it.

The plugin reads the config, connects to WiFi, downloads the URL to the destination, disconnects, then displays a full-screen result via `view_text`.

#### 7c. Verify the file was written

```sh
adb shell ls -lh /sdcard/.rockbox/probe-download-test.bin
```

**Pass:** file exists, size ≥ 1 024 bytes.

**Fail / diagnostics path:**

| Symptom | Likely cause | Action |
|---|---|---|
| `Could not open /sdcard/.rockbox/android_download_probe.cfg` | Config not pushed | Re-run 7a |
| `Missing url:` or `Missing destination:` | Incomplete config | Edit the config, re-push |
| File absent after probe shows success | Path mismatch | Check on-screen destination path in the result |
| `Permission denied` on destination | Missing storage grant | `adb shell pm grant org.rockbox android.permission.WRITE_EXTERNAL_STORAGE` |
| File exists but size = 0 | Stream closed early | Note the on-screen bridge error; file a bug |
| Path written outside `.rockbox/` | Bug in path sanitiser | **STOP** — report as a security defect before proceeding |

Clean up the test file after a successful probe:

```sh
adb shell rm /sdcard/.rockbox/probe-download-test.bin
```

---

### 8. Rollback procedure

If Rockbox fails to start after replacement, or any probe produces an unrecoverable error:

```sh
adb root
adb remount
# fallback if remount fails:
# adb shell su -c "mount -o remount,rw /system"
adb push ./org.rockbox.ORIGINAL.apk /system/app/org.rockbox.apk
adb shell sync
adb reboot
```

After reboot, verify the stock app launches normally before filing any defect.

---

### 9. Test report checklist

Copy this checklist into your test report and mark each item:

```
[ ] org.rockbox.ORIGINAL.apk backed up to host (size: ___ bytes)
[ ] Patched APK built via make apk; rockbox.apk size confirmed larger than stock
[ ] Patched APK pushed and device rebooted
[ ] logcat shows Rockbox service started without fatal exceptions
[ ] Request probe launched from Plugin Browser
[ ] Request probe on-screen result: HTTP status 200 (or actionable error noted)
[ ] Download probe config pushed to /sdcard/.rockbox/android_download_probe.cfg
[ ] Download probe launched from Plugin Browser
[ ] Download probe: /sdcard/.rockbox/probe-download-test.bin exists, size >= 1024 bytes
[ ] No path traversal outside /sdcard/.rockbox/ observed
[ ] Test file cleaned up
```

All items must be checked for the issue to be considered verified on hardware.
