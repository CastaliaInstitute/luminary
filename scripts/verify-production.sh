#!/usr/bin/env bash
set -euo pipefail

# Verify the committed Luminary production-candidate package without mutating it.
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

required=(
  scenes/nubble-aligned/source.png
  scenes/nubble-aligned/background.png
  scenes/nubble-aligned/island.png
  scenes/nubble-aligned/breaker.png
  scenes/nubble-aligned/foreground.png
  renders/nubble-concept.png
  renders/stl/luminary-7in-p4-backplate.stl
  renders/stl/luminary-7in-p4-fit-check.stl
  renders/stl/luminary-front-door-reference.stl
  renders/stl/nubble-basrelief-island.stl
  renders/stl/nubble-basrelief-breaker.stl
  renders/stl/nubble-basrelief-foreground.stl
)

for file in "${required[@]}"; do
  [[ -s "$file" ]] || { echo "missing or empty: $file" >&2; exit 1; }
done

for image in source background island breaker foreground; do
  dimensions="$(identify -format '%w %h' "scenes/nubble-aligned/$image.png")"
  [[ "$dimensions" == '1024 600' ]] || {
    echo "invalid canvas for $image: $dimensions (expected 1024 600)" >&2
    exit 1
  }
done

# Recompile from the canonical source masks, then require the reference-photo
# overlay used for visual validation. This deliberately avoids validating
# against the generated LCD background.
scripts/compile-bas-relief.sh nubble-aligned >/dev/null
reference_dimensions="$(identify -format '%w %h' scenes/nubble-aligned/compiled/reference-alignment.png)"
[[ "$reference_dimensions" == '1024 600' ]] || {
  echo "invalid reference-alignment canvas: $reference_dimensions (expected 1024 600)" >&2
  exit 1
}

blender_bin="${BLENDER_BIN:-/Applications/Blender.app/Contents/MacOS/Blender}"
[[ -x "$blender_bin" ]] || { echo "Blender not found: $blender_bin" >&2; exit 1; }

"$blender_bin" --background --factory-startup --python-expr 'exec("import bpy,bmesh,glob,os,sys\nroot=\"renders/stl\"\nnames={\"luminary-7in-p4-backplate.stl\",\"luminary-7in-p4-fit-check.stl\",\"luminary-front-door-reference.stl\",\"nubble-basrelief-island.stl\",\"nubble-basrelief-breaker.stl\",\"nubble-basrelief-foreground.stl\"}\nfailed=False\nfor name in sorted(names):\n    bpy.ops.object.select_all(action=\"SELECT\")\n    bpy.ops.object.delete(use_global=False)\n    bpy.ops.wm.stl_import(filepath=os.path.join(root,name))\n    bm=bmesh.new(); bm.from_mesh(bpy.context.selected_objects[0].data)\n    nonmanifold=sum(not e.is_manifold for e in bm.edges); boundary=sum(e.is_boundary for e in bm.edges)\n    print(f\"STL_VERIFY {name} nonmanifold={nonmanifold} boundary={boundary}\")\n    failed=failed or nonmanifold != 0 or boundary != 0\n    bm.free()\nsys.exit(1 if failed else 0)")'

echo 'Luminary production-candidate verification passed.'
