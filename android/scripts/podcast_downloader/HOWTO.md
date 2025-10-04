1. Add your feeds to `feeds.txt`
2. Push `feeds.txt` to .rockbox
```
adb push feeds.txt /sdcard/.rockbox
```
3. Push scripts to .rockbox
```
adb push podcast-parser.sh /sdcard/.rockbox
adb push podcast-downloader.sh /sdcard/.rockbox
```
4. Push `wget` binary to /system/xbin
```
adb remount
adb push wget /system/xbin/wget_new
adb shell chown root:root /system/xbin/wget_new
adb shell chmod 655 /system/xbin/wget_new
```
5. Modify "Debug SysCall" to execute `podcast-parser.sh`
```
sed -i '/system("sleep 1");/c\
splash(HZ, "Downloading Podcasts");\
system("sh /sdcard/.rockbox/podcast-parser.sh &");' ../../../apps/menus/main_menu.c

sed -i 's/\*: "Debug SysCall"/\*: "Sync Podcasts"/g' ../../../apps/lang/english.lang
```
6. Build and transfer to device as usual
