# Pixelica API overview

A map of the public C API for game authors. The headers in `include/pixelica/` are authoritative for
exact behaviour, error codes and edge cases; this file says where things are and how they fit
together. Include `pixelica/pixelica.h` to get everything.

Everything is portable C11, single-threaded per context, with explicit ownership, `PIXELICA_Status`
results and no allocation per frame. There is no GPU API, audio, editor, ECS or glTF support.

## Headers

| Header | Contents |
|---|---|
| `status.h` | `PIXELICA_Status`: `PIXELICA_OK`, `_ERR_INVALID_ARG`, `_ERR_NO_MEMORY`, `_ERR_CAPACITY`, `_ERR_BAD_ASSET`, `_ERR_PLATFORM`, `_ERR_NOT_FOUND` (new codes are only appended) |
| `math.h` | `PIXELICA_Vec3`, column-major `float[16]` matrices (translation, scale, rotations, multiply, perspective, transform point) |
| `renderer.h` | `PIXELICA_Mesh`, `PIXELICA_Vec2`, `PIXELICA_Texture`, `PIXELICA_Material`, `PIXELICA_FrameStats`, `PIXELICA_CullMode`, `PIXELICA_RGB565`, limits |
| `pixelica.h` | `PIXELICA_Config`, context lifecycle, frame protocol, draw calls, fixed-timestep `pixelica_update` |
| `platform.h` | `PIXELICA_PlatformOps` (the platform abstraction layer) |
| `input.h` | `PIXELICA_BTN_*` logical buttons, edge helpers, bounded input recorder/replay and the "P3IN" serialization |
| `assets.h` | `.p3mesh` / `.p3tex` / `.p3cart` loaders, `PIXELICA_MeshAsset`, `PIXELICA_Cart`, `pixelica_crc32` |
| `scene.h` | `PIXELICA_AABB`, `pixelica_aabb_move`, fixed-capacity `PIXELICA_Scene` list and `pixelica_scene_render` |
| `animation.h` | `PIXELICA_Ease`, `pixelica_anim_ease`/`pixelica_anim_tween`, `PIXELICA_AnimKeyframe`/`pixelica_anim_sample`, `PIXELICA_AnimPlayer` (`pixelica_anim_player_*`), `PIXELICA_Transform`/`pixelica_transform_matrix` (see [`animation.md`](animation.md)) |
| `skeleton.h` *(optional module)* | `PIXELICA_Bone`, `PIXELICA_Skeleton`, `PIXELICA_SkinWeight`, `pixelica_skeleton_pose`, `pixelica_skeleton_skin`. Only with `PIXELICA_BUILD_SKELETON=ON`; **not** included by `pixelica.h` (see [`rigging.md`](rigging.md)) |
| `ik.h` *(optional module)* | `PIXELICA_IKTwoBone`, `PIXELICA_IKTwoBoneResult`, `pixelica_ik_two_bone`. Only with `PIXELICA_BUILD_IK=ON`; **not** included by `pixelica.h` (see [`rigging.md`](rigging.md)) |

## Conventions

Right-handed, +Y up, camera looks down -Z, radians. Matrices are column-major and act on column
vectors: `clip = P * V * M * p`. Depth is stored as `uint16` (0 = near plane, cleared to
`UINT16_MAX`, strictly-less test). Counter-clockwise on screen is front-facing and back faces are
culled by default. Color is RGB565; texture and frame row 0 is the top row.

## Lifecycle and memory

```c
PIXELICA_Config cfg = { width, height, max_draw_commands, max_vertices_per_mesh, frame_arena_bytes };
PIXELICA_Context *ctx;
pixelica_create(&cfg, &platform_ops, &ctx);              // the ONLY heap allocation, or:
size_t need; pixelica_memory_requirement(&cfg, &need);
pixelica_create_in_buffer(&cfg, &platform_ops, memory, need, &ctx);   // caller-owned memory, never allocates
...
pixelica_destroy(ctx);
```

`frame_arena_bytes` must be at least `max_vertices_per_mesh * PIXELICA_ARENA_BYTES_PER_VERTEX`; size it with
the macro, not a literal. A failed create leaves `*out_ctx == NULL` and leaks nothing. The color
and depth buffers (`pixelica_color_buffer`, `pixelica_depth_buffer`) belong to the context.

