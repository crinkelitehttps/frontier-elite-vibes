# Native model cache

The native scene (`src/gl/gl_native.cpp`) used to run every object's model bytecode again for each game frame,
because the run's output was in camera space: the object's pose went in at the start. With the Detail slider
high (`native_detail_boost`), far objects take their detailed branches and those runs got long; the picture
that picked up a new game frame took much longer than the ones in between, so motion was uneven while the fps
counter stayed high.

Now each object's model is run **once in its own axes** (origin 0, unit axes, the same 2^s) and the result is
kept (`Cached`, keyed by object address + whether it is a shadow-casting run). Each later frame only places it
with the object's pose (`Place`: view = pos + R v) while what the run decided still holds.

## What a run depends on, and how each is handled

| Input | Handling |
|---|---|
| Model, table, scale `s`, detail options, `native_detail_boost`, light tints, cloud-shadow setting | `CacheKey`: any change runs again |
| Object variables (`Object.vars`) | only the ones the run read (`Rec.vread`); bit tests `$13`/`$14` on a variable keep the answer, not the value (`Rec.bits`) |
| Temporaries left at the hook (`Object.t`) | only those whose value reached a read (`Run.tsrc` follows copies down to sub-models; `Rec.tread`) |
| Text (`Frame.texts`) | the strings drawn (`Rec.texts`) |
| Distance tests `if z` (`$0b`/`$0c`) | kept with their limit and answer (`Rec.dist`), asked again each frame |
| Facing tests (`$0b`/`$0c` with a normal, text) | `Rec.facing` |
| `ifdist` screen-size tests (`$17`) | `Rec.apart` |
| Shades used for colours (`$1a`, text) | `Rec.shade` (facing + light tint) |
| Light direction used for placement (`msetup`, cloud shadows) | `Rec.light`: runs again when the light turns about 0.06 degrees relative to the object |
| Which faces face the camera, their shade | worked out each frame per face (`CFace`, `face_colour`); triangles carry their face index as colour |
| Circles, exhausts (sizes in pixels) | per frame (`CCircle`/`circle_eval`, `CTear`/`tear_eval`) |
| Depth-sort keys | from the kept key points (`Node.kpt`, `kcam` for `$7fd`/`$7fe` keys measured from the camera) |
| Random vertices (types 7/8), the camera matrix (`msetup` bit 15) | *volatile*: a sub-model that uses them is cut out and run each frame on its own (`Rec.live`); a top-level model that uses them runs every frame as before (ships' exhausts) |

The ship-shadow casting runs go through the same cache, and the shadow casters are only gathered once per game
frame (`gl_shadow_again` re-queues the map pass for further pictures of the same frame).

Fixed on the way: `Runner::unplace` (parent vertices passed to sub-models) treated the game's axes as unit
length; they are 127/128 long, so those vertices came out 1.6% short (seen on station bays' ground plates).

## Switches and checks

- `GLF_NATIVE_CACHE=0`: no cache, every model runs every frame as before.
- `GLF_NATIVE_CACHE=2`: logs, every 300 pictures, which models ran again and why (`native cache runs`), the
  time per part of `prepare` and each `prepare` over 15 ms.
- `GLF_NATIVE_CACHE=3`: self-check: every cached object is also run the old way and the two results compared
  (counts, colours, points); `native cache check: N same, M different`, and the first differing triangle.
  Mars (landed, launch), Merlin (launch) and Lave (docked, launch) at detail 8: all the same.
- `GLF_FRAME_PROF=1`: `frameprof.csv` has `native_ms`, `native_max_ms` (worst picture), `native_ran`,
  `native_reused` per game frame; the log has a `PROF native models` line.

## Measured (Renoir APU laptop, test instance, Mars, detail 8)

Bytecode running went from about a third of the render thread to about 4%. The whole `prepare` (models +
shadows) went from 9.5 to about 7.5 ms per game frame. What is left is placing the kept triangles on the CPU
each frame (~127k at detail 8), the span loop, the shadow casters' triangles (a ground base is a town), and the
driver copying all vertices every picture. Those need stage 2: the kept triangles in GPU buffers, placed by a
matrix in the shader, shadows drawn from the same buffers.

## Why spinning is still choppy (measured 2026-11-06)

The FPS counter the player sees is the game's own (game frames; turning it on lifts the game's 50/s limit).
What matters is the render thread's pictures (`GLF_FRAME_LOG=1`: `frames:` line). Mars, landed, this laptop
(Renoir APU), the user's settings:

| Detail | Pictures/s | Game frames shown of ~50/s | ms per picture (window) |
|---|---|---|---|
| 0 | 60 (vsync) | 49.6 | 10.5 |
| 3 | 32 | 31 | 29 (sky stage 12, native draw 10.5) |
| 8 | 27 | 25 | 35 (sky 15.6, native 12.6) |

Above detail 0 the drawing cannot keep up, frames are dropped unevenly: jumps when turning fast.
Both halves grow with the triangle count: `prepare` (placing the kept triangles, sorting, shadow casters)
and `gl_draw.c` (every vertex transformed on the CPU into a ~70 byte `GdVert` with clip position, log
depth and three shadow-map coordinates, all of it uploaded again every picture: `put`, `mat4_xform`,
the driver's memcpy).

## Stage 2: kept triangles on the GPU (steps 1-5 done 2026-11-06)

Done: when a cache entry is made, `build_mesh` uploads its triangles once, in the object's axes, to a
`GdMesh` (`gl_draw.c`: x, y, z, face index per vertex), node by node (`Cached.gn`: range, box, faces).
Only nodes that are no underlay and have no cloud-shadow faces go there; the rest stay on the CPU. Each
game frame `cache_place` works out the face colours as before into the face table (`face_rgba`, one
RGBA8 per face, alpha 0 = turned away; `gd_face_colours` uploads it) and gives the object a `GpuDraw`
(pose matrix, its first face-table entry); a GPU node only points at its range (`Node.g`, `gfirst`,
`gcount`, 0 when none of its faces is shown). `draw_gpu` emits `gd_mesh_draw` where the CPU triangles
were drawn (before the node's own CPU triangles: exhausts, live sub-models), in the colour pass and in
the depth-only pass. `gd_mesh_draw` records a `GD_MESH` command with the state `put` would have used
(projection x modelview x pose, log depth constants, shadow rows x pose); consecutive ranges of the same
draw merge, and `glMultiDrawArrays` draws them (a loop on GLES). The vertex shader (`VS_MESH`) reads the
face's colour with `texelFetch` and drops faces turned away; the fragment shader is `FS_FLAT`. Spans
come from each node's box (w is linear: least and most over the box), the shadow candidates' boxes from
the boxes' corners. Wireframe (`R_GLWIRE`) and the self-check place a GPU node's triangles on the CPU
again (`gpu_tris`). `GLF_NATIVE_GPU=0` turns it off (stage 1).

Measured (Mars, landed, same laptop and settings as above, `GLF_FRAME_LOG=1`):

| Detail | Pictures/s, GPU off | GPU on | Game frames shown, on |
|---|---|---|---|
| 3 | 32 | 60 (vsync) | 49.5 |
| 8 | 26.5 | 53 | 49 |

`GLF_NATIVE_CACHE=3` at detail 8: all the same; screenshots with and without match (148 of 2M pixels,
the clouds' animation).

Not done yet: step 6 (shadow casters from the same buffers: the casting runs still place their
triangles on the CPU each game frame), and pictures/s next to the game's FPS counter.

### The plan as written

Goal: picture time at detail 3-8 on this laptop under ~16 ms. Keep the stage 1 cache (`Cached`) as the
source; only what is drawn changes.

1. **Mesh per cache entry**: when an entry is (re)made, upload its triangles once in the object's own
   axes to a VBO: position (3 floats), face index (for colour). Node ranges (first, count per `Node`)
   kept with it. Free on eviction (`cache_trim`) and on re-make.
2. **Per-frame face colours, not per-vertex data**: `face_colour` stays on the CPU (cheap, one per face);
   results go into a small RGBA8 texture (face index -> colour, alpha 0 = faces away). The vertex shader
   reads it with `texelFetch` (GLES 3.0 safe for Android/Quest) and drops culled faces (degenerate
   position). Upload = 4 bytes per face per game frame instead of ~200 bytes per triangle per picture.
3. **Matrix in the shader**: per object a uniform model matrix (the pose, `Place`) times the view/projection
   of the picture; log depth (`depth_a/b`, `log_a/b` in `gl_draw.c put`) and the three shadow-map
   coordinates (`shadow_m*` x pose) computed in the shader from uniforms. Same output as `put`.
4. **Into the draw order as custom commands**: the native scene's order logic (draw_sky's items, objects by
   span, `before_planet`, underlays, depth slices `gd_set_depth`) stays. Where it now calls
   `draw_nodes(d.nodes)` for a cached object, emit one `gd_custom` command holding the mesh, the matrix and
   the node ranges in this picture's order (`sort_nodes_view` still sorts nodes on the CPU: per node, not per
   triangle). The callback binds its own program/VAO and draws the ranges (`glMultiDrawArrays` per state
   run: depth test on/off for underlays, blend for cloud shadows `0x8000`). It must set the state gd_flush
   would (cull, depth mask, colour mask, shadow texture/uniforms) and leave it as custom commands do.
5. **Spans** (`Drawn.near/far`, the per-picture loop over every point in `prepare`): from the mesh's local
   bounding box (8 corners) per node or per object, not every vertex.
6. **Shadow maps**: draw casters from the same VBOs with light-matrix x pose (`gl_shadow.c shadow_pass`),
   instead of gathering and re-transforming their triangles on the CPU each frame. The ground base filter
   (only triangles within 2.5 km cast) becomes per-node bounding boxes.
7. **Unchanged, CPU path**: volatile objects, live sub-models, circles, dots, lines, exhausts, the sky,
   planets, wireframe mode. Small counts.
8. **VR**: both eyes draw the same VBOs with their own matrices: no per-eye vertex work.

Checks: `GLF_NATIVE_CACHE=0` must still draw the old way; add a switch for stage 2 (e.g. `GLF_NATIVE_GPU=0`)
to compare pictures; measure with `GLF_FRAME_LOG=1` at Mars detail 0/3/8 (`new 2; waitgame; wait 12; quit`
from devtools/run, BIN=build-modded). Risks: draw order inside a node (keep the record order in the VBO),
underlays and cloud shadows (blend, no depth write), log depth matching the CPU path exactly (seams between
GPU and CPU-path objects), shadow bias, Android GLES (no TBO, no glMultiDrawArrays in GLES 3.0: loop
glDrawArrays there).

## Flying over a city (measured 2026-11-06, London, Detail x16)

Earth's London (model 48: calls 164, 167 and five 103s, about 13000 faces, 100000 triangles at x16) ran again
on most frames while flying low over it, for the drawn model and again for its shadow (about 8 ms each), and
each run built its mesh again. 28-36 pictures/s on this laptop.

- **Lag** (`cache_lags`): a model of 2000 faces or more whose run no longer holds only for the view (distance,
  size on the screen, a face turning to or from the camera, a shade) is kept up to 8 frames before it runs
  again; its variables, text and the light still run it at once. London: runs on about 13 frames in 60
  instead of 36. `GLF_NATIVE_LOD_LAG=0` turns it off; the self-check (`GLF_NATIVE_CACHE=3`) does too.
- **A ground base's shadow from the GPU** (step 6, for bases only): its casting run gets a mesh too
  (`cached_run(..., cast_gpu)`), and `setup_shadows` hands the GPU nodes whose box comes within 2.5 km of the
  eye to the map whole (`gl_shadow_add_mesh`, drawn by `gl_shadow.c`'s own mesh shader, bent like `VS_MESH`);
  the map is fitted to those boxes cut to 2.5 km. Casting runs no longer work out face colours. Ships and
  scenery still cast from CPU triangles.
- `build_mesh` fills its buffer in place and lists each node's faces without sorting (about 4.8 to 3 ms for
  London).
- Tried and dropped: keeping each sub-model call's output and putting it in again when its own tests still
  held. It worked (the calls' temporaries had to be compared only where they weren't the object's, and the
  call kept as made: `run` changes its `Run`), but copying 100000 triangles in and out cost as much as running
  them. Found on the way: `Rec.live` kept the call's `Run` after running it; now as made.

Result, same flight: 50-53 pictures/s in the slowest seconds.
