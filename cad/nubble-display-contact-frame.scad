// Luminary Nubble display-contact frame: thin 4x6 in black PLA ring.
// It is captured by the closed door/sight-mat; no front magnets are used.

outer_w = 152.4;  // 6 in
outer_h = 101.6;  // 4 in
opening_w = 139.7; // 5.5 in visible field
opening_h = 88.9;  // 3.5 in visible field
thickness = 0.8;

linear_extrude(height = thickness)
    difference() {
        square([outer_w, outer_h], center = true);
        square([opening_w, opening_h], center = true);
    }
