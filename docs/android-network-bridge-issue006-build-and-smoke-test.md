# Android network bridge issue006 build + smoke-test procedure

Status: documentation added; device execution **NOT RUN** in this environment.

This document covers only the issue006 documentation pass:

- building a patched Y1 Android APK
- safe/non-destructive install attempts
- explicit opt-in destructive system-app replacement
- backup/rollback of the original system APK
- request/download probe setup and smoke-test steps
- logcat checks

It does **not** claim on-device execution was performed here.

## Preconditions

- Repo root: `rockbox/`
- Target device: rooted Innioasis Y1 Android build
- Existing Rockbox Y1 install is expected to already provide helpers such as `/data/data/gocurl` and `/data/data/cacert.pem`
- `adb` available on the host

## Recommended build path

For local iteration, use the repo's Docker wrapper.

### 360p APK

```bash
cd /Users/hkfuertes/projects/rockbox
./tools/docker-local-build-y1.sh 360p
```

Expected artifact:

```text
android/build/rockbox_360p.apk
```

### 240p APK

```bash
cd /Users/hkfuertes/projects/rockbox
./tools/docker-local-build-y1.sh 240p
```

Expected artifact:

```text
android/build/rockbox_240p.apk
```

Notes:

- Use `240p` or `360p` for APK-only work.
- Do **not** use `all` unless update ZIP artifacts are explicitly needed.
- If an old build tree is corrupted, clear it first with `rm -rf android/build`.

## Equivalent manual compile steps

These are the explicit commands based on `android/building.md`.

```bash
cd /Users/hkfuertes/projects/rockbox/android
./installToolchain.sh
export ANDROID_SDK_PATH="$HOME/android-sdk"
export ANDROID_NDK_PATH="$HOME/android-ndk-r10e"
mkdir -p build
cd build
../../tools/configure --target=201 --lcdwidth=480 --lcdheight=360 --type=n
make
make classes
make zip
make unsigned-apk
APKSIGNER=""
if command -v apksigner >/dev/null 2>&1; then
    APKSIGNER="apksigner"
elif [ -n "$ANDROID_SDK_PATH" ] && [ -d "$ANDROID_SDK_PATH/build-tools" ]; then
    LATEST_BUILD_TOOLS="$(ls -1 "$ANDROID_SDK_PATH/build-tools" | tail -1)"
    if [ -n "$LATEST_BUILD_TOOLS" ] && [ -f "$ANDROID_SDK_PATH/build-tools/$LATEST_BUILD_TOOLS/apksigner" ]; then
        APKSIGNER="$ANDROID_SDK_PATH/build-tools/$LATEST_BUILD_TOOLS/apksigner"
    fi
fi
if [ -z "$APKSIGNER" ]; then
    echo "apksigner not found; install Android build-tools or add apksigner to PATH" >&2
    exit 1
fi
$APKSIGNER sign --key ../platform.pk8 --cert ../platform.x509.pem rockbox_unsigned.apk
cp rockbox_unsigned.apk rockbox.apk
cp rockbox.apk rockbox_360p.apk
```

For 240p, change the configure step to:

```bash
../../tools/configure --target=201 --lcdwidth=320 --lcdheight=240 --type=n
```

After signing, name the artifact to match the later install/push examples:

- 360p manual build: `android/build/rockbox_360p.apk`
- 240p manual build: `android/build/rockbox_240p.apk`

For a 240p manual build, use:

```bash
cp rockbox.apk rockbox_240p.apk
```

If you leave the file as `android/build/rockbox.apk`, substitute that exact filename in every later `adb install` / `adb push` example.

## Safe / non-destructive install path

Try this first. It does **not** overwrite `/system/app/org.rockbox.apk`.

```bash
adb install -r android/build/rockbox_360p.apk
```

or:

```bash
adb install -r android/build/rockbox_240p.apk
```

What this is good for:

- quick package/installability checks
- confirming whether the APK can be side-loaded at all

What this does **not** do:

- replace the privileged system copy under `/system/app/`
- guarantee final runtime parity with the production system-app setup

