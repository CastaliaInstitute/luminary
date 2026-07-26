// Fast physical fit-check for the Luminary P4 rear mount.
// Print at 1 mm thick before committing to the structural rear backplate.

$fn = 20;

fit_w = 166.0;
fit_h = 101.6;
frame_w = 4.0;
fit_t = 1.0;
slot_dx = 78.0;
slot_dy = 44.0;
slot_len = 6.0;
slot_d = 5.2;

module pill_slot(length, diameter) {
    hull() {
        translate([-(length - diameter) / 2, 0]) circle(d = diameter);
        translate([(length - diameter) / 2, 0]) circle(d = diameter);
    }
}

difference() {
    linear_extrude(height = fit_t)
        difference() {
            square([fit_w, fit_h], center = true);
            square([fit_w - 2 * frame_w, fit_h - 2 * frame_w], center = true);
        }
    for (x = [-slot_dx, slot_dx])
        for (y = [-slot_dy, slot_dy])
            translate([x, y, -0.01])
                linear_extrude(height = fit_t + 0.02) pill_slot(slot_len, slot_d);
}