## Frame and update protocol

```c
pixelica_update(ctx, elapsed_us, update_fn, userdata);     // fixed 60 Hz steps, at most 4 per call
pixelica_begin_frame(ctx, clear_color);
pixelica_set_view_projection(ctx, view_projection);
pixelica_draw_mesh(ctx, &mesh, model);                     // flat color
pixelica_draw_mesh_ex(ctx, &mesh, model, &material);       // textured and/or per-triangle color
pixelica_end_frame(ctx);                                   // presents exactly once through the platform
```

Draws rasterize immediately and validate before writing any pixel; mesh, material and texture
memory only has to stay valid during the call. `pixelica_update` reads the platform buttons once per call
and passes that mask to each step, so the simulation depends on the tick sequence, not the render
rate. `pixelica_frame_stats` reports triangles, `fragments_covered`, `pixels_covered`, `pixels_written` and
the arena high-water mark; coverage is `pixels_covered / (width*height)` and geometric overdraw is
`fragments_covered / pixels_covered`.

`pixelica_draw_mesh_options(ctx, mesh, model, material, &options)` adds per-axis
CLAMP/REPEAT and OPAQUE/TEST/BLEND through `pixelica/draw_options.h`.
`pixelica2d_draw_sprite_ex` accepts the same options for sprites. RGB565 textures
keep their existing format; optional per-texel alpha is a separate byte plane.
BLEND depth-tests without writing depth; callers draw it back-to-front after
opaque/TEST geometry. See [texture_options.md](texture_options.md).

## Textures and materials

`PIXELICA_Texture` is `{pixels, width, height}` (RGB565, 1..4096 per side). `PIXELICA_Material` is
`{uvs, triangle_colors, texture}`; any pointer may be NULL. A texture needs one UV per mesh vertex.
Sampling is nearest-neighbor with perspective-correct interpolation, `(0,0)` = top-left texel; there is
no filtering or mipmapping. A NULL material is the same as `pixelica_draw_mesh`.

## Platform abstraction layer

A platform fills `PIXELICA_PlatformOps {userdata, time_us, read_buttons, present_rgb565, log}`.
`present_rgb565` is **synchronous**: it must have finished reading the engine's pixel pointer, or
copied it, before it returns, because the engine may overwrite the buffer immediately. A platform
with asynchronous transfers (for example an LCD DMA) therefore owns its own staging memory; see
`docs/porting.md` for the ESP32-P4 SPI/I80 contract. Return `false` to report failure
(`pixelica_end_frame` then returns `PIXELICA_ERR_PLATFORM`).

## Input

Games see only `PIXELICA_BTN_LEFT/RIGHT/UP/DOWN/A/START/QUIT`. `pixelica_buttons_pressed/released` give edges
from the previous tick's mask. `PIXELICA_InputRecorder` stores run-length spans in caller storage,
`PIXELICA_InputPlayer` / `pixelica_input_replay` feed them back through the same tick function, and
`pixelica_input_encode/decode` serialize them (little-endian, CRC-checked) so a recorded play-through can be
replayed to a state hash.

## Assets

`pixelica_mesh_asset_memory_required` / `pixelica_mesh_asset_load`, `pixelica_texture_asset_memory_required` /
`pixelica_texture_asset_load` validate the whole file (structure, CRC-32, geometry) before touching caller
storage, copy decoded values into storage of any alignment, and need no heap or file IO. `pixelica_cart_open`
validates a `.p3cart` and every nested resource; `pixelica_cart_find` / `pixelica_cart_entry` return zero-copy views
of the nested files, so the cart bytes must outlive them. A mesh asset's material has `texture == NULL`:
bind your own texture to its `material.texture`. Format details: `docs/asset_format.md`. Compile-time
limits (`PIXELICA_ASSET_MAX_*`, `PIXELICA_CART_MAX_ENTRIES`) are overridable only by rebuilding
the core with the same definitions in all translation units. Defining them in a game
that links an already-installed library does not change that library's loader limits.
The Python packer keeps its own fixed default limits.

Assets are produced offline by `tools/pixelica_pack`. Its VOX import accepts one flat model (`VOX ` 150 or 200);
scene graphs, multiple models, layers and material chunks are rejected with an error, never silently dropped.

