# Nubble source run

The Nubble Lighthouse scene is represented by three separate AI-assisted visual assets.
These are the canonical references for the next silhouette revision; do not collapse them
into a generic one-piece trace.

| Layer | Asset | Use |
| --- | --- | --- |
| LCD sky and water | `assets/display-nubble-1280x720.png` | 1280 × 720 display background |
| Distant island and lighthouse | `assets/source/nubble-island-ai.jpg` | Main black island / white-building tracing reference |
| Foreground boulders | `assets/source/nubble-foreground-rocks-ai.png` | Separate raised black foreground tracing reference |

`assets/source/nubble-background-ai.jpg` is retained as the uncropped background source.

## Layering rule

The island output intentionally contains no foreground boulders. The foreground-rock output
intentionally contains no island, lighthouse, or sky. Keep them as independent physical
layers, held by the hidden ring and narrow clear traces where a rock is isolated. This
preserves open water on the display between the foreground and the island.
