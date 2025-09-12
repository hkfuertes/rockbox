#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Rockbox Theme Rescaler
---------------------
Author: thinkVHS (updated)
Date: 2025-09-03

Description:
Rescales BMP images and coordinates in .wps, .sbs, .fms files
of a Rockbox theme. Copies other files without modifying them.
"""

import argparse
from pathlib import Path
from PIL import Image
from tqdm import tqdm
import shutil
import re

def resize_bmp(input_path, output_path, factor, filter_bg, filter_icon):
    """
    Rescale a BMP image. If detected as an icon (≤32px), NEAREST is used;
    otherwise, the configured filter is applied.
    """
    img = Image.open(input_path)
    new_size = (int(img.width * factor[0]), int(img.height * factor[1]))

    if img.width <= 32 or img.height <= 32:
        resized = img.resize(new_size, filter_icon)
    else:
        resized = img.resize(new_size, filter_bg)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    resized.save(output_path)

def scale_value(val, factor):
    val = val.strip()
    if val == '-' or val.endswith('%'):
        return val  # do not rescale special values
    try:
        return str(int(int(val) * factor))
    except ValueError:
        return val

def rescale_wps_file(file_path, out_path, factor_x, factor_y):
    """
    Rescale coordinates inside .wps, .sbs, or .fms files.
    """
    try:
        text = file_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = file_path.read_text(encoding="latin-1")

    patterns = {
        "%V":  ["x","y","width","height","fontid"],
        "%Vl": ["id","x","y","width","height","fontid"],
        "%Vi": ["label","x","y","width","height","fontid"],
        "%dr": ["x","y","width","height","colour1","colour2"],
        "%pb": ["x","y","width","height","filename"],
        "%x":  ["label","filename","x","y"],
        "%xl": ["label","filename","x","y","nimages"],
        "%Cl": ["xpos","ypos","maxwidth","maxheight","halign","valign"],
        "%T":  ["label","x","y","width","height","action","options"],
        "%Lb": ["viewport","width","height","tile"],
        "%XX": ["x","y","width","height","filename","options"]
    }

    for key, params in patterns.items():
        regex = re.compile(rf"{key}\((.*?)\)")
        def repl(match):
            parts = [p.strip() for p in match.group(1).split(",")]
            for i, name in enumerate(params):
                if i < len(parts):
                    if name in ["x","y","width","height","xpos","ypos","maxwidth","maxheight"]:
                        factor = factor_x if "x" in name else factor_y
                        parts[i] = scale_value(parts[i], factor)
            return f"{key}({','.join(parts)})"
        text = regex.sub(repl, text)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(text, encoding="utf-8")

def main():
    parser = argparse.ArgumentParser(description="Rescale BMP images and WPS coordinates of a Rockbox theme.")
    parser.add_argument("input_dir", help="Input theme folder (e.g., MyTheme_240p)")
    parser.add_argument("input_res", choices=["240p", "360p"], help="Input resolution")
    parser.add_argument("output_res", choices=["240p", "360p"], help="Output resolution")
    parser.add_argument("--filter", choices=["NEAREST", "LANCZOS"], default="LANCZOS",
                        help="Filter for large images (default: LANCZOS)")
    args = parser.parse_args()

    input_dir = Path(args.input_dir)
    if not input_dir.exists():
        raise FileNotFoundError(f"The folder {input_dir} does not exist")

    output_dir = input_dir.parent / f"{input_dir.name}_{args.output_res}"
    output_dir.mkdir(exist_ok=True)

    res_map = {"240p": (320, 240), "360p": (480, 360)}
    in_w, in_h = res_map[args.input_res]
    out_w, out_h = res_map[args.output_res]
    factor_x = out_w / in_w
    factor_y = out_h / in_h

    FILTER_BG = Image.NEAREST if args.filter.upper() == "NEAREST" else Image.LANCZOS
    FILTER_ICON = Image.NEAREST

    files = list(input_dir.rglob("*"))
    with tqdm(total=len(files), desc="Processing files") as pbar:
        for file in files:
            if not file.is_file():
                pbar.update(1)
                continue

            rel_path = file.relative_to(input_dir)
            out_file = output_dir / rel_path

            # TODO: filter for specific icons and scale these to multiples of 10px (or however many images it contains)
            # Maybe parse these icons from .wps
            if file.suffix.lower() == ".bmp":
                resize_bmp(file, out_file, (factor_x, factor_y), FILTER_BG, FILTER_ICON)

            elif file.suffix.lower() in [".wps", ".sbs", ".fms"]:
                rescale_wps_file(file, out_file, factor_x, factor_y)

            else:
                out_file.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(file, out_file)

            pbar.update(1)

if __name__ == "__main__":
    main()
