// Nubble Lighthouse: opaque scene in a display-contact 4x6 frame.
// Units: millimetres. Export `part` one component at a time.

$fn = 48;
part = "assembly"; // contact_frame, island, breaker, foreground, assembly

frame_w = 152.4;  // 6 in hidden magnetic insert
frame_h = 101.6;  // 4 in hidden magnetic insert
visible_w = 139.7; // 5.5 in landscape door window
visible_h = 88.9;  // 3.5 in landscape door window
frame_t = 0.8; // matte-black PLA; directly against the P4 face

module ring_2d() {
    difference() {
        square([frame_w, frame_h], center = true);
        square([visible_w, visible_h], center = true);
    }
}

module contact_frame() {
    // No front magnets or pods. The closed door and wood sight-mat retain
    // this thin ring against the display and hide it in the glass margin.
    linear_extrude(height = frame_t) ring_2d();
}

// AI-separated assets are intentionally independent opaque pieces. The small clear
// supports required for isolated pieces live behind them in the magnetic frame.
module island_layer() {
    // Registered to the source photo: the island shoreline falls just below
    // the LCD horizon rather than floating in the upper sky.
    translate([-53, -14, frame_t])
        resize([106, 24, 0])
            linear_extrude(height = 0.8)
                import("../assets/nubble-island-mask.svg");
}

module breaker_layer() {
    // The isolated rock sits under the central breaking wave, not as a second
    // large island. Its source footprint is deliberately small.
    translate([-12, -25, frame_t + 1.2])
        resize([24, 5, 0])
            linear_extrude(height = 0.8)
                import("../assets/nubble-breaker-mask.svg");
}

module foreground_layer() {
    translate([-66.5, -48, frame_t + 2.4])
        resize([133, 27, 0])
            linear_extrude(height = 1.2)
                import("../assets/nubble-foreground-mask.svg");
}

module assembly() {
    color("#1b1b1b") contact_frame();
    color("#101010") island_layer();
    color("#161616") breaker_layer();
    color("#070707") foreground_layer();
}

if (part == "contact_frame") contact_frame();
if (part == "island") island_layer();
if (part == "breaker") breaker_layer();
if (part == "foreground") foreground_layer();
if (part == "assembly") assembly();
