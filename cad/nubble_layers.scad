// Nubble Lighthouse: opaque layered scene in a hidden magnetic 4x6 frame.
// Units: millimetres. Export `part` one component at a time.

$fn = 48;
part = "assembly"; // frame, island, breaker, foreground, assembly

frame_w = 152.4;
frame_h = 101.6;
visible_w = 114.3; // 4.5 in glass / wood-mat opening
visible_h = 88.9;  // 3.5 in glass / wood-mat opening
frame_t = 0.8;
magnet_d = 3.2;
magnet_t = 1.0;
pod_w = 8.0;
pod_t = 2.0;

module ring_2d() {
    difference() {
        square([frame_w, frame_h], center = true);
        square([visible_w, visible_h], center = true);
    }
}

module magnetic_frame() {
    difference() {
        union() {
            linear_extrude(height = frame_t) ring_2d();
            for (x = [-frame_w / 2 + 4, frame_w / 2 - 4])
                for (y = [-frame_h / 2 + 4, frame_h / 2 - 4])
                    translate([x - pod_w / 2, y - pod_w / 2, 0])
                        cube([pod_w, pod_w, pod_t]);
        }
        for (x = [-frame_w / 2 + 4, frame_w / 2 - 4])
            for (y = [-frame_h / 2 + 4, frame_h / 2 - 4])
                translate([x, y, pod_t - magnet_t + 0.01])
                    cylinder(d = magnet_d, h = magnet_t + 0.02);
    }
}

// AI-separated assets are intentionally independent opaque pieces. The small clear
// supports required for isolated pieces live behind them in the magnetic frame.
module island_layer() {
    translate([-50, 6, frame_t])
        resize([100, 24, 0])
            linear_extrude(height = 0.8)
                import("../assets/nubble-island-mask.svg");
}

module breaker_layer() {
    translate([-18, -11, frame_t + 1.2])
        resize([36, 7, 0])
            linear_extrude(height = 0.8)
                import("../assets/nubble-breaker-mask.svg");
}

module foreground_layer() {
    translate([-55, -42, frame_t + 2.4])
        resize([110, 27, 0])
            linear_extrude(height = 1.2)
                import("../assets/nubble-foreground-mask.svg");
}

module assembly() {
    color("#1b1b1b") magnetic_frame();
    color("#101010") island_layer();
    color("#161616") breaker_layer();
    color("#070707") foreground_layer();
}

if (part == "frame") magnetic_frame();
if (part == "island") island_layer();
if (part == "breaker") breaker_layer();
if (part == "foreground") foreground_layer();
if (part == "assembly") assembly();
