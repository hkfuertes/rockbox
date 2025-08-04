#!/usr/bin/env python3
"""
Cue to Bookmark Converter for Rockbox

This script parses .cue files and adds chapters as bookmarks to .bmark files.
It can either create new .bmark files or append to existing ones.

To obtain a .cue file from an audiobook with embedded chapters (m4b) you will need ffmpeg and something like ff2cue:
    ffmpeg -loglevel quiet -i Audiobooks/SomeBook/SomeBook.m4b -f ffmetadata - | ff2cue - SomeBook.cue -y SomeBook.m4b
Usage:
    python3 cue_to_bookmark.py <cue_file> [bmark_file] [options]

Examples:
    python3 cue_to_bookmark.py SomeBook.cue SomeBook.bmark --directory /sdcard/Audiobooks/SomeBook
"""

import os
import sys
import argparse
import re
from pathlib import Path
from typing import List, Dict, Optional, Tuple

class CueParser:
    """Parser for .cue files"""

    def __init__(self):
        self.tracks = []
        self.audio_file = ""
        self.title = ""
        self.performer = ""

    def parse_cue_file(self, cue_file_path: str) -> bool:
        """Parse a .cue file and extract track information"""
        try:
            with open(cue_file_path, 'r', encoding='utf-8') as f:
                content = f.read()
        except UnicodeDecodeError:
            try:
                with open(cue_file_path, 'r', encoding='latin-1') as f:
                    content = f.read()
            except Exception as e:
                print(f"Error reading cue file: {e}")
                return False

        lines = content.split('\n')
        current_track = None

        for line in lines:
            line = line.strip()
            if not line:
                continue

            # Parse FILE command
            if line.startswith('FILE '):
                match = re.match(r'FILE\s+"([^"]+)"', line)
                if match:
                    self.audio_file = match.group(1)

            # Parse TITLE command (global or track-specific)
            elif line.startswith('TITLE '):
                match = re.match(r'TITLE\s+"([^"]+)"', line)
                if match:
                    title = match.group(1)
                    if current_track is None:
                        self.title = title
                    else:
                        current_track['title'] = title

            # Parse PERFORMER command (global or track-specific)
            elif line.startswith('PERFORMER '):
                match = re.match(r'PERFORMER\s+"([^"]+)"', line)
                if match:
                    performer = match.group(1)
                    if current_track is None:
                        self.performer = performer
                    else:
                        current_track['performer'] = performer

            # Parse TRACK command
            elif line.startswith('TRACK '):
                match = re.match(r'TRACK\s+(\d+)', line)
                if match:
                    track_num = int(match.group(1))
                    current_track = {
                        'number': track_num,
                        'title': '',
                        'performer': '',
                        'offset': 0
                    }
                    self.tracks.append(current_track)

            # Parse INDEX 01 command (track start time)
            elif line.startswith('INDEX 01 '):
                if current_track is not None:
                    match = re.match(r'INDEX 01\s+(\d+):(\d+):(\d+)', line)
                    if match:
                        minutes = int(match.group(1))
                        seconds = int(match.group(2))
                        frames = int(match.group(3))
                        # Convert to milliseconds (frames are 1/75th of a second)
                        offset_ms = (minutes * 60 + seconds) * 1000 + (frames * 1000 // 75)
                        current_track['offset'] = offset_ms

        return len(self.tracks) > 0

class BookmarkParser:
    """Parser for .bmark files"""

    def __init__(self):
        self.bookmarks = []

    def parse_bmark_file(self, bmark_file_path: str) -> bool:
        """Parse an existing .bmark file"""
        try:
            with open(bmark_file_path, 'r', encoding='utf-8') as f:
                lines = f.readlines()
        except FileNotFoundError:
            return True  # File doesn't exist, that's OK
        except Exception as e:
            print(f"Error reading bookmark file: {e}")
            return False

        for line in lines:
            line = line.strip()
            if line:
                self.bookmarks.append(line)

        return True

    def add_bookmark(self, bookmark_entry: str):
        """Add a bookmark entry to the list"""
        self.bookmarks.append(bookmark_entry)

    def write_bmark_file(self, bmark_file_path: str) -> bool:
        """Write bookmarks to a .bmark file"""
        try:
            with open(bmark_file_path, 'w', encoding='utf-8') as f:
                for bookmark in self.bookmarks:
                    f.write(bookmark + '\n')
            return True
        except Exception as e:
            print(f"Error writing bookmark file: {e}")
            return False

class BookmarkGenerator:
    """Generate bookmark entries from cue track information"""

    def __init__(self):
        self.flags = 4  # BM_NAME flag (0x04) for name support

    def create_bookmark_entry(self,
                            track_info: Dict,
                            directory_path: str,
                            audio_filename: str,
                            resume_index: int = 0,
                            resume_seed: int = 0,
                            repeat_mode: int = 0,
                            shuffle: bool = False) -> str:
        """
        Create a bookmark entry in Rockbox format

        Format: >[flags];[resume_index];[offset];[seed];[elapsed];[repeat_mode];[shuffle];[name];[directory_path];[filename]
        """
        # Convert track offset from milliseconds to bytes (approximate)
        # This is a rough estimate - actual byte offset would need audio file analysis
        offset_bytes = track_info['offset'] * 44  # Rough estimate for 44kHz audio

        # Get track name for bookmark
        track_name = track_info.get('title', f'Track {track_info["number"]:02d}')

        # Create the bookmark entry with name support
        entry = f">{self.flags};{resume_index};{offset_bytes};{resume_seed};{track_info['offset']};{repeat_mode};{int(shuffle)};{track_name};{directory_path};{audio_filename}"

        return entry

def main():
    parser = argparse.ArgumentParser(
        description='''Cue to Bookmark Converter for Rockbox

This script parses .cue files and adds chapters as bookmarks to .bmark files.

It can either create new .bmark files or append to existing ones.

To obtain a .cue file from an audiobook with embedded chapters (m4b) you will need ffmpeg and something like ff2cue:
    ffmpeg -loglevel quiet -i Audiobooks/SomeBook/SomeBook.m4b -f ffmetadata - | ff2cue - SomeBook.cue -y SomeBook.m4b ''',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s SomeBook.cue SomeBook.bmark --directory /sdcard/Audiobooks/SomeBook
        """
    )

    parser.add_argument('cue_file', help='Path to the .cue file')
    parser.add_argument('bmark_file', nargs='?', help='Path to the .bmark file (optional)')
    parser.add_argument('--audio-file', help='Audio file name (if not specified in .cue)')
    parser.add_argument('--directory', help='Directory path for bookmarks')
    parser.add_argument('--append', action='store_true', help='Append to existing .bmark file')
    parser.add_argument('--create-chapter-names', action='store_true',
                       help='Create descriptive chapter names in comments')
    parser.add_argument('--resume-index', type=int, default=0,
                       help='Playlist resume index (default: 0)')
    parser.add_argument('--repeat-mode', type=int, default=0,
                       help='Repeat mode (default: 0)')
    parser.add_argument('--shuffle', action='store_true',
                       help='Enable shuffle mode')

    args = parser.parse_args()

    # Validate input file
    if not os.path.exists(args.cue_file):
        print(f"Error: Cue file '{args.cue_file}' not found")
        sys.exit(1)

    # Parse cue file
    cue_parser = CueParser()
    if not cue_parser.parse_cue_file(args.cue_file):
        print("Error: Failed to parse cue file or no tracks found")
        sys.exit(1)

    print(f"Found {len(cue_parser.tracks)} tracks in cue file")

    # Determine output file
    if args.bmark_file:
        bmark_path = args.bmark_file
    else:
        # Create .bmark file with same name as .cue file
        cue_path = Path(args.cue_file)
        bmark_path = cue_path.with_suffix('.bmark')

    # Determine audio file name
    audio_filename = args.audio_file if args.audio_file else cue_parser.audio_file
    if not audio_filename:
        print("Error: No audio file specified. Use --audio-file option or ensure .cue file contains FILE command")
        sys.exit(1)

    # Determine directory path
    directory_path = args.directory if args.directory else ""
    if not directory_path:
        # Try to extract from cue file path
        cue_dir = os.path.dirname(os.path.abspath(args.cue_file))
        if cue_dir:
            directory_path = cue_dir

    # Parse existing bookmark file if appending
    bookmark_parser = BookmarkParser()
    if args.append and os.path.exists(bmark_path):
        if not bookmark_parser.parse_bmark_file(bmark_path):
            print("Error: Failed to parse existing bookmark file")
            sys.exit(1)
        print(f"Loaded {len(bookmark_parser.bookmarks)} existing bookmarks")

    # Generate bookmark entries for each track
    bookmark_generator = BookmarkGenerator()

    for i, track in enumerate(cue_parser.tracks):
        # Create bookmark entry
        bookmark_entry = bookmark_generator.create_bookmark_entry(
            track_info=track,
            directory_path=directory_path,
            audio_filename=audio_filename,
            resume_index=args.resume_index,
            resume_seed=0,
            repeat_mode=args.repeat_mode,
            shuffle=args.shuffle
        )

        # Add comment if requested
        if args.create_chapter_names:
            track_title = track.get('title', f'Track {track["number"]:02d}')
            track_performer = track.get('performer', cue_parser.performer)
            if track_performer:
                comment = f" # {track_performer} - {track_title}"
            else:
                comment = f" # {track_title}"
            bookmark_entry += comment

        bookmark_parser.add_bookmark(bookmark_entry)

        # Print track info
        track_title = track.get('title', f'Track {track["number"]:02d}')
        minutes = track['offset'] // 60000
        seconds = (track['offset'] % 60000) // 1000
        print(f"  Track {track['number']:02d}: {track_title} ({minutes:02d}:{seconds:02d})")

    # Write bookmark file
    if bookmark_parser.write_bmark_file(bmark_path):
        print(f"\nSuccessfully created bookmark file: {bmark_path}")
        print(f"Total bookmarks: {len(bookmark_parser.bookmarks)}")
    else:
        print("Error: Failed to write bookmark file")
        sys.exit(1)

if __name__ == "__main__":
    main()
