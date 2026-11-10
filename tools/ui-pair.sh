#!/usr/bin/env bash
# Side-by-side of a reference image and the live surface at full resolution.
# Usage: tools/ui-pair.sh <reference.png> <live.png|--capture>
#          [--region 'X,Y WxH' | --ref-region 'X,Y WxH' --live-region 'X,Y WxH'] -o <out-dir>
# Regions are in each image's own pixels. Exit 1 when a crop is blank or a region is out of bounds.
set -euo pipefail

die() { echo "ui-pair: $*" >&2; exit 1; }
usage() { sed -n '3,5p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

# Below this normalized std dev (0..1) a crop is one flat colour.
BLANK_STD=0.004

[[ $# -ge 2 ]] || usage
ref=$1 live=$2; shift 2
ref_region= live_region= out=
while [[ $# -gt 0 ]]; do
  case $1 in
    --region) ref_region=$2 live_region=$2; shift 2 ;;
    --ref-region) ref_region=$2; shift 2 ;;
    --live-region) live_region=$2; shift 2 ;;
    -o) out=$2; shift 2 ;;
    *) usage ;;
  esac
done
[[ -n $out ]] || usage
[[ -f $ref ]] || die "no reference file: $ref"
mkdir -p "$out"

if [[ $live == --capture ]]; then
  live="$out/live-full.png"
  grim "$live"
fi
[[ -f $live ]] || die "no live file: $live"

size() { identify -format '%wx%h' "$1[0]"; }

# crop <in> <region|''> <out> <label>
crop() {
  local in=$1 region=$2 dst=$3 label=$4 w h
  IFS=x read -r w h <<<"$(size "$in")"
  if [[ -z $region ]]; then
    magick "$in[0]" -alpha off "$dst"
    return
  fi
  [[ $region =~ ^([0-9]+),([0-9]+)\ ([0-9]+)x([0-9]+)$ ]] || die "bad $label region '$region' (want 'X,Y WxH')"
  local x=${BASH_REMATCH[1]} y=${BASH_REMATCH[2]} cw=${BASH_REMATCH[3]} ch=${BASH_REMATCH[4]}
  (( cw > 0 && ch > 0 && x + cw <= w && y + ch <= h )) ||
    die "$label region '$region' is outside the ${w}x${h} image $in"
  magick "$in[0]" -alpha off -crop "${cw}x${ch}+${x}+${y}" +repage "$dst"
}

stats() {
  identify -format '%wx%h mean=rgb(%[fx:round(255*mean.r)],%[fx:round(255*mean.g)],%[fx:round(255*mean.b)]) std=%[fx:standard_deviation]' "$1"
}

check_blank() {
  local std
  std=$(identify -format '%[fx:standard_deviation]' "$1")
  awk -v s="$std" -v t="$BLANK_STD" 'BEGIN { exit !(s < t) }' &&
    die "$2 crop is blank (std dev $std < $BLANK_STD): wrong region or empty surface. Crop: $1"
  return 0
}

echo "reference source: $ref $(size "$ref")"
echo "live source:      $live $(size "$live")"
crop "$ref" "$ref_region" "$out/ref.png" reference
crop "$live" "$live_region" "$out/live.png" live
check_blank "$out/ref.png" reference
check_blank "$out/live.png" live

rs=$(size "$out/ref.png") ls=$(size "$out/live.png")
if [[ $rs != "$ls" ]]; then
  echo "scale: reference crop $rs -> live crop $ls"
  magick "$out/ref.png" -resize "${ls}!" "$out/ref.png"
fi

echo "ref:  $(stats "$out/ref.png")"
echo "live: $(stats "$out/live.png")"

label() {
  magick "$1" -background '#202020' -fill white -gravity North -splice 0x32 \
    -pointsize 20 -annotate +0+6 "$2" "$3"
}
label "$out/ref.png" "reference: $(basename "$ref")" "$out/.ref-l.png"
label "$out/live.png" "live: $(basename "$live")" "$out/.live-l.png"
magick "$out/.ref-l.png" "$out/.live-l.png" -background '#202020' +smush 8 "$out/pair.png"
rm -f "$out/.ref-l.png" "$out/.live-l.png"

# compare exits 1 when images differ, 2 on error.
rc=0
ae=$(compare -metric AE -fuzz 5% "$out/ref.png" "$out/live.png" "$out/diff.png" 2>&1) || rc=$?
(( rc < 2 )) || die "compare failed: $ae"
total=$(identify -format '%[fx:w*h]' "$out/live.png")
awk -v a="${ae%% *}" -v t="$total" 'BEGIN { printf "diff: %d of %d px differ by >5%% (%.1f%%)\n", a, t, 100*a/t }'
echo "pair: $out/pair.png"
echo "diff: $out/diff.png"