If install fails due to signature or `sharedUserId="android.uid.system"` constraints, stop here unless you explicitly opt into the destructive path below.

## Explicit opt-in destructive system-app replacement

Only do this on a rooted test device after taking a backup.

### 1. Backup the original system APK

```bash
adb shell su -c 'mount -o rw,remount /system'
adb shell su -c 'cp /system/app/org.rockbox.apk /sdcard/org.rockbox.apk.backup'
adb pull /sdcard/org.rockbox.apk.backup backups/org.rockbox.apk.backup
```

### 2. Decide whether APK-only replacement is sufficient

APK-only replacement is acceptable only for Java/resource-only verification where you know the installed native payload already matches your build.

If your smoke test touches the request/download bridge, plugins, JNI glue, or any native code, treat APK-only replacement as insufficient. `android/scripts/update.sh` also updates `/system/lib/librockbox.so` and refreshes `/data/data/org.rockbox/lib/*`, so use the opt-in native-payload steps below in that case.

### 3. Optional backup for native-payload replacement

Use this when your test needs updated native libraries in addition to the APK.

```bash
adb shell su -c 'cp /system/lib/librockbox.so /sdcard/librockbox.so.backup'
adb pull /sdcard/librockbox.so.backup backups/librockbox.so.backup
adb shell su -c 'rm -rf /sdcard/org.rockbox.lib.backup && mkdir -p /sdcard/org.rockbox.lib.backup && cp -r /data/data/org.rockbox/lib/. /sdcard/org.rockbox.lib.backup/'
adb pull /sdcard/org.rockbox.lib.backup backups/org.rockbox.lib.backup
```

### 4. Replace the system APK

```bash
adb push android/build/rockbox_360p.apk /sdcard/org.rockbox.new.apk
adb shell su -c 'mount -o rw,remount /system'
adb shell su -c 'cp /sdcard/org.rockbox.new.apk /system/app/org.rockbox.apk'
adb shell su -c 'chmod 644 /system/app/org.rockbox.apk'
adb shell su -c 'chown root:root /system/app/org.rockbox.apk'
```

Use the `240p` APK instead if that is the intended target artifact.

### 5. Optional native-payload replacement (consistent with `android/scripts/update.sh`)

Only do this if you intentionally built or changed native payloads.

```bash
adb push android/build/libs/armeabi /sdcard/rockbox-libs
adb shell su -c 'cp /sdcard/rockbox-libs/librockbox.so /system/lib/librockbox.so'
adb shell su -c 'chmod 644 /system/lib/librockbox.so'
adb shell su -c 'chown root:root /system/lib/librockbox.so'
adb shell su -c 'rm -rf /data/data/org.rockbox/lib && mkdir -p /data/data/org.rockbox/lib'
adb shell su -c 'for f in /sdcard/rockbox-libs/*; do cp "$f" /data/data/org.rockbox/lib/; done'
adb shell su -c 'chmod 777 /data/data/org.rockbox/lib/*'
```

Then reboot:

```bash
adb reboot
```

## Rollback

Restore the original APK backup:

```bash
adb push backups/org.rockbox.apk.backup /sdcard/org.rockbox.apk.backup
adb shell su -c 'mount -o rw,remount /system'
adb shell su -c 'cp /sdcard/org.rockbox.apk.backup /system/app/org.rockbox.apk'
adb shell su -c 'chmod 644 /system/app/org.rockbox.apk'
adb shell su -c 'chown root:root /system/app/org.rockbox.apk'
```

If you also replaced native payloads, restore them before rebooting:

```bash
adb push backups/librockbox.so.backup /sdcard/librockbox.so.backup
adb push backups/org.rockbox.lib.backup /sdcard/org.rockbox.lib.backup
adb shell su -c 'cp /sdcard/librockbox.so.backup /system/lib/librockbox.so'
adb shell su -c 'chmod 644 /system/lib/librockbox.so'
adb shell su -c 'chown root:root /system/lib/librockbox.so'
adb shell su -c 'rm -rf /data/data/org.rockbox/lib && mkdir -p /data/data/org.rockbox/lib'
adb shell su -c 'for f in /sdcard/org.rockbox.lib.backup/*; do cp "$f" /data/data/org.rockbox/lib/; done'
adb shell su -c 'chmod 777 /data/data/org.rockbox/lib/*'
```

