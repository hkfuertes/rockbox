# Rockbox Android Fork for Innioasis Y1

| <img src="./img/240p_menu.png" alt="Rockbox-240p Menu" width="75%"/> | <img src="./img/240p_wps.png" alt="Rockbox-240p WPS" width="75%"/> |
|:--:|:--:|
| Rockbox Menu | Rockbox WPS |

## General Information

This is an experimental build of Rockbox that is not in any way associated with or developed by Innioasis/Timmkoo.

Most of the work was already done by the original Rockbox team - all credits to them. 

Additionally, I want to thank Mitxela for allowing me to use the intro from his beautiful ["Ode to Rockbox" Video](https://www.youtube.com/watch?v=Qw-VvGsYpSU) as the boot animation of this ROM. You can support him on [Patreon](https://www.patreon.com/mitxela) or via [Paypal](https://paypal.me/mitxela).

Do NOT run this if you don't know what you are doing. You might brick your device in the process of installing this app. You have been warned.

### Donations

If you would like to support the development of Rockbox for the Y1 financially, consider donating to one of the following causes instead:
- The hungarian prime minister is trying to criminalize every queer person in Hungary and by extension Europe. To support the people standing up against this human rights violation (10000 HUF = 25€): https://budapestpride.hu/en/one-time-donation-paypal/
- Partners in Health: https://www.pih.org/maternal-center-excellence
- The maintainers of the original Rockbox project: https://www.rockbox.org/

## Controls

Most controls are identical to the iPod 6G, refer to this [manual](https://download.rockbox.org/daily/manual/rockbox-ipod6g.pdf)

Exceptions:
- Center (Short): Accept / Enter
- Center (Long): Open Context Menu
- Center (Very Long): Turn Off Screen
- Play/Pause (Short, Text Input): Open Keyboard
- Holding Next + Previous (>5 seconds): Restart Rockbox (Thanks, u/thinkVHS!)

## Dual Boot: How to switch between Rockbox and Stock
### In Rockbox
- Main Menu > System > Reboot to Stock Firmware

### In Stock
- Hold Menu/Back + Play/Pause for ~15 seconds

## Themes

### Installation

1. (optional) Download the fontpack, extract it, drag the .rockbox folder onto your device https://www.rockbox.org/dl.cgi?bin=fonts
2. Download a theme from (360p see below, 240p: https://themes.rockbox.org/index.php?target=ipod6g)
3. extract it
4. drag the .rockbox folder onto your device

### List of 360p Themes

- Theme Pack with included Fonts: https://github.com/rockbox-y1/themes/releases/latest
- ipodmod3blk-y1: https://github.com/AkikoKumagara/ipodmod3blk-y1
- AdwaitaPod-Arcticons (AdwaitaPod Dark Simplified): https://codeberg.org/joelchrono/AdwaitaPod-Arcticons-rockbox-360p/releases/

## Installation
1. Download the latest Rockbox `update.zip` here: https://github.com/rockbox-y1/rockbox/releases
2. Connect your Y1 and copy `update.zip` to `.rockbox/update.zip`
3. Safely disconnect your Y1
4. Go to `Main Menu > System` and click `Firmware Update`
5. The update process will now run in the background and automatically restart the device once it is done

**Note:** If this is the first time you install Rockbox on your Y1, if you don't see the `Firmware Update` option in `Main Menu > System`, or if just want to have a fresh install follow the steps for `Initial Installation` below.

## Initial Installation
### MTKClient

1. Install MTKClient: https://github.com/bkerler/mtkclient
2. Download the latest Rockbox included firmware (rom.zip) here: https://github.com/rockbox-y1/rockbox/releases 
3. Unpack the archive:
```
mkdir rom && cd rom
unzip ../rom.zip
```
4. Turn of the device, disconnect from the PC
5. Start the flashing process:
```
cd rom
python ../mtk.py w logo,uboot,bootimg,recovery,android,usrdata logo.bin,lk.bin,boot.img,recovery.img,system.img,userdata.img
```
6. Connect the device via USB
7. Unplug the device when the process has finished
8. Power on the device

### SPFlash Tool

1. Download the latest Rockbox included firmware (rom.zip) here: https://github.com/rockbox-y1/rockbox/releases
2. Unpack the archive
3. Install SP Flash Tool v5.1904: https://spflashtool.com/download/
4. Start SP Flash Tool
5. Follow the [official firmware flashing instructions](https://support.innioasis.com/download/flashing_tutorial/Flashing_tutorial-Y1_EN%20v2.0.7-20241021.pdf) but use the MT6572_Android_scatter.txt from the rom.zip file

### Manual installation (for developers)

If, despite all warnings, you still want to try installing it manually you need to do the following:

- have an Innioasis Y1 with ADB enabled
- root the device (see https://xdaforums.com/t/root-framaroot-a-one-click-apk-to-root-some-devices.2130276/):
```
wget https://supersuroot.org/downloads/supersu-2-82.apk
adb install supersu-2-82.apk
adb install Framaroot-1.9.3.apk
adb shell monkey -p com.alephzain.framaroot -c android.intent.category.LAUNCHER 1
# wait for it to start
adb shell input keyevent DPAD_DOWN
adb shell input keyevent DPAD_CENTER
# wait for the success message
adb shell monkey -p eu.chainfire.supersu -c android.intent.category.LAUNCHER 1
# follow instructions, once done
adb reboot
```
- Reboot device
```
adb reboot
```
- Update keymap file to be able to navigate systems menus
```
adb remount
adb push android/scripts/Rockbox.kl /system/usr/keylayout/Generic.kl Generic.kl
adb shell chmod 644 /system/usr/keylayout/Generic.kl
adb reboot
```
- Download the latest Rockbox release APK from the sidebar
- Download the latest Rockbox libs.tar.gz and unpack it (`tar -xzf libs.tar.gz`) such that there is a folder called `libs/armeabi`
- Either use one of the preinstalled themes or supply your own in the .rockbox folder on the SD card
- Uninstall any apps you do not want
```
# list packages
adb shell pm list packages
# uninstall package
adb uninstall <package>
# or if that fails
adb shell pm disable-user <package>
```
- Uninstall old Rockbox version (can be skipped if v0.1 was never installed)
```
adb uninstall org.rockbox
```
- Install rockbox as system app:
```
adb remount
adb push rockbox-[release].apk /system/app/org.rockbox.apk
adb shell chmod 644 /system/app/org.rockbox.apk
adb shell chown root:root /system/app/org.rockbox.apk
adb push libs/armeabi/librockbox.so /system/lib/
adb shell chmod 644 /system/lib/librockbox.so
adb shell chown root:root /system/lib/librockbox.so
adb shell rm -rf /data/data/org.rockbox/lib
adb shell mkdir /data/data/org.rockbox/lib
adb push libs/armeabi/* /data/data/org.rockbox/lib/.
```
- Restart the device, choose Rockbox as launcher when asked
```
adb reboot
```
- (optional) Download the voice pack from the releases, extract it, drag the .rockbox folder onto your device
- **If rockbox gets stuck at the Rockbox logo and doesn't load your theme please delete .rockbox/config.cfg on your SD card**

## Collection of technical information and quirks
### Keylock exemptions are weird
Sometimes keylock exemptions take some time to work or need a second press. It seems like this is a limitation that will be hard to fully solve without the touch wheel driver sources or access to the AOSP source used for this ROM. Once the device is in sleep mode for a while the touch wheel simply stops sending button presses to the kernel. This is likely a power-saving setting that an app has no control over. Feel free to contribute a fix for this if you have one.

### Overwriting files usually found in .rockbox (language files and others)
To for example edit `.lang` files you will want to access the respective files, usually found .rockbox. Currently these files are not populated in the .rockbox folder on the SD card of the device but instead on the internal storage (`/data/data/org.rockbox/app_rockbox/rockbox/langs`) . If you place these files in the .rockbox folder however Rockbox will use these instead of the files in the internal storage.

**So where can you find and edit these files easily?**
- download `update.zip` of the Rockbox release you are using
- extract `update.zip`
- rename `/data/data/org.rockbox/app_rockbox/rockbox/langs/libmisc.so` to libmisc.zip and extract it
- modify the files in the .rockbox folder you just extracted from `libmisc` and place them in the .rockbox folder on the SD card
- restart Rockbox
