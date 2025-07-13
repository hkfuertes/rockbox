# Rockbox Android Fork for Innioasis Y1

## General Information
This is an experimental build of Rockbox. 

Most of the work was already done by the original Rockbox team - all credits to them. 

I mostly added quick hacks to make this usable on a device without any touch inputs.

Do NOT run this if you don't know what you are doing. You might brick your device in the process of installing this app. You have been warned.

If you still want to try you need to do the following:

- have an Innioasis Y1 with ADB enabled
- download the latest release APK
- `adb install rockbox-[release].apk`
- either use one of the preinstalled themes or supply your own in the rockbox folder on the SD card
- restart the device, choose Rockbox as launcher when asked

## Changes
- remapped controls
- enable seek forward/backward by holding next/previous media keys
- change default theme to an adjusted version of MacClassic https://themes.rockbox.org/index.php?themeid=3104
- dark theme variant of MacClassic
- playlist creation without keyboard
- option to copy playlists found on the device to the playlist menu (Settings > Database > Copy Playlists on Scan)
- display brightness settings (Settings > Brightness)
- disable broken menu items
- external app launcher
- haptic scroll wheel vibration (Settings > Wheel Vibration Intensity)

## Known issues
- after initializing or updating the DB you need to restart Rockbox using adb:
```
adb shell am force-stop org.rockbox
adb shell monkey -p org.rockbox -c android.intent.category.LAUNCHER 1
```
- themes from other devices are often broken and too small
- Rockbox might randomly crash (especially after USB file transfers) - restart your device or Rockbox via:
```
adb shell monkey -p org.rockbox -c android.intent.category.LAUNCHER 1
```

## Planned
Ordered by priority

### Soon/Mid-term

#### UI/UX
- Menu item to restart the Rockbox app (for easier DB updates)

#### Themes
- fix more ipod classic themes to work on this port

### Unknown/Long-term
As a hosted app rockbox lacks permissions to do these things. The following features are likely not doable in the near future.

#### UI/UX
- hold center button to turn screen off
- device shutdown/restart feature

#### Settings Menus
- screen timeout
- bluetooth
- wifi

#### Connectivity
- fetch podcasts via rss
