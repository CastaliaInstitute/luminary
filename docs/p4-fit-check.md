# Luminary P4 physical fit check

Run this check before printing or installing the structural rear plate. It
validates the specific Waveshare ESP32-P4-WIFI6-Touch-LCD-7B board used by
Luminary; do not substitute the earlier portrait 5-inch P4 board.

## Evidence used by the CAD

The Waveshare outline drawing specifies a 164.00 x 97.00 mm board and a
164.28 x 99.17 mm LCD/bezel envelope. The rear plate's 166.0 x 101.6 mm
registration land therefore leaves 0.86 mm clearance at each long side and
1.215 mm at each short side of the bezel. The low-material fit ring uses the
same nominal corner-hole centers as the rear plate: +/-78 mm X and +/-44 mm Y,
with 6.0 x 5.2 mm horizontal M2.5 tolerance slots.

Source: [Waveshare ESP32-P4-WIFI6-Touch-LCD-7B outline drawing](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-7b.htm).

## Print

Print [luminary-7in-p4-fit-check.stl](../renders/stl/luminary-7in-p4-fit-check.stl)
flat in the intended rear-plate material at 0.20 mm layers. It is deliberately
only 1 mm thick and consumes little material. Do not scale it.

## Acceptance criteria

1. The board perimeter must sit entirely inside the 166.0 x 101.6 mm ring
   without forcing or bowing the printed plastic.
2. Each of the four board mounting holes must accept an M2.5 screw through its
   corresponding test-ring slot with at least 0.5 mm of unforced adjustment in
   both axes.
3. With all four screws finger-tightened, the LCD/bezel must lie flat and
   centered; no corner may lift more than 0.3 mm from the registration land.
4. Connect the selected right-side USB-C extension internally and route its
   external cable through both the side and bottom-center 8 mm plate exits.
   The connector and cable must not bear against the PCB, display flex, or
   screw head.
5. Confirm the selected 6 x 2 mm magnets sit below the rear plate surface and
   that their polarity attracts the intended steel-frame plates.

If any criterion fails, measure the actual board-hole coordinates from the
board center, edit the constants at the top of
`cad/luminary-7in-backplate.scad` and
`cad/luminary-7in-p4-fit-check.scad` together, then re-export both STLs.
Never enlarge a completed structural plate by drilling; that can reduce the
wall around its cable exits or magnet pockets.
