#!/bin/bash
#
# podcast-downloader
# Version: 1.0.1-y1
# Description: A tool to download podcasts from an RSS feed.
#
# Copyright (C) 2025 Willem L. Middelkoop (mail@willem.com)
# > Adjusted for Rockbox on Innioasis Y1 <
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# For more information, visit: https://source.willem.com/podcast-downloader/

# Default configurations
LAST_N=-1
PODCAST_DIR="/sdcard/Podcasts"    # Default directory for podcast downloads
RSS_FEED=""                       # RSS feed URL (to be provided by the user)
TMP_DIR="/sdcard/tmpdir"            # Temporary directory for processing
mkdir $TMP_DIR

# Function to print usage information
usage() {
    echo "Usage: $0 -u <RSS_FEED_URL> [-d <DOWNLOAD_DIR>] [-n <LAST_N>]"
    echo
    echo "Options:"
    echo "  -u  Specify the RSS feed URL (required)."
    echo "  -d  Specify the directory for downloads (default: ~/Podcasts)."
    echo "  -n  Specify how many recent episodes will be downloaded (0=None, -1=All, default: -1)."
    echo "  -h  Show this help message."
    exit 1
}

# Parse command-line arguments
while getopts ":u:d:n:h" opt; do
    case $opt in
        u) RSS_FEED="$OPTARG" ;;
        d) PODCAST_DIR="$OPTARG" ;;
        n) LAST_N="$OPTARG" ;;
        h) usage ;;
        \?) echo "Invalid option: -$OPTARG" >&2; usage ;;
        :) echo "Option -$OPTARG requires an argument." >&2; usage ;;
    esac
done

# Check if RSS feed URL is provided
if [ -z "$RSS_FEED" ]; then
    echo "Error: RSS feed URL is required."
    usage
fi

# Ensure the download directory exists
mkdir -p "$PODCAST_DIR"

# Start of the process, output to standard output
echo "Podcast Downloader started at $(date)"
echo "Downloading from RSS feed: $RSS_FEED"

# Download the RSS feed
RSS_FILE="$TMP_DIR/rss_feed.xml"
wget_new --no-check-certificate -q -O "$RSS_FILE" "$RSS_FEED"

if [ $? -ne 0 ]; then
    echo "Error: Failed to download RSS feed. Check the URL."
    echo "Aborting."
    rm -rf "$TMP_DIR"
    exit 1
fi

# Extract MP3 (or other media file) URLs from the RSS feed
# =======================================================
# The following line uses `awk` to parse the RSS feed and extract URLs 
# from the <enclosure> tags. These tags typically specify the location 
# of podcast media files (e.g., .mp3, .m4a).
#
# - The field separator (-F) is set to match the portion of the <enclosure> tag 
#   up to the 'url="' attribute, effectively splitting each matching line into fields.
#   The second field ($2) contains everything after 'url="'.
# - We use `split($2, arr, "\"")` to break this field at the next double quote (")
#   so that `arr[1]` contains the actual URL.
# - The `awk` command ensures that only the URL portion is output for further processing.
#
# Note: This parsing assumes that the <enclosure> tag contains a valid 'url' attribute 
# and follows common XML formatting conventions. This approach may fail on malformed or 
# unusual RSS feeds, so adjustments may be needed for specific edge cases.

# Parse the RSS feed for media URLs and process them one at a time
downloaded_count=0
awk -F'enclosure[^>]*url="' '/<enclosure/ {split($2, arr, "\""); print arr[1]}' "$RSS_FILE" | while read -r URL; do

    if [[ "$LAST_N" -ne -1 ]] && [[ "$downloaded_count" -ge "$LAST_N" ]]; then
        echo "Already downloaded $LAST_N episodes."
        break
    fi
	# Extract 10 lines before and after the URL for context
	# 'grep -B 10 -A 10' is used here to extract the 10 lines before and after the MP3 URL
    # This provides context to locate the title and pubDate that are typically
    # located near the MP3 URL in the RSS feed, but this assumption is not guaranteed.
    CONTEXT=$(grep -B 10 -A 10 "$URL" "$RSS_FILE")
	# Get or construct the following fields from the context
    TITLE=$(echo "$CONTEXT" | awk -F'<title>|</title>' '/<title>/ {print $2; exit}')
    PUB_DATE=$(echo "$CONTEXT" | awk -F'<pubDate>|</pubDate>' '/<pubDate>/ {print $2; exit}')
    echo $PUB_DATE
    DATE=$(echo "$PUB_DATE" | awk '{print $3 "-" $2 "-" $4}' | sed 's/Sep/09/;s/Jan/01/;s/Feb/02/;s/Mar/03/;s/Apr/04/;s/May/05/;s/Jun/06/;s/Jul/07/;s/Aug/08/;s/Oct/10/;s/Nov/11/;s/Dec/12/')
	echo $DATE

	# Escape title to remove problematic characters and normalize
	ESCAPED_TITLE=$(echo "$TITLE" | sed 's/[^a-zA-Z0-9_-]/_/g')

	# Remove (possible) double underscores
	ESCAPED_TITLE=$(echo "$ESCAPED_TITLE" | sed 's/__/_/g')

	# Remove (possible) trailing underscores
	ESCAPED_TITLE=$(echo "$ESCAPED_TITLE" | sed 's/_$//')
	# Extract the file extension from the URL (default to mp3 if not found)
    EXTENSION=$(echo "$URL" | grep -o '\.[[:alnum:]_]*$')
	EXTENSION=${EXTENSION:-mp3}

	# Construct the filename with the dynamic extension
	FILENAME="${DATE}_${ESCAPED_TITLE}.${EXTENSION}"
    FILENAME="$(echo "$FILENAME" | sed 's/[_]\{2,\}/_/g')"

    if [ ! -f "$PODCAST_DIR/$FILENAME" ]; then
        echo "Downloading: $FILENAME"
        wget_new --no-check-certificate -O "$PODCAST_DIR/$FILENAME" "$URL"

        if [ $? -eq 0 ]; then
            echo "Download completed: $FILENAME"
            ((downloaded_count++))
        else
            echo "Failed to download: $URL"
        fi
    else
        echo "Already exists: $FILENAME"
        ((downloaded_count++))
    fi
done

# Clean up
rm -rf "$TMP_DIR"

# Generate playlist
ls "$PODCAST_DIR" | grep -E "\.mp3|\.ogg" | awk -F'[_-]' '{printf "%04d-%02d-%02d_%s\n", $3, $1, ($2 < 10 ? "0" $2 : $2), $0}' | sort | cut -d'_' -f2- > "$PODCAST_DIR/$(basename "$PODCAST_DIR").m3u"

# End of the process
echo "Podcast Downloader finished at $(date)"