Then reboot:

```bash
adb reboot
```

## Launch + logcat smoke test

Clear logs, then watch Rockbox tags:

```bash
adb logcat -c
adb logcat -v time Rockbox:D RockboxService:D RockboxActivity:D *:S
```

In another shell, launch Rockbox:

```bash
adb shell am start -n org.rockbox/.RockboxActivity
```

Useful success indicators:

- `Start RockboxService`
- `extracting resources to SD card` or `extracting resources to internal memory`
- `wifi connect: success`
- `internet connection: success`

Useful failure indicators:

- `Failed to connect to internet`
- `Failed to connect to wifi: ...`
- install/launch failures from `adb` itself

## Probe locations on device

From the Rockbox UI, open:

- `Plugins` -> `Applications` -> `Android request probe`
- `Plugins` -> `Applications` -> `Android download probe`

## Request probe behavior in this branch

There is currently **no request-probe config file parser** in this branch.
The compiled-in request smoke test is effectively:

```text
method: GET
url: https://example.com/rockbox-android-request-probe
header: Accept: text/plain
header: X-Rockbox-Probe: 1
body: (none)
```

Expected probe output fields:

- WiFi connect result
- bridge return code
- HTTP status
- response preview
- bridge error text

Interpretation:

- `rc=0` with `HTTP status=2xx/4xx/5xx` means the bridge itself succeeded
- non-zero `rc` means bridge/JNI/helper failure
- response truncation may report `rc=-5`

## Download probe config example

Create `/sdcard/.rockbox/android_download_probe.cfg` with lines like:

```text
# required
url: https://example.com/files/probe.bin
destination: /sdcard/.rockbox/test-downloads/probe.bin

# optional repeated headers
header: Authorization: Bearer REPLACE_ME
header: Accept: application/octet-stream
header: X-Rockbox-Probe: 1
```

Example without auth:

```text
url: https://example.com/robots.txt
destination: /sdcard/.rockbox/test-downloads/robots.txt
header: Accept: text/plain
```

Push from host:

```bash
adb shell mkdir -p /sdcard/.rockbox
adb push android_download_probe.cfg /sdcard/.rockbox/android_download_probe.cfg
```

Expected download probe output fields:

- config path
- URL
- destination
- whether headers were configured
- WiFi connect result
- bridge return code
- HTTP status
- bridge/filesystem error text

Interpretation:

- `rc=0` and `HTTP status=200` should leave the destination file in place
- `HTTP status=4xx/5xx` should not leave a corrupt final file behind
- helper/filesystem failures should surface in the error text

## Header format examples for request/download bridge callers

The bridge expects newline-separated header strings.
Examples:

```text
Authorization: Bearer REPLACE_ME
Content-Type: application/json
Accept: application/json
```

JSON request body example for request-oriented callers:

```json
{"probe":true,"source":"rockbox-y1"}
```

## Suggested smoke-test matrix

1. Launch Rockbox and confirm service startup in logcat.
2. Run `Android request probe` and capture:
   - WiFi result
   - `rc`
   - HTTP status
   - response preview
   - bridge error text
3. Create `android_download_probe.cfg` and run `Android download probe`.
4. Verify the destination file exists under `/sdcard/.rockbox/...` only on HTTP 2xx success.
5. Re-run with an intentional 404 URL and confirm the final destination file is not kept.
6. If any destructive system replacement was performed, verify rollback steps before leaving the device.

## Validation performed for this documentation pass

Repo-local checks run here:

```bash
python3 tools/test-android-request-bridge-issue003.py
python3 tools/test-android-download-bridge-issue005.py
```

Observed result: both passed.

On-device execution in this environment: **NOT RUN**.
