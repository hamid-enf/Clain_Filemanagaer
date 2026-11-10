#!/bin/sh
# How test/vectors/png/*.png were made (ImageMagick 6.9, `convert`).
#
# The files are checked in; this script is the record of where they came
# from.  `plasma:` is only reproducible within one ImageMagick version, so
# re-running it elsewhere gives different pictures -- then take the new
# CRC table from `python3 tools/png_ref.py <dir>` into test/main.c
# (k_vec in test_png_formats).
#
#   sh tools/gen_png_vectors.sh /tmp/pngvec
set -e
out=${1:?usage: gen_png_vectors.sh <output dir>}
tools=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
cd "$out"

convert -seed 7 -size 37x29 plasma:fractal -depth 8 -strip PNG24:src.png
convert src.png -alpha set -channel A -fx 'i/w' +channel -strip PNG32:srca.png
convert -seed 11 -size 211x157 plasma:fractal -blur 0x2 -depth 8 -strip PNG24:bigsrc.png

# g <name> <input> <convert options...>: writes <name>.png and what
# ImageMagick itself decodes from it as <name>.rgb (RGB888).
g() {
    n=$1; i=$2; shift 2
    convert "$i" "$@" -strip "$n.png"
    convert "$n.png" -depth 8 "rgb:$n.rgb"
}
ct() { echo "-define png:color-type=$1 -define png:bit-depth=$2"; }

g rgb8_a7       src.png  $(ct 2 8)  -interlace PNG
g rgba8_a7      srca.png $(ct 6 8)  -interlace PNG
g rgb16         src.png  -depth 16 -blur 0x1.2 $(ct 2 16)
g rgba16_a7     srca.png -depth 16 -blur 0x1.2 $(ct 6 16) -interlace PNG
g gray1         src.png  -colorspace Gray -threshold 50% $(ct 0 1)
g gray2_a7      src.png  -colorspace Gray -depth 2 $(ct 0 2) -interlace PNG
g gray4         src.png  -colorspace Gray -depth 4 $(ct 0 4)
g gray8_a7      src.png  -colorspace Gray $(ct 0 8) -interlace PNG
g gray16        src.png  -depth 16 -blur 0x1.2 -colorspace Gray $(ct 0 16)
g ga8           srca.png -colorspace Gray $(ct 4 8)
g ga16_a7       srca.png -depth 16 -blur 0x1.2 -colorspace Gray $(ct 4 16) -interlace PNG
g pal1_a7       src.png  -colors 2   $(ct 3 1) -interlace PNG
g pal2          src.png  -colors 4   $(ct 3 2)
g pal4_a7       src.png  -colors 16  $(ct 3 4) -interlace PNG
g pal8_a7       src.png  -colors 200 $(ct 3 8) -interlace PNG
g big_rgb8_a7   bigsrc.png $(ct 2 8) -interlace PNG
g tall_gray4_a7 src.png  -resize '5x41!' -colorspace Gray -depth 4 $(ct 0 4) -interlace PNG

rm -f src.png srca.png bigsrc.png
echo "vectors in $out; table:"
python3 "$tools/png_ref.py" .
