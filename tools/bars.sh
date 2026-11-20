#!/bin/zsh
# HUD geometry probe and shift-light previews, drawn in picture buffer 6 (126 x 77 on this car) through the CarPlay plugin's
# test channel (hudcmd.sh). Each screen stays up until the next one or hud-clear.txt. The distance text shows the frame's
# inset or the stage number. Colours from the palette probe: 6 white (day and night), 8 orange (day and night).
#
#   bars.sh frame [inset=0]          outline of the whole buffer (3 px edges), or of the buffer inset by <inset> px
#   bars.sh stage <1|2|3> [x0 x1 y0] M-style preview along the top: 1 short orange bars at both ends, 2 longer, 3 one white
#                                    bar across; drawn between x0..x1 (default 0..125) from y0 (default 0)
#   bars.sh print ...                print the batch instead of sending it
set -e
cd "$(dirname "$0")"
B=0006
SEND=1
[ "$1" = print ] && { SEND=0; shift; }

rect() { # rect <item> <x0> <y0> <x1> <y1>: AddPolygon(6, item, filled, 4 points); coordinates are s16 (negative allowed)
  local x0=$(( $2 & 0xffff )) y0=$(( $3 & 0xffff )) x1=$(( $4 & 0xffff )) y1=$(( $5 & 0xffff ))
  printf '21d %s%04x0304%04x%04x%04x%04x%04x%04x%04x%04x\n' $B $1 $x0 $y0 $x1 $y0 $x1 $y1 $x0 $y1
}
text() { # text <digits>: SetText(distance 3, UTF-16BE)
  local t="" ch
  for ch in ${(s::)1}; do t+=$(printf '%04x' $(( ch + 48 ))); done
  printf '210 000300%s0000\n' $t
}
head_() { # panel, buffer and distance on; buffer emptied; fill colour <c>; no outline
  printf '204 000101\n204 %s01\n204 000301\n220 %s\n21b %s%02x\n219 %s00\n' $B $B $B $1 $B
}

case "$1" in
frame)
  i=${2:-0}
  x0=$i; y0=$i; x1=$(( 125 - i )); y1=$(( 76 - i ))
  batch=$(head_ 6
    rect 0 $x0 $y0 $x1 $(( y0 + 2 ))
    rect 1 $x0 $(( y1 - 2 )) $x1 $y1
    rect 2 $x0 $y0 $(( x0 + 2 )) $y1
    rect 3 $(( x1 - 2 )) $y0 $x1 $y1
    text $i)
  ;;
ruler)  # the top row (y 0..3): a long orange bar x 14..73, then five short segments (white, orange, white, orange, white)
        # x 76-83, 86-93, 96-103, 106-113, 116-125: how many show = where the visible area ends on the right at the top
  batch=$(head_ 8
    rect 0 14 0 73 3
    printf '21b %s06\n' $B; rect 1 76 0 83 3
    printf '21b %s08\n' $B; rect 2 86 0 93 3
    printf '21b %s06\n' $B; rect 3 96 0 103 3
    printf '21b %s08\n' $B; rect 4 106 0 113 3
    printf '21b %s06\n' $B; rect 5 116 0 125 3
    text 5)
  ;;
mask)   # does a dark colour cover white? a white bar x 21..95 (item 0), then a bar of colour <c> over x 36..80 (item 1)
  m=${2:-7}
  batch=$(head_ 6
    rect 0 21 0 95 5
    printf '21b %s%02x\n' $B $m; rect 1 36 0 80 5
    text $m)
  ;;
order)  # stacking by item number or by send order? black mask x 47..69 as item 5 first, then a white bar x 21..95 as
        # item 2: a gap = by item number (5 above 2); one whole white bar = by send order
  batch=$(printf '220 %s\n21b %s07\n' $B $B
    rect 5 47 0 69 5
    printf '21b %s06\n' $B; rect 2 21 0 95 5
    text 8)
  ;;
twobars) # both stage 2 bars as ONE 8-point polygon joined by a zero-width slit along the top edge:
          # (21,0) (95,0) (95,5) (70,5) (70,0) (46,0) (46,5) (21,5). Two bars = non-convex fill works.
  batch=$(printf '220 %s\n21b %s06\n' $B $B
    printf '21d %s00000308%04x%04x%04x%04x%04x%04x%04x%04x%04x%04x%04x%04x%04x%04x%04x%04x\n' $B \
      21 0 95 0 95 5 70 5 70 0 46 0 46 5 21 5
    text 9)
  ;;
unmask) # ImageRemoveItem(6, item 1): the mask goes, the white bar should be whole again
  batch=$(printf '21f %s0001\n' $B)
  ;;
above)  # can anything draw above the buffer's top? white bar at y -8..-3 and the orange reference bar at y 0..5, x 21..95
  batch=$(head_ 6
    rect 0 21 -8 95 -3
    printf '21b %s08\n' $B; rect 1 21 0 95 5
    text 8)
  ;;
stage)  # environment: C1/C2/C3 colour of stage 1/2/3 (C12 sets 1 and 2; defaults 8, 8, 6), P1/P2 bar length per side in %,
        # H1/H2/H3 bar height in px (defaults 4, 4, 6)
  s=$2; x0=${3:-0}; x1=${4:-125}; y0=${5:-0}
  c12=${C12:-8}; c1=${C1:-$c12}; c2=${C2:-$c12}; c3=${C3:-6}; p1=${P1:-25}; p2=${P2:-45}
  h1=${H1:-4}; h2=${H2:-4}; h3=${H3:-6}
  w=$(( x1 - x0 + 1 ))
  case $s in
  1) n=$(( w * p1 / 100 )); batch=$(head_ $c1; rect 0 $x0 $y0 $(( x0 + n - 1 )) $(( y0 + h1 - 1 )); rect 1 $(( x1 - n + 1 )) $y0 $x1 $(( y0 + h1 - 1 )); text 1) ;;
  2) n=$(( w * p2 / 100 )); batch=$(head_ $c2; rect 0 $x0 $y0 $(( x0 + n - 1 )) $(( y0 + h2 - 1 )); rect 1 $(( x1 - n + 1 )) $y0 $x1 $(( y0 + h2 - 1 )); text 2) ;;
  3) batch=$(head_ $c3; rect 0 $x0 $y0 $x1 $(( y0 + h3 - 1 )); text 3) ;;
  *) echo "stage 1, 2 or 3"; exit 1 ;;
  esac
  ;;
*) sed -n '2,12p' "$0"; exit 1 ;;
esac
print -r -- "$batch"
[ $SEND = 1 ] && ./hudcmd.sh <<< "$batch" && echo "sent"
exit 0
