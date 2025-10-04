#!/bin/bash

# Specify the input file
input_file="/sdcard/.rockbox/feeds.txt"

# indicate start with double vibration
echo 100 > /sys/class/timed_output/vibrator/enable
# Read the file line by line
while IFS= read -r line; do
    # Ignore commented lines
    if echo "$line" | grep -qv '^#'; then
        # Parse the line using IFS (Internal Field Separator)
        DOWNLOAD_PATH=$(echo "$line" | cut -d';' -f1)
        NAME=$(echo "$line" | cut -d';' -f2)
        LAST_N=$(echo "$line" | cut -d';' -f3)
        FEED_URL=$(echo "$line" | cut -d';' -f4)

        # Print or use the extracted values
        echo "DOWNLOAD_PATH: $DOWNLOAD_PATH"
        echo "NAME: $NAME"
        echo "LAST_N: $LAST_N"
        echo "FEED_URL: $FEED_URL"
        echo "-----------"
        sh /sdcard/.rockbox/podcast-downloader.sh -u "$FEED_URL" -d "$DOWNLOAD_PATH/$NAME" -n $LAST_N
    fi
done < "$input_file"

# indicate finish with triple vibration
echo 100 > /sys/class/timed_output/vibrator/enable
sleep 1
echo 100 > /sys/class/timed_output/vibrator/enable
sleep 1
echo 100 > /sys/class/timed_output/vibrator/enable