## Scene and collision

`PIXELICA_Scene` is a caller-owned fixed-capacity array of `PIXELICA_SceneNode {mesh, material, model, bounds, id, solid}`
drawn in insertion order by `pixelica_scene_render`; `pixelica_scene_add` validates and rejects duplicate ids.
`pixelica_aabb_move` performs a deterministic X-then-Y-then-Z swept move that stops at first contact (no
tunneling) and slides along touching faces; `pixelica_scene_solid_bounds` collects obstacle boxes. There is no
hierarchy.

## Animation

`animation.h` is a small allocation-free module that needs no context: easing curves, a bounded scalar tween,
keyframe tracks sampled at an integer tick, a once/loop/ping-pong `PIXELICA_AnimPlayer` that advances in O(1), and
`PIXELICA_Transform` (translation, Euler XYZ rotation, scale, pivot) turned into a column-major matrix with
`pixelica_transform_matrix`. A child part is composed with `pixelica_mat4_multiply(world, parent_world, local)`; there is no
hierarchy type, and scene nodes and their bounds are never updated automatically. Every status function returns
`PIXELICA_ERR_INVALID_ARG` for NULL, unknown enums, non-finite floats or an invalid player and then leaves all outputs
unchanged. Details, time and endpoint rules and the interactive demo: [`animation.md`](animation.md) (Korean).
Executed checks and actual framebuffer captures: `ANIMATION_AUDIT.md` (local-only record).

### Optional: skeleton and IK

The core animation above has no skeleton or IK. Two **separate, default-off** modules (outside `pixelica_core`, not part
of `pixelica.h`, independent of each other) add them; details, ownership rules and build/install in
[`rigging.md`](rigging.md) (Korean):

- `pixelica_skeleton_pose` turns caller-supplied local affine matrices of a parent-before-child bone array into world and
  palette matrices (`palette = world * inverse_bind`, model space); `pixelica_skeleton_skin` does positions-only linear
  blend skinning (4 influences) into a caller-owned vertex array that can be used directly as `PIXELICA_Mesh.vertices`.
  Caller-owned workspaces are mandatory (`2 * bone_count` matrices, `vertex_count` vertices); on every failure the
  logical outputs stay byte-for-byte unchanged.
- `pixelica_ik_two_bone` solves a two-bone chain analytically and returns joint/end positions only (pole is a point,
  the target distance is clamped, no stretch or angle limits). It does not touch a skeleton.

## Minimal program shape

Layered mesh materials: [material_layers.md](material_layers.md) documents
`pixelica_draw_mesh_layers`, per-vertex RGBA8 tint/alpha, and up to three additional
textures with independent UVs and multiply/add/lerp composition. The legacy material
layout and entry points remain unchanged. This API belongs to the 3D renderer and the
3D portion of hybrid rendering.

Camera helpers (`Pixelica::common`): [camera.md](camera.md) covers 2D follow/bounds/coordinate conversion,
3D look-at/smoothing and deterministic shake. Debug timing and formatting are in [debug.md](debug.md);
the optional-to-call HUD uses the bundled `Pixelica::ui` library. Neither common API requires a renderer.
Chunk management (`Pixelica::common`): [chunks.md](chunks.md) covers checked coordinates,
inclusive target ranges, caller-owned slots, load/unload callbacks, budgets and retry after a failed load.

Offscreen targets, borrowed frame/texture views and caller-owned color/depth snapshots:
[framebuffer.md](framebuffer.md). Copy and sampling APIs belong to `Pixelica::common`
and also accept 2D buffers with a caller-supplied stride; context adapters belong to 3D.
[render_effects.md](render_effects.md) covers `pixelica_draw_mesh_effects`: nearest/bilinear
material sampling, a pixel callback after material evaluation, and up to four world clip planes.
These mesh effects apply to 3D and the 3D portion of hybrid rendering.

See `sdk/template/src/` (portable `game.c` plus a headless driver) and `examples/` (hello_triangle, hello_cube,
hello_animation, pixel_room; `hello_rig` with the optional modules) for complete uses; `docs/sdk.md` explains how to build one against the installed package.
Test coverage of this API (loaders, renderer, scene/AABB, input replay) and its results are in
`docs/REMAINING_AUDIT.md`.
