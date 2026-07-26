# Nubble source run

The Nubble Lighthouse scene is represented by four separate AI-assisted visual assets.
These are the canonical references for the next silhouette revision; do not collapse them
into a generic one-piece trace.

| Layer | Asset | Use |
| --- | --- | --- |
| LCD sky and water | `assets/display-nubble-1280x720.png` | 1280 × 720 display background |
| Distant island and lighthouse | `assets/source/nubble-island-ai.jpg` | Main black island / white-building tracing reference |
| Middle breaker rock | `assets/source/nubble-breaker-rock-ai.png` | Shallow black rock under the crashing wave |
| Foreground boulders | `assets/source/nubble-foreground-rocks-ai.png` | Separate raised black foreground tracing reference |

`assets/source/nubble-background-ai.jpg` is retained as the uncropped background source.

## Layering rule

The island output intentionally contains no foreground boulders. The breaker-rock output
provides the low physical target for the wave. The foreground-rock output intentionally
contains no island, lighthouse, or sky. Keep all three as independent physical layers, held
by the hidden ring and narrow clear traces where a rock is isolated. This preserves open
water on the display between the foreground, breaker, and island.

## Printable extraction

`cad/nubble_layers.scad` turns the approved AI separations into four print-ready meshes:

- `renders/stl/nubble-magnetic-frame.stl` — hidden 6 × 4 in magnetic carrier with four 3.2 mm magnet pockets
- `renders/stl/nubble-island-layer.stl` — distant island and lighthouse mask
- `renders/stl/nubble-breaker-layer.stl` — the single middle rock where the wave breaks
- `renders/stl/nubble-foreground-layer.stl` — near boulders

The first is installed behind the wood mat; the three opaque masks stack toward the glass.
The LCD remains the fourth visual layer (sky and water), 20 mm behind the printed scene.
`renders/nubble-concept.png` is the Blender render made by importing those exact STL files.
