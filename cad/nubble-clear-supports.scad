// Minimal clear-PETG supports for the shallow Nubble contact stack.
// The traces begin beneath the hidden 4x6 contact-frame margin; no magnets
// and no full transparent carrier sheet are used.

trace_w = 0.45; // two 0.2 mm nozzle lines
trace_t = 0.40;
post_w = 0.70;

module trace(x, y, length, vertical = false) {
    if (vertical)
        translate([x - trace_w/2, y, 0]) cube([trace_w, length, trace_t]);
    else
        translate([x, y - trace_w/2, 0]) cube([length, trace_w, trace_t]);
}

module post(x, y, height) {
    translate([x - post_w/2, y - post_w/2, 0]) cube([post_w, post_w, height]);
}

// The breaker is isolated over open water. A left-edge trace hides beneath
// the black contact-frame margin, then two small posts meet its rear face.
trace(-76.2, -12.0, 43.0);
post(-35.0, -12.0, 3.55);
post(-22.0, -12.0, 3.55);

// Foreground boulders lead into the lower frame margin. These short traces
// are hidden by the wood sight-mat / black contact ring when the door closes.
trace(-40.0, -44.45, 10.0, vertical = true);
trace(43.0, -44.45, 10.0, vertical = true);
post(-40.0, -35.0, 5.35);
post(43.0, -35.0, 5.35);
