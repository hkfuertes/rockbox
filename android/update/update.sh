mount -o rw,remount /system
cd /sdcard/.rockbox/update
cp /sdcard/.rockbox/update/libs/armeabi/librockbox.so /system/lib/
chmod 644 /system/lib/librockbox.so 
chown root:root /system/lib/librockbox.so
rm -rf /data/data/org.rockbox/lib
mkdir /data/data/org.rockbox/lib
for f in /sdcard/.rockbox/update/libs/armeabi/*; do
  cp "$f" /data/data/org.rockbox/lib/
done
chmod 777 /data/data/org.rockbox/lib/*
rm -rf .rockbox
unzip libs/armeabi/libmisc.so
for f in .rockbox/*; do
  cp -rf "$f" /sdcard/.rockbox/
done
rm /system/app/org.rockbox.apk
cp /sdcard/.rockbox/update/rockbox.apk /system/app/org.rockbox.apk
chmod 644 /system/app/org.rockbox.apk
chown root:root /system/app/org.rockbox.apk
reboot
