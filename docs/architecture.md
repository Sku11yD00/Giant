# Architecture

Five layers. Each layer only uses the layers below it.

```
game        C# scripts, levels, materials, scatter graphs, weather presets
editor      panels, play-in-editor, undo; uses everything below
ai          command layer, MCP server, rule checks
gameplay    game rules: items, inventories, weapon parts, crafting, loot (docs/game-plan.md)
sim nav audio save world  giants, destruction, NPCs, animation | walkable ground, paths | sound through the world | game save slots | cells, streaming, landscape, scatter, weather
render      virtual geometry, voxel world, GI, materials, hair, clouds
rhi geometry voxel  thin graphics layer (D3D12 first) | meshes, import, cluster LOD (CPU only) | voxel bricks, clipmap
platform    windows, input, files (SDL3 underneath)
core        jobs, memory, double-precision math, logging, reflection, text formats
```

## Allowed module dependencies

This table is the source of truth for `tools/check_architecture.py`.

| Module | May include |
| --- | --- |
| core | (nothing) |
| platform | core |
| rhi | core |
| texture | core, rhi |
| anim | core |
| geometry | core, anim |
| voxel | core |
| render | core, rhi, geometry, voxel |
| world | core, voxel |
| sim | core, world |
| nav | core, world |
| audio | core |
| save | core |
| ui | core, rhi |
| gameplay | core, world, sim |
| script | core, world, sim |
| ai | core, platform, rhi, anim, geometry, render, world, sim, script |
| editor | everything |

`rhi` never includes `platform`: a swap chain takes the native window handle
as plain data, so the graphics layer works headless and in tests.

`render` reads world state through plain data handed to it each frame (cells,
instances, weather values, voxel edits); it never includes `world` or `sim`.
This keeps the renderer replaceable.

## Graphics layer

`rhi` (engine/rhi/include/giant/rhi/rhi.h) is a thin, bindless layer. The
rest of the engine reaches the GPU only through it, so a Vulkan backend can
follow without touching the renderer.

- **Handles.** Buffers, textures and pipelines are `Handle<Tag>{index,
  generation}` into device pools. A destroyed slot bumps its generation, so
  a stale handle stops resolving instead of reaching whatever is created
  there next. Zero is "none".
- **Bindless.** Shader model 6.6: every shader-visible resource has a
  descriptor index in one heap (`ResourceDescriptorHeap` in shaders), and
  draws pass indices in up to 128 bytes of push constants (register b0).
  One global root signature, so pipelines need no binding setup. Four fixed
  samplers. Shaders include `giant/bindless.slang` (engine/rhi/shaders) and
  never touch the heaps directly.
- **Vertex pulling.** No vertex buffers or input layouts: vertex shaders read
  what they need from buffers by `SV_VertexID`. Index buffers stay (32-bit).
- **Frames.** `begin_frame(swap_chain or null)` returns the frame's command
  list; `end_frame` submits (and presents). Two frames in flight live on the
  device. Destroying anything is deferred until the GPU is done with it, so it
  is safe mid-frame. Per-frame transient memory (`allocate`, `upload`) holds
  constants, instance lists, streamed geometry and UI geometry; each slot
  makes 32 MiB of it at init (the default streaming budget), so a streaming
  burst does not create upload buffers mid-frame.
- **Uploads (N2 part 1).** A buffer or texture created with initial data
  inside a frame never waits for the GPU: the data goes into the frame
  slot's staging memory (persistent, mapped upload-heap chunks of 8 MiB,
  four per slot made at init, bump-allocated and reused once that slot's
  fence is reached; a burst past them adds chunks, kept until unused for
  120 frames; buffer data is split across chunks, so only a texture larger
  than a chunk needs one of its own) and is copied by the frame's upload list,
  submitted on its own just before the frame's list on the same queue.
  Outside a frame, before any frame has run (load time), the copy is
  waited for. Between frames once frames run (what an update before
  `begin_frame` makes: a character's skinned copy, a cloth's mesh, an
  imported mesh), a buffer's data is staged in chunks of 1 MiB the
  copies share (people coming near make a hundred small buffers in one
  update: one allocation, not one each; larger data has a chunk of its
  own) and copied by the next frame's upload list, first, so the main
  thread does not wait for the GPU while it is busy with the frame before
  (D3D12 `m_deferred`; this wait was the living gate's slow frames, 9 ms
  whenever people came near, as read from the code: the gate on a real
  GPU is the proof). The chunks are held until that frame, and none may
  begin for a long while (a minimised window, a broken swap chain), so
  at most 256 copies and 64 MiB are held: past that (a world opened or
  many meshes imported by a command) a copy is waited for as at load
  time. An immediate submit before that frame (a read-back, a waited
  write) submits the held copies first, on their own, so what is done to
  a buffer keeps its order and the buffers are back in their common state
  for what follows; a buffer destroyed before its copy is not copied
  into. The chunks count in `DeviceCounts` (created, allocations,
  released), not in `upload_bytes`. A texture made with its contents
  between frames is still waited for (only the textures `wait` command
  and load time do that; streamed textures upload inside the frame).
  `FrameStats::upload_bytes` counts what a frame staged; the caller keeps
  it within its budget.
- **Texture formats, mips and layers (photoreal part 3a-1).** Besides the
  plain colour, float and depth formats, textures can be block compressed
  (`bc1_unorm`/`bc1_srgb` RGB, `bc4_unorm` one channel, `bc5_unorm` two
  channels for normal maps, `bc7_unorm`/`bc7_srgb` RGBA; 4x4 blocks of 8 or
  16 bytes), sampled only, sized in multiples of 4 and made with their
  contents (cooked offline). `TextureDesc::mips` (up to the full chain) and
  `layers` (up to 2048) are for sampled textures. `TextureDesc::array`
  makes a texture read as a `Texture2DArray` (`read_texture_array` in
  bindless.slang) whatever its layer count, so a one-layer array keeps its
  shader type; more than one layer needs it. A texture is at most 4 GiB.
  Contents run layer after layer, each mip after mip from the largest, rows
  of texels (or of blocks) packed tight (`texture_bytes`, `level_bytes`,
  the D3D12/DDS subresource order); every subresource is copied in one go.
  `texture_desc_problem` and `texture_contents_problem` cover every rule,
  so a corrupt cooked file is refused, never asserted: both devices log the
  reason and return an invalid handle. Compressed textures are made whole
  for now; mip streaming (part 3b) adds partial uploads and a min-mip
  clamp at the same descriptor. `read_texture` reads the first layer's
  largest level of uncompressed textures only (empty for compressed ones).
- **Small buffers share heaps.** A buffer that is its own allocation (a
  committed resource) is new video memory: Windows finds, zeroes and maps
  it, and unmaps it when it goes, on the GPU's timeline, and the queue
  waits behind that work. Streaming makes and drops thousands of small
  buffers (terrain tiles, meshes), so on D3D12 a gpu-memory buffer up to
  4 MiB made with all its contents is placed in one of a few 64 MiB heaps
  instead (four made at init, 256 MiB, more when they fill, none given
  back), a range of 64 KiB units each (src/heap_ranges.h; a tiny buffer
  takes a whole unit, as a committed one does). The heaps are not zeroed
  and their ranges are reused, so a buffer made without its contents stays
  committed (it comes zeroed). Larger buffers and textures are committed
  too; if a heap cannot be made, small buffers are committed from then on.
  `Device::counts()` keeps running totals of resources created, new memory
  allocations and resources released, and `FrameStats::wait_ms` is
  begin_frame's wait for the GPU; the editor keeps both per frame and the
  gates print them for the two frames before each hitch (begin_frame
  waits for the frame two back).
- **GPU timing (N2 part 1).** `CommandList::begin_timing(name)` /
  `end_timing()` (nestable, up to 32 per frame) put timestamps around GPU
  work; `Device::gpu_timings()` and `gpu_frame_ms()` give the newest
  finished frame's times, read once its fence is reached, never waited for.
  The renderer times streaming, culling, raster, HZB, shading and
  anti-aliasing; the editor adds its UI.
- **States.** Each resource's state is tracked; `barrier()` moves it to what
  the next use needs, and render passes move their own targets. Buffers fall
  back to `common` after each frame, as D3D12 decays them.
- **Conventions.** Reverse-Z depth (1 near, 0 at infinity, GREATER passes),
  counter-clockwise front faces, back faces culled. Colour targets are sRGB
  (`rgba8_srgb`); UI draws through a UNORM view so its gamma-space colours
  show as picked.
- **Backends.** D3D12 on the DirectX 12 Agility SDK (shipped in a `D3D12`
  folder next to the exe), and a null backend that validates every call the
  way the debug layer would (plus stale handles and pass/pipeline mismatches)
  and emulates memory, clears and copies, so tests read results back without
  a GPU. Windows CI runs every GPU test on WARP with the validation layer and
  fails on any validation error.
- **Shaders** are Slang, compiled at build time by `giant_shaders()`
  (cmake/GiantShaders.cmake) to DXIL and embedded in the module; one Linux
  CI job compiles them too, to catch errors there.

The renderer (`render`) sits on top: plain per-frame data in (camera,
lighting, instances), an image out. Since N1 part 3 it draws clusters
chosen on the GPU into a visibility buffer, then shades it (see Geometry).

## Geometry

`geometry` holds meshes as plain data and everything done to them on the
CPU: glTF/GLB import (cgltf), and cluster LOD (meshoptimizer and its
`clusterlod.h`). It knows nothing about the GPU, so tools, tests and later
an offline cooker can use it without a device.

- **Clusters.** A mesh becomes clusters of up to 128 triangles and 128
  vertices. Groups of about 16 clusters are simplified to half their
  triangles with their outer border locked, then cut again; level by level
  until one group is left. Each group stores the error (metres) and bounds
  of its simplified replacement; errors only grow towards coarse levels.
- **The cut.** A cluster is drawn when its group's replacement would show
  more than `lod_error_pixels` of error and its own detail would not (or it
  is original). A spatial tree per level skips whole regions that are
  precise enough. Locked borders mean neighbours from different levels meet
  without cracks, so detail changes cluster by cluster, with no popping of
  whole objects.
- **GPU culling (N1 part 3, `shaders/cull.slang`).** The cut runs on the
  GPU, per instance: instances are tested against the view and the depth of
  the last frame, then the LOD trees are walked one level per dispatch
  through queues of (instance, node) items, with dispatch sizes written on
  the GPU (indirect arguments). Chosen clusters are tested against the view
  and the depth too, and appended to the visible list.
- **Two-pass occlusion.** Pass 0 tests against a depth pyramid (HZB, the
  farthest depth per texel, `shaders/hzb.slang`) built from the last frame,
  reprojected with the camera's movement. What it hides is kept aside and
  tested again in pass 1 against the HZB of what pass 0 just drew, so
  nothing visible is lost when the camera or objects move. The HZB of the
  finished frame is kept for the next.
- **Visibility buffer.** One raster pipeline draws every visible cluster:
  mesh shaders (one group per cluster, 128 threads) where the GPU has them,
  else a vertex shader that pulls 384 vertices per cluster instance. The
  pixel shader writes only (visible-list index + 1, triangle) into an
  rg32_uint target. A full-screen pass (`shaders/resolve.slang`) then
  rebuilds each pixel's triangle, works out barycentrics against the camera
  ray, and shades: one material evaluation per pixel, however dense the
  geometry.
- **Counters.** The GPU counts clusters, triangles and occluded work; the
  counts are copied to a readback ring and reach `RenderStats` a frame or
  two late (`from_gpu`). Full lists set an overflow flag (logged once).
  Slots and layouts shared by C++ and shaders live in `src/gpu_layout.h`.
- **Vertices (photoreal part 3a-2).** A mesh vertex
  (`geometry::Vertex`) holds its position, normal, uv (TEXCOORD_0: (0, 0)
  the image's top-left, v down) and a tangent frame as glTF defines it: the
  tangent along +u and a bitangent sign, the bitangent `sign *
  cross(normal, tangent)` pointing up the image. `generate_tangents` makes
  frames from the uvs the MikkTSpace way (per-corner directions weighted by
  angle, summed per vertex, made perpendicular to the normal), splitting a
  vertex whose triangles disagree on the sign (a mirrored uv seam); the
  glTF importer keeps a file's own TANGENT and makes the rest, and the
  built-in shapes have uvs and tangents. The GPU reads 28 bytes a vertex
  (`geometry::PackedVertex`): position floats, normal and tangent as
  octahedral snorm16 pairs (the sign in the tangent's lowest y bit), uv
  floats; `cluster_normal`, `cluster_tangent` and `cluster_uv` in
  common.slang read them. Simplification weighs normals and, lightly, uvs,
  and only drops vertices, so every level keeps the originals' uvs and
  tangents. Skinning poses the tangent with the normal and keeps it
  perpendicular; the uv stays put.
- **Cooked textures (photoreal part 3a-3a, `engine/texture`).** Material
  textures are cooked offline: `decode_image` reads PNG, JPEG, TGA or BMP
  (stb_image), `make_mips` filters a full chain the way each kind's data
  means, and `cook` block compresses it (rgbcx, bc7enc), spread over the
  job system when given one. Colour is filtered in linear light and
  weighted by alpha, so cut-out edges keep their colour; BC7 sRGB, or BC1
  sRGB (`small`) at half the size without alpha. Normal maps (green up;
  `flip_green` for DirectX ones) average their vectors and renormalise;
  BC5 keeps x and y. Roughness and metallic (from a glTF metallic-roughness
  image) go to BC5, kept apart because they rarely move together (BC7 would
  smear hard metal edges into the roughness). Roughness is filtered as GGX
  alpha squared and, given the material's normal map, widened by the normal
  spread each level averages away (Toksvig), so bumpy surfaces turn rough
  with distance instead of sparkling. Masks (any one channel, such as
  glTF occlusion) are BC4. The top level is resized to sides that are
  multiples of 4 within `max_size`. A `.gtex` file is a 64-byte header
  (kind, format, size, mips, layers, contents length, and a hash of header
  and contents) and the contents in rhi's layout, so `read_file` hands `create_texture` the
  bytes as they are and `level_offset` finds any level for streaming.
  Smaller mips need not be multiples of 4, so a texture streamed from one
  of its levels (part 3b) starts only at a level whose sides are whole
  blocks (`render::coarsest_first_mip`).
- **Material textures (photoreal part 3a-3b).** `Renderer::add_texture`
  takes a cooked texture (any sampled single-layer format, mips and all)
  and returns a `TextureId`: a slot and its generation, so an id kept after
  `remove_texture` stops resolving (frames in flight finish with the
  texture). A `Material` names up to four, combined as glTF combines its
  maps (in the cooker's channels): base colour (sRGB, multiplies
  `base_color`; alpha unused yet), normal (x, y in R, G; green up; z
  rebuilt), roughness and metallic (R, G; multiply theirs) and occlusion
  (R; dims the ambient light and everything reflected, never the sun). An
  unknown or removed id counts as none, and so does one in the wrong colour
  space (base colour sRGB, the rest linear; logged once). gpu::Material is
  48 bytes: the plain values and four read descriptors. Shading samples them
  anisotropically (wrapping) with `SampleGrad`, the footprint from ray
  differentials: the rays of the pixels right of and below this one, met
  with the triangle's plane, give the uv's change per pixel (at most the
  whole texture, for grazing views), so mips are right on the visibility
  buffer, which has no screen derivatives. Pixels of untextured materials
  skip all of it. The normal map bends the shading normal in the
  interpolated tangent frame (a mirroring instance flips the bitangent);
  specular anti-aliasing measures the bent normal, while shadow lookups,
  bounce light and reflection cones start from the surface's own normal so
  bumps do not shift them. Far-field voxels keep the plain colours.
- **Texture streaming (photoreal part 3b-1).** A renderer texture keeps
  its whole description and holds its levels from a first mip down, as a
  texture of its own (`texture_from_mip`), so its descriptor and sampling
  are unchanged: uvs cover the same image at less detail.
  `add_texture(desc, contents, first_mip)` makes one from a level;
  `stream_texture(id, first_mip, levels)` brings finer ones and
  `trim_texture` drops them. `render()` makes the changes, oldest first,
  before the material table takes descriptors: a finer change makes a
  texture from the target level, copies the levels it has into it on the
  GPU (`rhi::Device::copy_texture_levels`) and uploads the new levels rows
  of blocks at a time (`write_texture`) under
  `RendererDesc::texture_stream_bytes_per_frame` (8 MiB, at least a row a
  frame), swapping it in when whole; a coarser one copies the levels kept
  into a smaller texture and swaps at once. At most
  `texture_changes_per_frame` (8) textures are made a frame. Both rhi
  calls are recorded with the frame's uploads, so nothing waits, and
  block-compressed textures may now be made empty. Which detail is needed
  comes from the GPU: shading takes one pixel in 16 a frame (a 4x4
  pattern stepping each frame) and, for its material, keeps the largest
  ceil(-log2 footprint) + 1 (the uv step over the pixel, anisotropic
  filtering allowed 16 texels along the long axis) in a u32 per material
  (`InterlockedMax` after a plain read), for the first 65536 materials
  (past them a textured material counts as needing the finest level). It
  is read back two frames later; each texture keeps the finest level its
  materials needed in this and the last window of 32 frames
  (`texture_residency`: `wanted_mip`, or not seen). The streaming policy
  (what to bring, what to drop, under what memory) is the caller's: the
  editor's is next.
- **Texture streaming policy (photoreal part 3b-2).** The editor's
  `TextureLibrary` puts a cooked texture up from its tail, the finest
  level with sides up to `tail_size` (256), so a scene of new 4096 maps
  costs a few hundred KiB each at first. Every `update()` it reads each
  ready texture's residency and keeps it at least at its tail and at most
  as sharp as its materials wanted lately. Finer levels are read on a
  worker (at most `max_level_reads`, 4, at once, the furthest behind
  first) from the texture's `.gtex` at the level's offset, or from the
  cooked levels kept in memory when there is no cache, and handed to
  `stream_texture`; a read whose file changed or went makes it cook again
  (it draws what it has meanwhile), and no read starts while a newer
  version is on its way (its cook may replace the file). Levels sharper
  than wanted are trimmed after `trim_after_updates` (120, about 2 s, so a
  glance away does not reload them), or at once, most bytes first, while
  all textures hold more than `gpu_budget_bytes` (1 GiB); still over, the
  biggest lose their finest level even if wanted. The budget counts each
  texture at the level it is heading for (reads and renderer uploads under
  way included, trims under way at their target), and a finer read is cut
  back to what fits, so levels on their way are neither trimmed again nor
  trimmed twice. A new version of a drawn
  texture (a reload) goes up as sharp as the one it replaces. `finish()`
  (`get_textures` with `wait`) uploads new cooks whole and puts every
  streamed texture up whole at once (read on the main thread; tests and
  screenshots only), kept so at least `trim_after_updates` (unless over
  the budget). The upload budget charges what goes up: tails, or a
  reload's sharper levels.
  Feedback comes from the `lit` and `lighting` views only, so in other
  debug views textures fall back to their tails after a while, and a
  screenshot straight after a camera move shows what had streamed so far
  (`get_textures` `wait` first for full detail). Textures whose sides stop
  halving into multiples of 4 (e.g. 4092: 2046 is not) cannot start at a
  finer level, so their tails are larger (up to the whole texture);
  power-of-two maps stream fully. `get_textures` reports `first_mip`,
  `tail_mip`, `wanted_mip`, `reading`, `bytes` (on the GPU now) and
  `full_bytes`. Tests set `wanted_override`, as the null device gives no
  feedback.
- **Split-sum BRDF (photoreal part 4a).** Ambient specular uses the
  split-sum table (`src/brdf.h`): for n.v and roughness, (a, b) with
  f0 * a + b the light a GGX surface (resolve's height-correlated Smith
  and Schlick Fresnel) keeps from a white surround in one bounce, 32x32
  points made on the CPU by sampling GGX's visible normals (Heitz 2018;
  512 samples a point, within 0.002 of converged; some 50 ms, once a
  process) and read bilinear from a buffer (`ResolveExtras::dfg`; columns
  linear in n.v, so at silhouettes below n.v 0.03 it is off by up to
  0.07). The light lost to bounces between
  microfacets is put back (Fdez-Aguera 2019) as light from all round, and
  the sun's highlight is scaled by 1 + f0 (1 / Ess - 1) (Filament), so a
  rough white metal is as bright as a smooth one (a WARP furnace test).
  Diffuse ambient takes what the specular leaves. Reflections (voxel
  cones and the screen's) are weighted by the single-bounce part times a
  specular occlusion from the material's occlusion (Lagarde 2014, fed
  alpha, roughness squared), in
  place of the old hand-tuned Fresnel and roughness dimming.
- **Ambient occlusion (photoreal part 4b).** `ao.slang`, between the
  raster passes and shading in lit views (`RenderSettings::
  ambient_occlusion`, default on; `ambient_occlusion_radius` metres,
  default 1): GTAO (Jimenez 2016) at half size from this frame's depth.
  Each half-size pixel takes the full-size pixel 2i, its view point and a
  normal from depth (the flatter neighbour on each axis), and integrates
  the cosine-weighted visibility between the horizons of two slices, six
  steps a side out to the radius as seen on screen (at most 96 pixels,
  fading towards it; samples between pixels take depth interpolated, as
  it is linear across a plane, so open ground does not occlude itself),
  plus the bent normal. The slices turn by a fixed
  4x4 pattern; `cs_denoise` averages each 4x4 block round a pixel (all 16
  angles) weighted by depth (allowing for the slope seen there, so
  grazing ground still averages), then clamps, so there is no history and
  nothing smears.
  Output rgba16_float: visibility, view depth, bent normal (octahedral,
  view space). Resolve takes the four half-size texels round each pixel,
  bilinear and weighted by depth (again allowing for the surface's slope),
  multiplies the material's occlusion by
  the visibility (so it dims ambient diffuse, GI's bounce and, through the
  specular occlusion, reflections) and leans the normal the ambient
  light is looked up along towards the bent normal as much as the place
  is occluded (none on open ground). Timed as
  "ambient occlusion"; `RenderStats::ambient_occlusion`; MCP
  `set_render_settings`. The sun is left alone (its shadows are the
  shadow pages' and the contact shadows').
- **Contact shadows (photoreal part 4c).** `ao.slang`'s `cs_contact`,
  a compute pass of its own at full size after ambient occlusion, lit
  views (`RenderSettings::contact_shadows`, default on;
  `contact_shadow_length` metres, default 0.3, 0.05 to 2): for each
  pixel whose surface (its normal from depth) faces the sun, up to 12
  steps (one each 4 pixels the ray crosses, at least 2) march from the
  point, lifted off the surface by 1.5 pixels' size, towards the sun in
  view space, never further than 48 pixels' size or behind the camera's
  near side, starting at a 4x4 pattern's jitter. A step is in shadow
  where the depth there is nearer than the ray by more than a bias
  (growing with distance) and by less than a thickness (0.1 m plus 1% of
  the distance, so things far in front do not shade what is behind
  them). The shadow fades over the march's last quarter and by the
  screen's edges; off screen and sky samples do not shade. The result
  (r32_float, the sun let through) is read by shading where the sun
  still lights the pixel after its shadow pages and cloud shadow. Kept
  out of the shading pass: a march inside that large shader more than
  doubled the software renderer's frame time (the Windows CI gates). No history; timed as
  "contact shadows"; `RenderStats::contact_shadows`; MCP
  `set_render_settings`.
- **Terrain layers (photoreal part 5a).** A material with
  `Material::terrain` is shaded from `FrameInput::terrain_layers` (up to
  8; `gpu::Material` grew to 64 bytes for its flags). A full-size compute
  pass (`terrain.slang`, timed as "terrain", only in lit views with layers
  given) rebuilds the triangle from the visibility buffer where a material
  is terrain and writes the colour, roughness and shading normal to two
  rgba16_float targets; resolve only loads them (the same code inside
  resolve's pixel shader doubled WARP frame time for every pixel, terrain
  or not). The pass stacks the layers like paint: the first covers everywhere, each later one where its slope
  band (degrees from level, of the surface normal) and height band
  (metres above y = 0) hold, soft over `slope_blend` and `height_blend`
  at their edges. Each layer's colour, roughness and textures (base
  colour, normal map, roughness) are mapped by world position every
  `tiling` metres: from above, and from the sides where the ground is
  steep (triplanar, weights |n|^4 with small ones dropped, so level
  ground takes one sample a texture). The CPU passes each layer's
  frac(camera / tiling) so the uvs stay exact far from the origin, and the
  footprint for filtering comes from the neighbouring pixels' rays met
  with the surface's plane. Normal maps bend the normal along each
  plane's axes (summed, as UDN). A value noise over 32 m and 128 m cells
  (period 4096 m, from the camera's x and z modulo it) varies the
  brightness by about 18% against the textures' repeats. Fixed loops,
  unrolled. Layer textures have no streaming feedback yet: they are
  always wanted at their finest. Shadows, AO and probes keep the surface
  normal. Painted splats and height-blended edges come in 5c.
- **Terrain layers in worlds (photoreal part 5b).** `world::TerrainMaterial::layers`
  (`world::TerrainLayer`: colour, roughness, `MaterialMaps` without
  occlusion, tiling, slope and height bands) save with scenes and in
  world.json's terrain settings. The editor's `scene_frame` makes the
  ground's material a terrain one when there are layers (terrain tiles and
  far ground chunks alike, so the layers reach the horizon) and hands the
  layers to the renderer each frame, their images cooked and streamed by
  the TextureLibrary like any material's (kept in use beside the drawn
  materials'). The far voxel world past the meshes keeps the terrain
  material's own `base_color`. MCP `set_terrain_layers` (undoable, replaces
  them all), `get_terrain` lists them; the scene panel edits them.
- **Painted terrain layers (photoreal part 5c-1).** `world::Terrain` keeps
  a tile's paint apart from its heights: `k_paint_channels` (8) bytes a
  height sample, the weights of layers 1 to 7 (the last byte spare), with
  its own versions and change list (`take_paint_changes`), so painting
  rebuilds no meshes, physics or scatter. Saved as hex rows under the
  tile's `"paint"` in scenes and in cell files beside `"heights"` (paint
  with no tile is refused); all zero is no paint. `world::apply_paint` is
  the brush (towards 255 or 0 by strength times the brush falloff, rounded
  away from where it is so light dabs arrive). Streaming treats paint like
  heights: a cell whose paint version is not its file's is unsaved and
  stays loaded. MCP `paint_terrain` (undoable, drag merging as
  `sculpt_terrain`, sharing its stroke code), `get_terrain` reports
  weights; the viewport brush has paint and erase ops with a layer.
- **Painted layers drawn (photoreal part 5c-2).** `Renderer::set_terrain_paint`
  (grid by `set_terrain_paint_grid`) keeps each tile's weights in one pool
  buffer, a block a tile (`TerrainPaintStore`: grows by copying on the GPU,
  uploads whole tiles under `terrain_paint_bytes_per_frame`, at least one a
  frame; a tile waiting draws its old weights). Each frame a small table
  (open addressing, at most half full) maps tiles to blocks; terrain.slang
  reads the four samples around a pixel (across tile edges) and a layer's
  coverage is the larger of its bands and its paint. A layer with a colour
  image has its edge sharpened by the image's brightness against its
  average (its coarsest level) as a height, so stones show through sand
  before the sand ends. The editor's `TerrainPaintSync` hands over only
  tiles whose paint changed (all for a new terrain or spacing).
- **Two-sided leaves (photoreal part 6a).** `Material::two_sided` draws a
  surface from both sides: when a frame has any, the visibility raster runs
  twice, the second time with a pipeline that culls nothing, and each pass
  keeps only the clusters whose material matches it (read from the frame's
  material table; shadows draw everything). Resolve turns a back face's
  normals towards the eye. `Material::translucency` (0 to 1) lets the sun
  through a thin surface: lit from behind, its base colour shines by the
  sun's light where the back side is unshadowed (clouds included).
- **Cut-outs (photoreal part 6b).** `Material::alpha_cutoff` (with a base
  colour texture) cuts a surface away where the texture's alpha is under
  it. The visibility raster has a pipeline per material kind (one- or
  two-sided, solid or cut out), each a draw of the frame's clusters that
  skips other kinds; only the cut-out ones (ms_cut/vs_cut/ps_cut) carry uvs
  and discard, so solid surfaces keep early depth. Sun shadows draw cut-outs
  in a second draw with the same alpha test (a cut-out material coming,
  going or changing redraws every shadow page). The texture cooker scales each
  smaller colour level's alpha so as much of it stays over 0.5 as of the top
  level's, so leaves do not thin away with distance (.gtex version 2).
  The editor sends a material as two-sided or cut out only while something
  uses it. Known gap: each kind's draw still walks every visible cluster
  (culling could split them by kind).
- **Far vegetation matching near (photoreal part 6c-1).** The voxel world
  cannot sample textures, so a material carries what it needs from its
  base colour texture: `Material::base_color_average` (the texture's mean
  colour in linear light, of every texel for an opaque material, of the
  texels its cutoff leaves for a cut-out; the voxel world multiplies it by
  base_color) and `Material::coverage` (the share of the texture its
  alpha_cutoff leaves). Both count only while the material has a base
  colour texture. The editor measures the texture on a worker when it
  cooks or loads (`texture::summarize_color`, on the finest level within
  512 texels: mean colour and coverage at 17 alpha steps; exact at any
  cutoff up to 512, exact at 0.5 and close near it above, where the
  measured mip's alpha was scaled to keep 0.5's coverage) and, once the
  texture is drawn, reads both at the material's cutoff (until then:
  white, solid). `voxel::BrickBuilder` thins cut-outs: each cut-out
  triangle adds its coverage times its area inside the brick (clipped to
  it, shared among the voxels it touches there, at most a diagonal slice
  of one) over the voxel's face, and a voxel only cut-outs touch fills
  when that reaches a threshold dithered per voxel and seeded by the
  brick's key (a rebuild comes out the same). A sparse card leaves a
  sparse field, a dense clump a solid one; a thin stand-in over solid
  ground falls back to the ground's own voxel; a brick thinned to nothing
  takes no pool room. Colour and facing count by coverage. The voxel world
  compares material rows itself each update (a row's values change under
  unchanged cells as textures arrive) and rebuilds what uses a changed one;
  one used by more than an eighth of the instances rebuilds everything,
  as any bulk change does.
- **Canopy normals (photoreal part 6c-2).** `Material::canopy` (0 to 1,
  saved with world materials, MCP `canopy` on entities, kit pieces and
  scatter items) bends the shading normal towards the direction out from
  the instance's bounding sphere's centre (resolve.slang, after the
  two-sided flip and the normal map), so a crown of leaf cards lights as
  one rounded mass: bright towards the sun, falling off around it, and
  glowing through (translucency) on the far side; the bent normal is
  pulled back where it would face away from the viewer (no grazing
  sparkle on a crown's edge). Shadows, GI lookups and contact shadows keep
  the surface's own normal; the middle is the crown's at rest (a swaying
  crown's light shifts a little). The voxel world bends each triangle's
  facing the same way (its side facing out, bent towards the direction
  through its centroid: `BrickBuilder`'s `facing`; exact for leaf-sized
  triangles, coarse for big ones), so far crowns and the bounce light off
  them match; this also stops a two-sided card's opposite faces
  cancelling to +Y. It works per
  instance, so a tree's leaves want their own mesh (entity, kit piece or
  scatter item) apart from the trunk.
- **Day and night (lights part L1).** `world::Clock` (`clock.h`; saved in
  scenes and world.json only when set) holds the time of day, the day of
  the year, latitude, a north offset and a speed. When on, `sky_bodies`
  places the sun (a circular orbit, equinox on day 80, local solar time:
  good to about a degree), the moon (on the ecliptic at its phase's angle
  from the sun, new on day 1, twelve times a 365-day year so its phase
  carries over the year's end) and the sky's own
  (equatorial) frame; `clock_lighting` puts Lighting's light where the
  sun is, or, once the sun is 6 degrees under the horizon, where the moon
  is (k_moon_light of the sun's intensity, by the share of its disc lit,
  fading in to 12 degrees under so the handover shows no jump: the sun's
  light fades out from the horizon to 2 under, `world::sun_share`), so
  shadows follow whichever leads.
  `ambient_scale` (the day sky's light from the sun's and moon's heights
  plus k_star_light) scales the sky and ground light (`weather_light`'s
  and `scene_lighting`'s `daylight`), and the sky ambient pass takes the
  flat night colours while the moon leads. The renderer's `Sky` gets
  `stars` (their brightness, out from 4 to 12 degrees under), `celestial`
  (world to the sky's frame) and `moon` / `moon_sun`: `sky_background`
  draws a fixed hashed star field in the sky's frame (about 7000 above the
  horizon, each kept in one grid cell and a pixel or so wide by
  `pixel_angle`, at most a quarter cell, no twinkle; brighter under a moon
  up, an estimate to check by eye), a faint Milky Way band and the moon's
  disc at its true size, shaded by its phase without limb darkening
  (Lommel-Seeliger) and as bright at any phase (`moon_lit`: its light
  already carries the share lit), with a faint earthshine; clouds draw
  over them. The editor's `ClockState`
  (`clock_state.h`) is view state: it shows the scene's clock and play
  moves the time on (game seconds times speed); set_time (even to the
  scene's own values) and close_game show the scene's clock at once, game
  saves keep the time reached. Stars only draw
  with a weather's sky (the flat sky stays flat, only darker). Not yet:
  the twilight glow once the moon leads, the moon's own orbit tilt, the
  planets. (Each 0.25 degree step of the sun redraws its shadow pages,
  the old ones standing in meanwhile: "Sun shadows", part L1b.)
- **Textures from files (photoreal part 3a-3c).** `world::Material::maps`
  names image files (base colour, normal, glTF metallic-roughness, glTF
  occlusion; `normal_green_down` for DirectX normal maps), saved under
  `"material": {"maps": ...}` on entities, kit pieces and scatter items,
  only when set. The editor's `TextureLibrary` cooks each (file, slot) the
  first time a drawn material asks for it, at most two at once (one a
  worker, each encoded on its own worker, so no cook starts inside another
  and only two hold their images), at most 4096 texels a side; roughness is
  cooked with the material's normal map (Toksvig). Results are kept in
  `giant_cache/textures/<key>-<files>.gtex` under the working folder: the
  first hash names the key, the second the files' sizes and change times,
  so an edited file cooks again and its older file is deleted. Finished
  textures upload once a frame under a 16 MiB budget (at least one; since
  part 3b-2 only their tails).
  `SceneFrame` dedupes materials on their values and texture refs, asks for
  textures on built-in shapes and on meshes whose file has uvs (once per
  material a call), fills in renderer ids as they become ready without
  rebuilding cells, and tells the library which textures drawn materials
  use; one unused for 600 updates (about 10 s) leaves the GPU and comes
  back from the cache when used again. Until ready a material draws its
  plain values. Closing drops cooks not yet begun. MCP: `maps` on entities
  and scatter items, `get_textures`, `reload_textures`.
- **glTF materials (photoreal part 3a-3c-2).** The importer reads the
  materials the shown meshes use (`ImportResult::materials`, most triangles
  first): metallic-roughness factors (emissive times
  `KHR_materials_emissive_strength`) and the images of the four maps, each
  a file beside the glTF (a relative URI, decoded, refused if it could
  leave the folder: `..`, absolute, backslash, colon, scheme) or bytes
  inside it (a buffer view or a base64 data URI). Specular-glossiness
  materials read as non-metals (roughness 1 - glossiness, diffuse image).
  Triangles drawn with no material count for the default one. A mesh is
  one material in the renderer (its raster pipeline, shadows, cut-outs
  and wind go by the instance's material), so a file that uses two to
  sixteen materials is drawn in **parts**: the importer keeps each
  triangle's material (`ImportResult::triangle_materials`),
  `geometry::part_of` takes one material's triangles out as a mesh of
  its own (a skinned one with its weights and every morph target by
  name), and `MeshLibrary::Mesh::parts` holds one cluster mesh per
  material, most triangles first (glTF's default material's triangles are
  a part named "" with the default look; a name already had by a part
  before it gets a number, "steel (2)", so every part's name is its own),
  each with that material's look and maps. An entity or a placed kit's piece draws one instance per
  part: the first with the entity's own look (which it took from that
  part when given the file), the rest with the file's base colour,
  roughness, metal, glow and maps under the entity's wind and surface
  settings (sway, flutter, two sides, translucency, cut-out, canopy:
  what files do not say, so the whole thing sways as one). An entity
  may give any part but the first a look of its own
  (`world::Entity::parts`, a `PartLook` by the part's name: saved with
  the scene, set by the `parts` argument of `create_entity` and
  `set_entity`): that part then draws with it whole (its colour, maps,
  skin or eye, sides and cut-out), still bending as the entity does, so
  one character file is skin, cloth, eyes and hair cards at once. Names
  the file lacks do nothing (kept, for when the file has them again). A
  kit's pieces take no such looks yet, and a character's hair still
  takes its scalp from the whole file (every part the head joint holds):
  give `hair.top` a share that clears the eyes. A scene saved
  before parts keeps the look its entities stored then (the busiest
  material, perhaps without maps) on the first part until its maps are
  set, or it is given another mesh and then this one again (the same
  file given again keeps the look it has). An animated one
  has a posed copy per part (`AnimationState::Animated::part_copies`,
  shared between characters part by part), all given the one pose and
  drawn once all are ready; everything else about a character (its
  skeleton, capsules, hair's scalp, ragdoll) still reads the whole mesh,
  which is kept beside the parts (so such a file is clustered twice at
  import, and a character in three parts is three skinned meshes of the
  renderer's count, beside the file's three sources). Parts are simplified apart, but where two meet
  (`geometry::part_seams`: corners at a place another part uses too)
  both keep every corner at every level of detail, locked as terrain
  tiles' edges are, so they never crack apart; a long seam therefore
  holds its triangles far away, and a file whose materials are woven
  through each other (a checkerboard) hardly thins at all: the import
  warns of any part of 4096 triangles or more with a quarter of its
  corners on seams. A character in parts uploads a copy of every part
  in the frame it starts animating (all or none), which then counts
  against the frame's budget of copies for the next. Scatter items and the pieces of scattered kits
  draw the whole mesh with one look (a scattered piece that took the
  first part's maps from the file draws them only where the whole look
  keeps them), as does a file with more than
  sixteen materials: for those `MeshLibrary` keeps the one with the
  most triangles as the file's look (none if the default has more), with
  its maps only when it covers at least three quarters of the triangles. Images inside the file are named
  `"<mesh path>#image<N>"` and never copied out: texture workers read
  them through `geometry::read_gltf_image`, and the cache keys them by the
  glTF file's size and time (and a .gltf's buffer files'), so worlds stay
  portable. A metallic-roughness map that is left out makes the material
  a non-metal (glTF's metallic factor defaults to 1). An entity or scatter
  item given the mesh takes it under the look fields given (also when its
  mesh changes). Not read yet: other uv sets (dropped with a warning),
  texture transforms, alpha, emissive maps, WebP/KTX2 images.
- **Streaming (N1 part 4, `src/streaming.h`).** Cluster metadata (clusters,
  groups, LOD tree) stays on the GPU for every mesh, but the geometry
  (vertices, triangles) lives group by group in one fixed GPU pool
  (`RendererDesc::geometry_pool_bytes`, 256 MiB by default). A per-mesh
  residency table says where each group is, or that it is not loaded.
  Culling treats a missing group as precise enough, so the coarser clusters
  made from it draw instead, and asks for it; it also marks the loaded groups
  it used. The CPU loads what was asked for within a per-frame budget,
  parents before children (so the cut stays whole), and makes room by
  evicting the least recently used groups that have no loaded children. The
  coarsest level of every mesh loads first (within the same budget, meshes
  in the order added, except that a mesh the pool cannot hold yet does not
  block the rest; detail waits while any of it waits) and is never evicted;
  a mesh draws once all of it is in. Geometry comes from a CPU
  copy for now; world streaming (cells, from disk) will feed the same path.
- **Kits and shared materials (N1 part 5).** A kit is a named group of
  pieces (meshes, shapes, decals) kept in the scene; kit entities place it
  any number of times, and changing the kit changes every placement. The
  editor expands placements into instances each frame and gives equal
  materials one entry in a per-frame material table, which instances
  reference by index. An instance is 64 bytes (its 3x4 matrix, mesh,
  material and largest scale; the normal matrix is worked out in the
  shader). Draw calls do not grow with instances, meshes or materials:
  everything goes through the same raster and shading passes.
- **Decals (N1 part 5, `src/decals.h`, `shaders/decals.slang`).** A decal
  is a box that stamps a material onto whatever lies inside it, from its
  local +Y. The CPU projects each box to the screen and lists it in every
  16-pixel tile it covers; the shading pass tests only its tile's decals,
  in order, and blends their colour, roughness, metallic and emissive over
  the surface before lighting. Masks are shapes for now (disc, rectangle,
  ring, seeded splat and cracks), anti-aliased to the pixel's footprint;
  image stamps arrive with textures. Surfaces facing away from the stamp
  take none of it, and the box's top and bottom fade out. A `wet` decal
  (water plan W5f) wets every surface inside its box whichever way it
  faces, as rain does: darker where it is porous, glossier, its top edge
  (the waterline) soft.
- **Local lights (lights part L2a, `src/lights.h`, `shaders/lights.slang`).**
  `FrameInput::lights` takes lamps: points, and spots shining down a cone
  (inner and outer half angles, a smooth edge between). Light at distance
  d is colour x intensity / d^2 in the sun's relative units, eased to
  nothing at `range` by a windowed falloff (`(1 - (d/range)^4)^2`), with
  the source's radius added to d^2 so close surfaces never flare, and its
  highlights widened by that radius (Karis 2013). Like
  decals, they are binned on the CPU each frame (never late): the view is
  cut into 16 x 9 screen tiles by 24 depth slices spaced evenly in log
  depth from 0.5 m to 4096 m, each light's bounding sphere (a spot's is
  the sphere round its cone) lists it in the clusters it may touch,
  nearest lights first, at most 4096 lights a frame and 256 a cluster
  (`RenderStats::light_overflow`: the furthest were left out). Far
  lamps are merged first (lights part L5, a lamp LOD found by the night
  gate: a far town's thousand lamps fell in a few clusters, over their 256,
  and each pixel there would walk 256 lights): a lamp past 256 m whose
  sphere's radius is at most half a cell about a 32nd of its distance across
  (a power of two metres: 8 m at 256 m, 32 m at 1 to 2 km) is summed with
  the others in its cell into one point light at their power-weighted
  centre, reaching as far as they all do, its source as wide as they are
  spread, as bright as to light the ground 1.5 m under them as much as
  they all did (a spot as the point light sending as much light down its
  cone, its soft edge counted as shading counts it); a cell
  of one lamp keeps it as it is. Far lamps are grouped by distance alone,
  in view or not, and the group then tested for view, so turning the view
  never changes a group (only crossing a cell size's distance does).
  Merged lamps cast no shadows: a casting lamp sharing its cell with
  another (past 1 km for the gate's lanterns and street lights, 512 m for
  its 6 m window lamps) loses its shadow even when fewer than 64 nearer
  ones cast; one alone in its cell keeps it. The voxel world's lamps (below) are never merged here.
  `RenderStats::light_lamps` counts the lamps the shaded lights stand
  for, `lights_merged` the merged ones. Shading
  finds its pixel's cluster and lights the surface with each light listed
  there, GGX and Lambert as for the sun. Lit views only, not the far
  field (which takes them from the voxel world instead, below);
  `RenderSettings::local_lights` turns them off. Lights without a shadow
  id light through walls.
  In the world (lights part L2b), any entity can carry a `world::Lamp`
  (saved with it, only when set; kept while off) and so can a kit piece,
  lighting every placement. The editor's scene frame turns each into a
  render light where its entity or piece is drawn (the offset scaled and
  turned with it, a spot down its -Y), kept per cell beside its decals and
  moved in place with swinging lanterns, walkers and physics poses; hidden
  entities give none, a kit placement may carry its own beside its
  pieces', and a breakable kit's loose piece keeps its lamp lit where it
  lies. Scatter items carry none.
  Shadows (lights part L3, `src/local_shadows.h`): a light with a
  `shadow_id` (a lamp with `shadows` on; the editor hashes its entity and
  which of its lamps, so the id stays the same frame to frame) casts them
  from depth tiles of 256 x 256 in one atlas buffer
  (`RendererDesc::local_shadow_tiles`, 192 by default: 48 MiB), a spot one
  tile, a point light six (a cube round it, each face a little over 90
  degrees so filtering at its edge stays inside it). Depth is near /
  distance along the face's axis, larger nearer, written by the sun's
  shadow raster pipelines (`RasterPush::local_size` switches them to a
  tile, `InterlockedMax`). A light's faces drawn in a frame share one GPU
  culling (the box round its reach, open at the top; a lone face its own
  four sides and its reach: a face's planes are its four sides and a far
  plane at the light's range, not a near one, as the sides keep to what
  is in front and the projection clips what is nearer than `near`), each
  face rasterising what that found into its tile (the mesh shaders drop
  the clusters outside the face): the culling is a chain of small dependent dispatches and was
  most of the cost (the night gate's street: 24 faces each culling the
  whole world to the horizon, 2.6 ms on an RTX 5090).
  Tiles are kept by id frame to frame: a face is drawn again only when its
  light changes (range, cone, near: unshadowed until drawn), moves (the
  faces drawn from within 1% of its range, or 1 cm, keep shading
  meanwhile, so a swinging lantern over the budget keeps a shadow), or a
  caster near it comes, goes or moves (the sun pages' changed boxes and
  posed skinned meshes', tested against every kept light, in view or not,
  and only the faces that see the box: a sign swinging beside a lantern
  redraws the one face looking at it; the old depths stand in). At most
  `RenderSettings::local_shadow_faces_per_frame` faces (24) are drawn a
  frame, for at most 64 lights: faces with nothing to shade with first,
  then those whose old depths stand in, each drawn longest ago first, so
  lights changing every frame share the budget and all their faces come
  round; lights not wanted
  for 600 frames give their tiles back, and when tiles run out the oldest
  not wanted this frame give theirs. Shading picks the face (a point
  light's by its major axis), moves the point off the surface by a texel
  and a half along the surface normal and filters four taps bilinearly.
  They go with sun shadows (lit views, GPU culling). No contact shadows
  for local lights yet, and foliage swaying in the wind casts the pose
  it had when the face was last drawn (faces are not redrawn for swaying
  alone; a caster's changed box is grown by 2 m for what sways when
  picking the faces that see it).
  In the voxel world (lights part L4, `src/voxel_lamps.h`): each voxel
  has a lamp word beside its light word (a second buffer the size of the
  light pool, 64 MiB by default, made with it). When a
  brick is lit (`voxel_light.slang`), the CPU lists the lamps whose sphere
  touches it (a 32 m grid; at most 8, the brightest there first; a brick
  of 2 m voxels or more with more lamps takes those in each eighth of it
  merged into one, summed at their centre, so a far town lights it
  whole) and each
  filled voxel adds each one's light, over pi as light words hold it,
  with a march through the voxel world towards it for its shadow (the
  lamp's own fitting, within a couple of voxels of it, left out). Coarse
  voxels, far bigger than a lamp's reach, take its light over the voxel
  (the reach from its nearest point, at least half a voxel away, the
  normal wrapped as the lamp gets inside it, a voxel its reach only
  grazes taking as much less), so a town's lamps light the
  coarse ground kilometres off. The lamp word is what a voxel sends on:
  bounce (cones), voxel reflections, the voxel view and the far field
  (distant towns glow at night), never what pixels beside it take as
  indirect light (the lamps light those themselves). Lamp light changes
  reach the voxels as bricks are lit again in turn.
- **Anti-aliasing (N1 part 6, `src/smaa.h`, `shaders/smaa.slang`).** No
  temporal anti-aliasing: nothing uses earlier frames, so the image is as
  sharp moving as standing still. Edges: SMAA 1x (High preset, no diagonal
  search) runs after shading in three full-screen passes (edges from luma,
  blend weights from SMAA's area and search tables, then the blend into the
  target); `RenderStats::post_draws` counts them. It is skipped in debug
  views. Shading's image is finished (screen reflections, then exposure,
  bloom and tone mapping in `post.slang`) into its own sRGB image first. Specular: in the
  shading pass, the change in normal across each 2x2 pixel quad (same
  instance only) widens the GGX roughness (Kaplanyan-Hoffman style, capped),
  so highlights on tight curves stay stable instead of flickering.
- **CPU path.** `gpu_culling = false` chooses the cut on the CPU (frustum
  only) and feeds the same raster and shading passes; tests use it for
  exact counts.

## World cells (N3 part 1)

The world is cut into square cells on the ground, 64 m a side
(`world::k_cell_size`, `world::cell_of`); an entity belongs to the cell its
position is in. Work goes by cell, so a change to one entity costs its
cell's work, not the world's:

- **The scene says what changed.** `world::Scene` hands out entities to
  change only through `modify` (reading is `find`, const), and logs the ids
  added, removed and modified; `take_changes` hands the log over. A new,
  copied or assigned scene, `clear`, or a log grown past the scene's size
  says "all". Kits and lighting are not logged (compared instead).
- **The editor keeps the frame by cell** (`editor/src/scene_render.h`).
  Each cell keeps its instances and decals; a frame rebuilds only the
  cells whose entities changed, or that place a kit that changed, and
  copies them into place. A new scene, an imported mesh, or a material
  losing its last user (the table is made again, so it holds only what is
  drawn) rebuild everything. Each cell keeps its place in the instance
  list from frame to frame (N3 part 3b): a cell that goes leaves a hole
  (instances with no mesh, which nothing draws, sent as a cell of its
  own, in pieces of at most 1024, each keeping its place and version
  until a cell takes it, so the renderer looks at it once; joined with
  the holes beside it to find
  room), a new cell takes the first hole it fits or the end (an empty
  cell sits at 0), and a cell that outgrows its place moves, with a
  quarter more room. So cells coming and going (streaming) leave the
  others where they are. When holes no cell has pass half the list (and
  4096), or push the list past `render::k_max_instances` when laying it
  out again would not, and on a new scene, the list is laid out afresh, end to end, by
  z then x (within a cell by id). `FrameInput::cells` covers the list
  end to end: each cell's (and hole's) place, size and version (new on
  every rebuild).
- **The renderer looks only at cells that changed.** The voxel world and
  sun shadows each keep a copy of the instances to find what moved; they
  compare only the cells not exactly as when they last looked (place,
  size and version, `src/cells.h`), so cells joining, leaving or moving
  cost only themselves. Otherwise (no cells, or fewer instances than
  before: the list laid out afresh) they compare everything, as before.
  A cell whose ground alone changed (its terrain or far ground tile mesh,
  rebuilt in place by the editor) says so: `CellRange::prev_version` and
  `changed_first`/`changed_count` name the part that changed, and one
  seen at `prev_version` is compared over that part only, not its
  thousands of scatter instances. Meshes coming and going (tiles stream
  all the time) make the voxel world and shadows look again only at the
  instances of meshes that went or changed bounds, found from a packed
  list of each copy's mesh.
  `RenderStats::cells` and `cells_changed` count them (holes included).
- **Instances stay on the GPU (N3 part 2, `src/instance_store.h`).** One
  buffer keeps every instance, in frame order, from frame to frame; a frame
  uploads only the cells not as before (all of them for a fresh layout,
  a new material table size, or without cells). Translations are kept from each
  cell's anchor (its first instance when uploaded; each cell keeps its
  anchor slot while it stays as it is, and new ones take free slots),
  which keeps them precise in float however big the world; each frame writes the anchors
  relative to the camera (16 bytes a cell) and a bit per instance saying
  whether it is drawn (its mesh ready, not left to the far field), and
  `load_instance` adds the instance's anchor back. Culling walks every
  instance and skips the unset ones; instances are addressed by their
  place in the frame everywhere. Per frame that is a bit per instance
  instead of 64 bytes.
- **Worlds stream by cell (N3 part 3, `editor/src/world_streaming.h`).** A
  world is a folder: `world.json` (`world::WorldIndex`: lighting, kits,
  the meshes its cells use, which cells have a file, the next id) and
  `cells/<x>_<z>.cell.json`, one per cell with entities or a terrain tile
  (`world::save_cell`: sorted by id, so an unchanged cell saves the same
  text; terrain in streamed worlds is under Terrain, below). Cells with any
  part within `load_radius` of the camera are read and parsed on a worker
  thread, nearest first, a few at a time; the main thread adds whole
  parsed cells to the scene up to `entities_per_frame` a frame and never
  waits on disk. Every `unload_every` frames, loaded cells past
  `load_radius + unload_margin` are let go, but only when what the scene
  holds of the cell is, entity for entity, what its file held (kept in
  memory while loaded): a cell with unsaved edits stays loaded until
  `save_world`. An
  entity moved (or put back by an undo) into a cell still on disk makes
  that cell load at once, and an entity id already in the scene is
  skipped when its cell loads (the scene's copy is newer). Saving writes
  only cells whose entities changed, through a temporary file and a
  rename, after `world.json` (with a next id past every id about to be
  written, and every cell about to have a file: a listed file that is
  missing loads as an empty cell); a cell on disk that the scene has
  entities in is merged with its file. An id met in a cell's file that
  the scene holds elsewhere, and was not read from that cell, is a clash
  (two files share it, or a new entity took it): the file's entity gets a
  new id, so neither is lost. Streaming runs only while the scene is the
  world's (`new_scene`, `load_scene` or undoing `open_world` pause it;
  undoing them resumes it). What streaming knows of the world's scene is
  kept with each copy of it an undo step holds (`WorldStreaming::memo`),
  taken when the undo or redo leaves that scene (unless a script replaced
  it since: a script's scene is never kept), so the copy put back is the
  scene as it was left, cells loaded since included. Which cells have a
  file stays as it is now; a cell whose file was written since the memo
  was taken is read again and merged, and the ids saved since are never
  handed out again. An entity's cell of origin is kept when its cell
  unloads, so an entity a redo puts back is never copied from the file it
  was saved to since (this costs about 40 bytes per entity met since the
  world opened: a later part bounds it), and a save writes again, without
  it, the file on disk of a cell an entity has left. Ids any of the
  world's scenes has handed out are never handed out again by another
  undo side. A save keeps the bookkeeping true to each file as it writes
  it, so one cut short by an error loses nothing either; a
  scene replaced any other way stops streaming. Opening another world is
  refused while the open one has unsaved edits, or its scene is replaced
  (it may be in the undo history), unless they are given up. The main thread does no work
  per frame that grows with the world: the cells to read are worked out
  again only when the camera enters another cell or a cell goes back on
  disk, and counts and the loaded cells are kept as they change. Loaded
  cells also keep their file's entities in memory (twice the entity
  memory of what is loaded). A material falling out of use no longer
  rebuilds every cell: unused entries stay in the table until they
  outnumber the used ones (and 256).
- **Known limits, next parts.** The CPU still looks at every instance each
  frame to decide which are drawn (mesh ready, far field) and which cast
  shadows: cheap per instance, but whole-cell tests come next. Streamed
  out cells are gone from the voxel far field too (cells need saved voxel
  stand-ins for that, later). While many cells stream in at once (more
  than an eighth of the scene in a frame, or over 4096 64 m squares of
  changes waiting) the voxel world still rebuilds every window; holes cost a few bytes per
  instance a frame in the loops that walk every instance. Meshes are imported when a world opens (the
  index lists them), not as cells stream. Unloading a cell is one
  `Scene::remove_many`, a pass over the scene's entities, and the unload
  look walks the loaded cells. Undoing an edit to a cell that was saved
  and unloaded since does nothing. A world cannot yet be saved to another folder, and its kits cannot
  be deleted while some cells are not loaded. Decals overlapping across a cell border draw in cell
  order, not the outliner's.

## Terrain (N3 part 4)

The ground is a height field (`world::Terrain`, `engine/world/include/giant/world/terrain.h`),
cut into tiles, one per world cell, kept in `Scene::terrain`:

- **Samples.** A tile holds `samples()` x `samples()` heights (8 to 128 a
  side, a power of two: 8 m to 0.5 m apart; 64, 1 m, by default; set only
  while there are no tiles), the first at its cell's corner. The surface's
  last row and column come from the neighbours; `Terrain::sample(i, j)`
  takes a height by whole-world sample number, from its tile or, when that
  tile is missing, from the sample before it on x, z or both. Tiles
  sharing an edge always see the same heights and normals along it; a
  tile touching another only at a corner, with neither tile between them,
  keeps its own corner (no spike, and no edge to crack along). `height_at(x, z)` is the surface exactly as the finest
  mesh draws it (two triangles a square, split on the (0, 0)-(1, 1)
  diagonal).
- **Changes.** The terrain logs the tiles set, changed (`modify`) and
  removed, as the scene logs entities (`take_changes`; a new, copied or
  assigned terrain is "all").
- **Meshes** (`editor/src/terrain_meshes.h`). Each frame `TerrainMeshes`
  takes the changes and queues the changed tiles and their eight
  neighbours (a tile's mesh reaches one sample into them). It looks at a
  few queued tiles a frame (8 x `builds_per_frame`): a snapshot of the
  heights the mesh is made from (the tile's rows copied, the ring around
  them looked up), hashed; a tile whose snapshot matches what it draws
  (a neighbour whose shared edge did not change, or a whole terrain
  copied by an undo) is not built. The rest are built on two worker
  threads (`builds_per_frame` started, `builds_in_flight` at once): the
  job makes the triangles and normals and runs `geometry::build_clusters`
  with the tile's edge vertices locked (`meshopt_SimplifyVertex_Lock`),
  so every level of detail keeps the edge and tiles drawn at different
  detail meet without cracks. Finished meshes are uploaded a few a frame
  (`uploads_per_frame`) and replace the old one only once
  `Renderer::mesh_ready` says they can draw, so a tile never blinks out.
  A tile changed while it builds is built again with the newest heights.
  The renderer hands a removed mesh's id (and its streaming group
  numbers) to the next mesh added, so rebuilt tiles do not grow the mesh
  table every frame walks; callers stop drawing an id when they remove
  it.
- **Drawing.** `scene_frame` puts each built tile first in its cell, at
  the cell's corner, with the terrain's material and
  `render::Instance::always_mesh`: the far field never stands in for it
  (the CPU never leaves it to the voxels, its voxels are not stand-ins,
  and the resolve pass does not skip stand-in voxels around it, so far
  trees on a tile still show). It is in the voxel world like any mesh, so
  it casts shadows and takes part in GI and reflections. `always_mesh`
  travels to the GPU in the top bit of `gpu::Instance::material`.
- **Saved as text** with the scene (`"terrain"`: `samples`, `material`,
  `tiles` of `cell` and `heights`, one string of numbers per row in the
  shortest form that reads back exactly).
- **In streamed worlds (N3 part 4b).** A cell's file holds its tile too
  (`"heights"`, rows of text as in a scene), so a tile loads and unloads
  with its cell: a cell is let go only when its entities and its tile are
  exactly what its file holds. That check is one compare: every tile has a
  version (`Terrain::version`, new on each set or modify, unique across
  terrains, kept by copies and put back by undo), and a loaded cell keeps
  the version its file's tile had in the scene, so no second copy of the
  heights is kept and nothing compares them. A tile weighs samples^2 / 64
  entities against `entities_per_frame` (64 at 64 samples), and at most
  `unloads_per_frame` cells (128) go in one frame, the rest in the frames
  after; their meshes are freed `drops_per_frame` (64) a frame. A tile new
  to `TerrainMeshes` waits `settle_frames` (6) with its neighbours before
  it is built, so a burst of tiles (a world opening) builds each once
  (changes to tiles already there do not wait). Flying, a tile at the
  edge of the loaded area is still built again when the next row arrives
  and when a neighbour unloads (its edge reaches into them): about 2.8
  builds per tile. `world.json` holds the terrain's `samples` and
  `material` and which cells' files hold a tile (`terrain.cells`). A tile made or put back by an undo in a cell still on
  disk makes the cell load at once, and the scene's tile wins over the
  file's; saving such a cell keeps the file's entities (and its tile, if
  the scene has none there). All tiles are one spacing: while any file
  holds a tile, `create_terrain` takes the world's spacing and refuses
  another, streaming gives an empty terrain the world's spacing back (an
  undo can take it away), a file whose tile is another spacing fails to
  load alone, and a save that would mix spacings is refused.
- **Far ground (N3 part 4c).** Past the loaded cells the ground is drawn
  from coarse copies of the terrain (`engine/world/include/giant/world/far_ground.h`,
  `editor/src/far_ground.h`), a quadtree of branching 8: level 1 chunks
  are 512 m (8 x 8 cells) with 32 x 32 heights 16 m apart, level 2 chunks
  4096 m (8 x 8 level-1 chunks) with heights 128 m apart. A chunk's heights
  are the finer level's at its own sample points (no averaging, so levels
  agree where they share a point); NaN where there is no ground (a cell
  with a tile has all 16 of its samples, one without none). Each level is
  a `world::Terrain` of 512 m or 4096 m tiles that may hold NaN, with its
  own `TerrainMeshes`, so chunks mesh like tiles (triangles with a corner
  where there is no ground are left out). Files: `ground/1/<x>_<z>.ground.json`
  and `ground/2/...` (format `giant-ground`, 32 rows of numbers, `-` for
  none), written by `save_world` after the cell files over every cell whose
  tile changed: the chunk's old heights where nothing changed, the tiles
  (the scene's, or read from their cells' files when the chunk had no file
  yet) elsewhere; a chunk left with no ground loses its file, and the save
  hands what it wrote to the far ground at once. The editor reads chunks
  on a worker within `far_radius_1` (8192 m) and `far_radius_2` (32768 m)
  of the camera, adds `chunks_per_frame` (16) a level a frame and lets go
  of ones past the radius plus a margin (512 m, 4096 m). What draws:
  a level-2 chunk draws its mesh unless every level-1 chunk in it can
  stand in (its mesh is drawn, every cell of it with ground has its full
  tile drawn, or it has no file); a level-1 chunk under no drawing parent
  draws its cells' full tiles when all of them are drawn, else its mesh,
  else what tiles it has; a full tile draws only where its level-1 chunk
  says so. The choice is worked out only for chunks whose inputs changed
  that frame (a tile's or chunk's mesh came, went or was replaced; a
  chunk was added or let go). Chunks draw from the cell at their corner
  (always_mesh, the terrain's material). Every tile and chunk hangs a wall
  from its edges (`TerrainMeshes::skirt_depth`: 8 m full tiles, 64 m level
  1, 256 m level 2, seen from both sides) that fills the gaps where tiles
  of different spacing meet. Cells with ground load and unload by their
  level-1 chunk's distance, so a chunk near the camera has all of its
  tiles; a chunk with an unsaved tile keeps all of its tiles loaded, so
  near edits always draw in full (the chunk files are the saved ground).
- **Known limits, next parts.** The far ground shows edits only once
  saved (a chunk far enough to draw coarse shows its file). Level 2 takes
  only every other cell's corner, so ground narrower than 128 m (a lone
  tile) may not show at all at that distance, and a level-2 chunk whose
  points make no square draws nothing (its children do as they can).
  `far_radius_1` is taken as at least `load_radius` + 5800 m (a level-2
  chunk's diagonal), so a level-2 chunk never draws over the full tiles
  by the camera. A chunk file that cannot be read stands in for nothing
  (its full tiles draw where loaded), and a cell whose file failed is not
  waited for. The coarse ground stops one of its own samples short (16 m,
  128 m) at the edge of the ground, where the tiles' walls hang. A save
  cut short owes its chunks to the next save, kept in `world.json`
  (`terrain.far_owed`, `far_owed_2`: written before the cells, cleared by
  the save that writes them), so a crash or reopen does not lose them. Every tile gets the 8 m edge walls, streamed world or not.
  A world saved before part 4c has no chunks until its tiles
  are saved again. Every loaded
  tile is a full mesh in memory (CPU copy and cluster data: about 0.4 MB
  a 1 m tile, 1.2 MB at 128 samples; the default radius loads about 1,800
  cells, more now that ground loads by whole 512 m chunks), and a command may make at most 16.7M heights, the terrain hold
  67M (16,384 tiles of 64 samples, 4,096 of 128). Locked tile edges keep
  every level's edge, so a tile never drops below about 4 x samples
  triangles (256 at 64 samples) however far. An undo step holds copies of the tiles a command changed
  (before and after), and a scene swap's undo side the whole terrain. One
  material for all of it (changing it rebuilds every cell with ground,
  once); painted layers and erosion come later. Like lighting, a changed
  terrain material does not count as an unsaved cell.
- **Terrain editor (N3 part 5).** Brushes (`engine/world/include/giant/world/terrain_brush.h`):
  a round brush of a radius, full strength inside `radius * (1 - falloff)`
  and easing (smoothstep) to nothing at the rim, dabbed at least every
  quarter radius along a stroke. Raise and lower add metres; flatten moves
  towards a height and smooth towards each sample's 3 x 3 average (read
  from a copy taken before the dab, across tile edges), both by up to the
  strength (0 to 1). Each sample belongs to one tile, so a dab over a tile
  edge changes both tiles' own samples and their meshes stay joined. Only
  tiles that exist change, and a tile no height of changes is left as it
  is (no new version, no undo step). Smooth copies only the tiles there
  are. `sculpt_terrain` refuses a call over 4096 cells, 65536 dabs or 16.7
  million heights reached (dabs times each one's square), so one call is a
  bounded hitch; ground saved in an open world but not loaded is left as it
  is (`cells_not_loaded`; the loaded tile's edge can then meet it in a
  slope, as smooth reads no heights there). One undo step of a drag (its calls merged:
  `CommandContext::merging`) holds each tile once: as it was before the
  drag first changed it, and, taken when undone, as it was after (for
  redo). The viewport brush (`editor/src/terrain_brush_tool.h`) casts a ray
  from the camera through the pointer, marches it over the ground (half
  the sample spacing near, steps growing a 500th of the distance) and
  refines the hit, and sends one `sculpt_terrain` a frame while the left
  button is held (only when it moved a quarter radius, or 30 times a second
  held still, so the frame rate does not set the speed; no ray is cast
  until the pointer or camera moves or a dab is due), in one merge group:
  the stroke is one undo step. A move of over 4 radii is bisected along
  the pointer's path (up to 6 more rays), keeping the half least like
  smooth ground: its gap against the pixels between times the ground a
  pixel covers at its ends (distance x pixel angle / cosine to the ground,
  floored at 1e-5;
  their geometric mean is exact for flat ground at a grazing angle). A
  fast drag over far ground stays one line; a gap of over 4 radii and 4
  times what smooth ground would give (the pointer over a ridge), or a
  jump since the camera moved, starts the line again where it lands. The panel
  keeps the radius to 128 samples (`BrushSettings::max_radius`), and
  flatten goes to the ground where the stroke began unless a height is
  given. Far ground (past the loaded tiles) cannot be sculpted. Height maps
  (`engine/world/include/giant/world/height_map.h`): 16-bit RAW (.r16,
  .raw; square, little-endian) and PGM (P5 or P2, 8 or 16 bit), up to
  16384 a side, sampled bilinearly into tiles at the terrain's spacing; a
  `to` on a cell's edge also sets the first samples of a tile past it.
- **Scatter (N3 part 6).** Rules for strewing things over the ground
  (`engine/world/include/giant/world/scatter.h`; the layer types are scene
  data in `scene.h`, saved with scenes and in world.json). A layer's points
  come from a global grid of `spacing` squares, one point a square jittered
  inside it (no two closer than spacing x (1 - jitter)), each square's
  numbers from a hash of the seed and the square, so a point is the same
  whichever cell asks and cells share none. Points are kept by density, a
  value-noise clump mask, the area, and the ground's height and slope
  (normal from heights a sample either side); each gets an item by weight,
  a turn about up, a scale, and a tilt towards the normal (`align`).
  `scatter_cell` reads only the cell's tile and its neighbours', so the
  editor's `ScatterPlacer` (`editor/src/scatter_placer.h`) copies those 9
  tiles to a worker job (2 workers) and swaps a cell's points in whole
  (within `instances_per_frame`, one cell at least). Only cells within
  `radius` (2048 m) of the camera are placed, nearest first; a cell draws
  at most 16384 instances (a frame swaps in one whole, about 1.5 ms;
  otherwise `instances_per_frame` is 4096, since every part of the
  renderer looks at each placement the frame it arrives) and
  all cells together `max_instances` (524288; a kit item counts its
  pieces, and a kit's piece count is part of every cell's key). A cell
  arriving over the budget takes room from farther cells, cells out of
  range go before any arrive, and cells cut short try again when room
  frees. The scene's instance list reserves room for scatter once, so it
  never doubles in a frame. A cell is placed again when its
  tile or a neighbour's changes (TerrainMeshes::last_changes) or the layers
  change (compared each frame), and keyed by the layers and those 9 tile
  versions, so an unchanged cell is never placed twice. Placements are not
  saved and not entities: `scene_frame` draws them after the cell's
  entities (items' meshes and materials looked up once a cell, kits
  expanded), so they stream with the ground, reach the voxel world and far
  field like any instance, and a kit change rebuilds scattered cells. At
  most 2^18 points a cell (`cells_cut_short`). Not yet: masks from
  entities (no trees in houses), layers avoiding each other, painted
  density, and destruction removing placements.
- **Weather and sky (N3 part 7).** `world::Weather` (`weather.h`) is
  optional scene data (saved in scenes and world.json); without it the flat
  sky colours stay exactly as before. The editor's `WeatherState`
  (`editor/src/weather_state.h`) is view state: it follows the scene's
  weather, blends towards a new one over `over_seconds`, and carries the
  cloud pattern along on the wind (an offset, so the clouds move without
  history). `weather_light` turns sun height, haze and cloud cover into
  the sun, sky and ground light the shading uses (the sun through the air
  by air mass, the sky greyed by cloud). The renderer gets a `Sky` in
  `FrameInput` and runs, before shading (`sky.slang`, `sky_common.slang`):
  a 192x108 sky-view table of single scattering (Rayleigh and Mie, round
  earth), a 512^2 tiling cloud pattern made once, a 256^2 cloud shadow map
  over 32 km about where the sun's rays from the ground near the camera meet the cloud base (snapped to texels; past it, the clouds' mean shade), and half-resolution
  volumetric clouds (a shell between base and top, a fixed dither, no
  temporal history: rule 2) written as light and transmittance plus the
  distances to the first cloud and to the scene. The resolve upsamples the
  clouds by nearest depth, composites them over sky and scene, adds height
  fog (exponential in height, capped at 30 km) and darkens the sun by the
  cloud shadow (screen reflections are added scaled by what of the pixel comes through). A change of cloud size rescales the wind offset about the camera, so the clouds overhead stay put. Settings `clouds`, `cloud_steps` (48) and `cloud_shadows`.
  Not yet: lightning, clouds parting round giants.
- **Physical sky (photoreal part 1).** With `RenderSettings::physical_sky`
  (default on), the sky passes add three tables, without history, all
  inside the "sky" timing (the last two every frame):
  - **Light scattered many times** (Hillaire 2020). A 32^2 table, indexed
    by the sun's cosine and the square root of height:
    - each texel marches 64 directions with 20 steps;
    - it collects the light scattered once towards it (an even phase,
      plus the sunlit ground's share) and the fraction the air scatters
      again (f_ms), and stores L2 / (1 - f_ms).

    The sky view (`sky_radiance`) and the air over the view add it to
    single scattering, for a deeper, brighter sky and the glow after
    sunset. The table depends only on the haze, so it is made again only
    when the haze changes.
  - **The air over the view** (aerial perspective):
    - 32^2 rays through the screen, each marched over 32 slices ending
      `aerial_distance` * ((k + 1) / 32)^2 away (default 32 km, two steps
      a slice);
    - each slice stores the light scattered in and, per colour, what
      comes through, in two 1024x32 textures with the slices side by
      side, read with a half-froxel clamp so no slice bleeds into the
      next;
    - past the last slice, its air carries on alike (transmittance to the
      power of distance over `aerial_distance`), so far ridges fade like
      the horizon.

    The resolve puts it over every surface, meshes and far-field voxels
    alike (`aerial_at`, before the height fog), and over the clouds up to
    the first of them. The sky background has it already. Far land fades
    blue and the far sunlit land reddens.
  - **The sky's light from all round.** One group projects 1024
    directions onto 9 spherical harmonics, convolved with the cosine and
    over pi:
    - the upper half is the sky view table;
    - under the clouds, as they cover it, it is their deck: grey and
      even, the clear sky's light plus half the sunlight their shade
      takes out;
    - the lower half is the ground (albedo 0.2), lit by the sun through
      the clouds' mean shade and by that sky above;
    - in the dusk (the sun 3 to 6 degrees under the horizon) it goes
      over to the flat `sky_light`/`ground_light` (the night's; no moon
      or stars yet).

    `ambient_light` / `ambient_light2` (common.slang) evaluate it for
    shading's diffuse and specular ambient, global illumination's sky
    (cone misses, unlit voxels) and dust, in lit views (the voxel view
    keeps the flat colours).
    `sky_light_of`/`ground_light_of` (sky_common.slang) do the same for
    the clouds' light, rain and snow, and the fog's colour. Together they
    replace the flat hemisphere (`FrameConstants::sky_flags` bit 0,
    `SkyParams::atmosphere` bit 2). Global illumination lights voxels
    before the frame's sky passes, so it reads the table filled a frame
    before; until one has been, it uses the flat colours.

  Off, or with the sky off, everything is as before. The sky's ambient
  is physical, about 8% of the sun on flat ground by day. That is 2 to 4
  times darker in the shade than the old flat colours, which were tuned
  by eye. Exposure (part 2) brings the image back up, and light is still
  in the engine's relative units until then. The air over the view has
  no shadows (no light shafts yet). The multiple-scattering table takes
  the sun's angle at the camera everywhere along a ray (as single
  scattering always has), so very long horizon rays shift the twilight
  glow slightly.
- **Exposure, bloom and tone mapping (photoreal part 2, `shaders/post.slang`,
  `src/exposure.h`).** Shading (and the screen reflections) write the
  light before exposure into an rgba16 float target (alpha 1 where it is
  lit, 0 for the background and debug views, which are shown as they
  are). After them, in the "post" timing (lit views):
  - **Auto exposure** (`auto_exposure`, default on). A compute pass counts
    the lit pixels' luminance into 64 bins: bin 0 is black and anything
    under 2^-16, bins 1 to 63 are even in stops up to 2^8 (0.38 stops
    each). The counts are copied to a read-back ring of three and read on
    the CPU once the GPU is done with them (two frames later). Black (bin
    0) is left out; of the rest, the 10th to 90th percentiles, averaged in
    stops, give the scene's brightness (all black holds the exposure); the
    target exposure brings it to middle grey (0.18), plus
    `exposure_compensation` stops, within `exposure_min` to `exposure_max`
    stops. The exposure eases there at `exposure_speed_up` stops a second
    brightening (default 3) and `exposure_speed_down` darkening (1.5), over
    `FrameInput::delta_seconds` (the editor passes its real frame time, so
    eyes adapt while paused too; a screenshot's extra frame passes 0). The
    first measurement is taken at once; until then, and with it off,
    `Lighting::exposure` alone. Turned off and on again, it eases on from
    where it was; settings changes apply at once to the last measurement. It is always
    multiplied in. It is one number, eased: no image history, so nothing
    smears. The light it measures is before exposure, so it never measures
    itself.
  - **Bloom** (`bloom`, default on). The light halves six times (Jimenez's
    13-tap filter; on the first, each texel held to 64 times white once
    exposed, so a lone blazing pixel cannot flicker it, and only lit pixels
    counted, each by its own alpha, so the flat background never bleeds
    in), then comes back up (each level plus a 3x3 tent of the one below).
    Alpha carries how much of each texel was lit, so the top level divided
    by it is the average of the lit light round a pixel: it takes the place
    of `bloom_strength` (default 0.04) of each pixel's light, so bloom only
    moves light about and adds none, even beside the background. It is
    skipped in debug views. If its textures cannot be made (out of
    memory), the frame draws without it; they are tried again on a resize.
  - **Tone mapping** (`tone_mapper`): Khronos PBR Neutral (default: base
    colours true until highlights roll off) or AgX (Wrensch's fit of
    Sobotka's: very bright colours go gently over to white). Middle grey
    (0.18) shows as about sRGB 105 under PBR Neutral, 129 under AgX. It writes the sRGB image SMAA then
    smooths. The dust over a flat background (no sky) is tone mapped in
    shading, with the same tone mapper (`FrameConstants::tone_mapper`).

  `RenderStats` (`last_frame.exposure` over MCP) gives the multiplier
  used, whether a measurement has come back and the scene's brightness in
  stops. Costs: the histogram is one dispatch over the screen with shared
  memory bins; bloom is 11 small dispatches at half resolution and below
  (about 2.7 MB of extra targets at 1440p). The light stays in the
  engine's relative units (the sun about 3); auto exposure makes the
  absolute scale not matter, and real units (lux, EV100 cameras) are not
  done. Not yet: lens flare, local exposure.
- **Rain, snow and wet ground (N3 part 8).** `world::falling` splits the
  weather's precipitation into rain and snow by temperature;
  `world::weather_ground` steps the ground's wetness and snow cover
  (exponential approaches in steps of at most 10 s, so any step size
  agrees) from the scene's start values when weather first shows or the
  scene is replaced; the editor's `WeatherState` carries it on each frame
  under the shown weather (view state, like the blend), and set_weather's
  `wetness`/`snow_cover` put it somewhere at once. The renderer's `Sky` takes what falls, the wind,
  the ground and a time. In the sky passes a cover map (256^2, a metre a
  texel round the camera, r32) marches the voxel world straight down per
  column (from 800 m above, or the coarsest window's top, to the finest
  window holding it, then down that window) and stores the height of what
  is on top, less the size of the voxel it hit (the march reports it,
  whichever level or stand-in brick it was found on).
  `weather_surface` (sky_common.slang) then wets surfaces open to the sky
  (darker by porosity, rougher surfaces most; glossier; puddles from a
  noise that repeats every 4096 m in the hollows of flat ground) and lays
  snow on what faces up, before shading, on meshes and far-field voxels
  alike (walls judged from the air beside them). Voxel-cone reflections
  follow the dry roughness, so rain does not put a cone on every open
  pixel (the wet gloss shows in screen and sky reflections). `apply_falling`
  draws rain streaks or snowflakes on five cylinders round the camera (1.5
  to 24 m), a hash per cell of a fixed size per cylinder, falling whole
  cells per 64 s cycle so the time wraps without a jump, the drops and
  their streaks leaning with the wind (a bounded shear, so cells never fold
  across columns; not carried round, which would differ by view), none
  where sheltered: no particles, no history. Settings `precipitation`, `wet_surfaces`,
  `weather_cover`. Not yet: ripples, splashes, drips, wet voxel colours for
  GI, snow depth (it is a coat, not a volume).

## Physics (N4 part 1)

`giant::sim` holds the physics world (`sim/physics.h`), Jolt underneath
and kept private: callers see plain types (`BodyDesc` of box, sphere,
capsule, convex hull and height-field parts; `BodyId`; `RayHit`). Jolt is
built in double precision, so bodies sit at world positions tens of
kilometres out without a moving origin. Steps are fixed at 1/60 s, at most
four a call (the rest is dropped and counted, so a slow frame never
snowballs into more work), and run on a `JobSystem` of their own: the
editor gives physics half the cores, two to four, so it never waits on
the voxel, scatter or terrain workers.

`ScenePhysics` (`sim/scene_physics.h`) keeps the physics world in step
with a `world::Scene`: each entity whose shape collides gets a body made
from what the renderer draws (a cube a box, a sphere a ball or, scaled
unevenly, a hull, a plane a 0.5 m slab whose top is the plane, a kit one
fixed compound of its pieces; sizes under a millimetre count as one), and
each terrain tile a height field of its own samples plus its +x and +z
neighbours' edge (the same triangles the mesh draws). It reads the change
list the frame took (`SceneFrame::take_for_followers`) and the terrain's
own second change log (`Terrain::take_physics_changes`), so a frame looks
at what changed, not the scene's size; a moved entity is teleported, a
resized one remade, and a body that goes or moves wakes what slept on it.
`Entity::body` says how an entity collides (`fixed`, `dynamic`, `none`)
and `Entity::mass` its weight (0: from its size, water's density); both
are saved only when not the default. Meshes and decals do not collide
yet, and kits stay fixed whatever their body (they break rather than
fall, N4 part 4).

Simulated motion never goes into the scene. The dynamic bodies' places
are poses beside it (`ScenePhysics::poses`), and the editor draws those
entities there (`scene_frame`'s `poses`, rebuilding the cells of the ones
that moved); the scene keeps where things were put, which is what undo,
saving and streaming see. Simulating in the editor
(`editor/src/physics_state.h`) is view state, like a play button: stopping
drops the poses and puts the bodies back where the scene has them. The
bodies follow the scene while stopped too, so starting costs nothing and a
raycast answers at once. A body remade while simulating (its size, body
or mass edited) starts again from where the scene has the entity.

Ragdolls (C8a). `PhysicsWorld::add_ragdoll` makes a tree of dynamic
bodies (bones, parents first, at most 128) joined each to its parent at
its own origin by a swing-twist joint: a cone the bone may lean within and
limits on its twist about its own axis, about where it is placed or about
a `centre` given (a ragdoll made mid-stride keeps its rest pose's limits).
A bone and its parent never collide (they overlap at the joint; a group
filter per ragdoll), nor do pairs the description lists (`no_collide`:
thighs at the hips, a spine's short bones); other bones, and other
ragdolls, do, and a hard contact between two bones of one ragdoll is not
reported as a hit. Limp it falls as
bodies do, a little friction in each joint. `drive_ragdoll` turns each
joint's motor on toward a turn in its parent's space, as a spring of a
frequency, damping and most torque (an active ragdoll: it follows a pose
and is still pushed about; a motor sags under load as a spring does, less
the stiffer); a new target wakes it, the same one leaves it asleep;
`relax_ragdoll` lets it go limp. A turn asked for outside a joint's limits
is driven to as near as they allow. The root is not driven: its mover
sets its body's velocity. `move_ragdoll` places the whole of it as one
piece (a bone alone is not placed: `set_transform` and `move_kinematic`
leave bones be, as a bone moved alone tears its joints). Bones are bodies like any other with the ragdoll's `user`
(`ragdoll_bodies`, `ragdoll_bone`: rays, contacts, `add_impulse` and
`state` work on them), but `remove` leaves them: they go with
`remove_ragdoll`. A description that cannot be is refused whole. `clear`
lets every ragdoll go (their owner sees `clears()` move on and forgets
them; `ScenePhysics::clear` only takes the fresh-world path when every
body is accounted for, so the editor's physics counts the bones,
`ragdoll_bones()`, into `others` with the forest's trunks). A bone with a
`hinge_axis` folds one way (an elbow, a knee): up to `swing_degrees`
either way about that axis and `side_degrees` the other way (0: not at
all), so a knee that folds 140 degrees is a hinge of 70 whose `centre` is
the leg half folded. `sim` does not see
`anim`: the editor builds descriptions from skeletons.

Editor ragdolls (C8b, `editor/src/ragdoll_state.h`).
`ragdoll_entity` makes a visible skinned mesh entity a ragdoll while
simulating, as it is posed and drawn at that moment (a walker where it
walks, with its velocity; it walks no more). Bones come from the capsules
fitted round the joints at import (the hit shapes): a joint gets a body
when its capsule is at least a twenty-fifth of the character's size, so
fingers and toes ride the hand and foot (every joint follows its own bone
or the nearest above it, keeping its place in that bone's space); mass is
the capsule's volume as water, none lighter than a fiftieth of the
heaviest. Joints are told apart by name (`RagdollState::kind_of`: the
Mixamo, Unreal and Blender names): forearms and shins are hinges folding
140 degrees, forward and back (the character faces +z; the axis is square
to the bone at rest and to that way), the rest cones about the rest pose
(`limits_of`). Bones that overlap in the rest pose, hang from the same
bone, or are two apart never collide. Each frame after the physics steps,
a ragdoll whose bodies are awake hands its joints' model matrices to the
animation (`AnimationState::hold_pose` / `set_held_pose` /
`release_pose` (which can fade from a pose given: getting up, below): a pose from outside shown instead of the clips' or the
matcher's: a clip's time moves on underneath, a matcher waits; posed when due, as any other,
and only when it changed: one asleep works out no pose; its size on
screen for animation LOD is told from where it is drawn), and is drawn where
its first bone has carried the entity's transform (`RagdollState::poses`,
handed to the scene frame, the occluders and the hair before the walkers'
poses; always marked moving). The scene keeps where the entity was put.
While it is one it has no hit capsules: bullets meet the bones' bodies (as
flesh, naming the entity, and the bone's joint and its zone by the file's
zone map: `PhysicsState::ragdoll_joint`) and push them. Let go (`stop_ragdoll`, the
simulation stopped, another scene, a game loaded, the entity hidden,
removed or drawing another file) it stands where the scene has it and its
clips pose it again (one with nothing to play is animated no more). Each
bone starts moving as its joint was, over the character's velocity: the
animation keeps the pose it worked out before the one it shows
(`Animated::before`, `moved_seconds` apart; none across a cut: a clip
started, the matcher taking over), and a bone's speed (at its middle) and
spin are the change between the two, when they are a moment apart (at most
half a second: animation LOD poses far ones seldom) and fresh (worked out
within a quarter second: one long still starts at rest), with no cut
since and the animation not paused. A bone moving faster than 20 m/s or
30 radians a second between the two is a pop, not motion (a loop whose
end does not meet its start): the whole character starts as one then (a
smaller pop still passes for motion: two poses cannot tell). Every
`play_animation` and `blend_animations` call is a cut, so one driven by
such calls each frame starts at rest. Not for a mirrored character. Limits:
let go standing straight on level ground it can balance on its
shins; joints give some degrees under hard landings.

Shots and blasts drop characters (C8b part 2; `connect_blows`). A shot
whose hit on a standing character's capsule gives it
`RagdollState::Settings::shot_drop` N s or more (`knock`) and a blast
(`impact`) that reaches its nearest capsule with `blast_drop` (`blast`)
only ask for the drop (a walker has no capsules: a shot meets its body and
pushes the bone nearest the hit, a blast reaches it by its middle): the hit is taken inside the physics' step, and
making a ragdoll (its bodies, and a copy for a character not yet animated)
is main-thread work a crowd would turn into a hitch. Those asked for wait
in a list (1024 at most, oldest first) and `before_step` makes
`starts_per_step` of them (default 4), each pushed by what dropped it:
a shot's impulse on the struck joint's bone where it struck, a blast's
shared among the bones by mass (one asked for twice is made once and
pushed by both). A blast also throws the ragdolls already lying, bone by
bone from its point; no blast throws a body faster than 12 m/s at its
heart, less toward its edge (a blast's impulse is sized for masonry).
The list is first come, first made, with no entry merged: after a blast
in a crowd a later shot's drop waits behind the blast's; a blast passes
through walls; it looks at every skinned character's capsules (no
cull: a blast is rare); one that cannot be made (a file with no capsules
fitted, the 256 reached) costs its turn again each time it is struck. A ragdoll's hit capsules are
forgotten as it starts (`HitTargets::forget`), so a bullet in flight meets
its bones only. Those waiting are stamped with the scene and the run they were asked in
and forgotten when the simulation stops, runs anew or the scene is another; one gone or hidden before its turn
stays as it was. A weaker shot does nothing to a standing character yet
(staggering under the animation is left until a game wants it).

Saved games (C8c part 3). `RagdollState::lies` gives how one lies: its
bones' joints, each bone's body (place, turn, velocities) and how long it
has lain still; a game save keeps that for every ragdoll in a `ragdolls`
section beside the walkers' (and for those of a load still waiting).
Loading lets all go, and once the save's simulation runs again each is
made anew by `RagdollState::resume`: a ragdoll started at rest as the
character is drawn at that moment (`start`), then every bone put where
the save had it (`sim::PhysicsWorld::place_ragdoll`, told it was made
this moment so that what sleeps where the character stood, another
already lying there, is not woken: asleep when it slept, so its time
lying still counts on and one about to get up still does; else moving on
as it moved, a save mid-fall falling on). Four are made a frame
(`GameSaves::ragdolls_per_frame`), each once its entity has been in the
scene a frame, the bodies round it are made (waited for 120 frames at
most), the ground under its hips is in and its file is read
(`RagdollState::ready`, 600 frames more at most); it is asked once.
Skipped: one moved or scaled in the world since, hidden, walking or limp
already, or whose file now makes other bones than those saved (the
joints are compared). Once a body or a ragdoll of the load has been
skipped, what another slept on may not be there: those made after are
made awake, to settle on what is (their time lying starts again). In the
save each bone is a row of numbers (its joint, place and turn; velocity
and spin too for one awake). Limits: joints without a bone of their own
(fingers, toes) ride as the character is posed when made again, not as
they were; the joints' anchors and limits are those of the new start, so
on a rig whose clips move joints apart a sleeper can twitch when woken;
drops asked for and not yet made (a blast's waiting list) are not saved;
the section's text is made on the frame, as the bodies' and walkers' is;
one skipped before those lying on it were made is covered, one skipped
after is not (the one on it sleeps on in the air until touched).

The characters gate (C9, `tools/characters_gate.py`) flies the N3 flight
over that valley with a crowd in its town: the gates' person
(`tools/gate_person.py`, a skinned glTF written from nothing, about 44
thousand triangles, with idle, walk and run clips) as walkers in the
alleys, each wearing hair, motion matched and kept walking (each in its
turn sent to the other end of its way), people standing round the town,
and a few by the street seen close, each one entity whose skin, clothes
and eyes are the three parts of the person's file, with a look apiece
(`parts`). A blast and shots in the street
drop walkers, who lie and get up. It holds the flight to 60 fps without
micro stutter and the characters' own passes ("skin", "character occlusion",
"subsurface scattering", "hair", "hair shadows") to 2.0 ms half a metre
from a face, and checks that all animate, walkers play their walk and
those standing their idle, that from up the river the crowd (drawn with it less drawn
with it hidden) takes under a fiftieth of its triangles and is posed
less than every frame, that
none struck down was left waiting to be made a ragdoll and at least one
got up, and that skin, eyes,
occlusion and hair were there. Off a real GPU the crowd is smaller (the
null device clears every target on the CPU). It does not hold the draw
count still: skin's and hair's passes come and go with what is in view.

Getting up (C8c part 2; `RagdollState::get_up`, the `get_up` command, or
by itself after `Settings::get_up_seconds` at rest, two a step). Only an
entity that can walk rises (body none): it has no body of its own to be
left behind where the scene has it. A ray down from its hips (from 0.3 m
over them, 3 m long, fixed bodies and the ground only: not another lying
under it; else 0.1 m under the hips) finds what its feet come on, it is
handed to the walkers standing there (`WalkerState::resume`, arrived),
facing the way its hips do, or toward its feet lying on its back, its head
on its front, and only then is its ragdoll removed: one the walkers refuse
lies on (and, rising by itself, is asked again after as long). Its pose
as it lay is carried into the space it is drawn in standing and handed to
`AnimationState::release_pose` with a fade: each joint, in its parent's
space, goes from how it lay to what its clips or its matcher give, eased
over `Settings::get_up_fade` (0.8 s), in the pose job, with nothing
allocated after the first time. So what shows does not jump as the ragdoll
is let go, and it ends standing as its animation has it. Limits: there are
no get-up clips, so limbs take the short way and may pass through the
ground; one with nothing to play stands at once at rest; an entity with a
body lies until `stop_ragdoll`; nothing looks for headroom (under a low
fixed roof it stands up through it); one at rest on a crate or a moving
platform comes to stand on the fixed ground under that, if within the
ray; one told to get up in the air has its feet put on the ground under
it.

## The player (N4 part 2)

`PhysicsWorld` also holds characters (`add_character`, `CharacterDesc`):
Jolt's `CharacterVirtual`, a capsule that is moved rather than pushed
about. Each fixed step, before the bodies, a character's velocity comes
from its input (a wanted speed over the ground, reached within a set
acceleration on the ground, slowly in the air, plus a jump asked for in
the last fifth of a second); then it is swept through the world, sliding
along walls, walking up steps, sticking to the floor going down them and
sliding off slopes over its limit. Jolt keeps the speed it was asked for,
not the one it got, so the character keeps both: how it really moved (a
wall stops it) and its own speed up or down (so a step up is never taken
for a leap). A kinematic body inside it lets rays and falling things meet
it. `PhysicsWorld::clear` keeps characters, made again where they were.

Play mode in the editor (`editor/src/play_state.h`) puts one on what is
under the camera, turns simulating on and puts the camera at its eyes,
drawn between the last two fixed steps (`PhysicsWorld::step_fraction`) so
it moves smoothly at 144 Hz as at 60, and eased up steps; the window's
relative mouse mode gives the look, and the panels get no mouse and only
key releases meanwhile. Starting never flushes the bodies (a stall in a
world just opened): it queues a frame's share and holds the player at the
camera until no entity waits and the ground under it is in
(`ScenePhysics::entities_waiting`, `ground_waiting`; five seconds at most),
then stands it on what is under the camera. It also holds it
(`hold_character`) while the ground tile under it is still being made
(`ScenePhysics::ground_waiting`), so streaming in never drops it through.
Only the camera is drawn between steps: bodies are drawn where the last
step left them, so a platform it rides can lag the view by up to a step
(16 ms) until poses are blended too.
Like simulating it is view state: stopping removes the character and puts
the camera and simulating back as they were.

### Climbing and being carried (N4 part 9)

A character told it may climb (`CharacterInput::climb`, on for the player)
takes hold of what it walks into when a ray from its chest finds a surface
too steep to walk, facing it, within a third of a metre, and not a dynamic
body (a crate is pushed, not climbed). On the wall its step is its own,
before the walking step: its input turns into the wall's frame (into the
wall is up, away is down, across is along, at `climb_speed`), it is held
5 cm off the surface (re-found by a ray each step, so it follows round
corners of a convex wall and a moving wall's surface speed), and swept
with no gravity. It lets go when the ray finds nothing to hold (past an
edge, or what it held came loose and is dynamic now), when climbing down
reaches the ground, when told not to climb, and when a jump pushes it off
(away and up; no hold again for 0.4 s); a jump asked for as it meets a
wall is a plain jump, not a push off. At the top (the chest ray hits, one
over the head does not) a ray down beyond the edge looks for ground it can
stand on: if its shape (a little smaller) fits risen beside the wall,
halfway over and there, it mantles, up for 0.27 s and then over the edge
for 0.18 s, moved directly. On a ledge that moves (not static) the path is
kept in the ledge's frame, so it lands where the ledge is now. Climbing
speed, the wall's surface speed included, is held to the characters' cap.

Any body that is not a character's can carry one (`carry_character`): the
character's place in the body's frame is kept, and after each step it is
put there, its speed the body's speed at that point (so a body put
somewhere at once, as `set_entity` does, carries it there without
flinging it on when let go), and what it touches is found again there (so
held up in the air it is not on the ground). Its input is left aside. `release_character`,
`set_character_position`, the body removed or the world cleared let it
go, and it falls on from there. The editor's `carry_player` carries the
player by an entity's body; an entity whose body is made again (its
shape or kind changed) lets it go. It is the start of a giant's hand
picking the player up: the hand will be a kinematic body moved with the
animation.

## Breakable buildings (N4 part 4)

A kit marked `breakable` (with a `strength` in N·s, default 1500) is still
one fixed compound body while whole, but its contacts are reported piece by
piece: `PhysicsWorld` keeps a contact listener that, for each new contact,
records the two bodies, the parts struck and the impulse (closing speed at
the point over the two bodies' inverse effective masses there: inverse mass
plus the turning share, (r × n) · I⁻¹ (r × n)), keeping hits of at least `min_hit_impulse` (50
N·s). Manifold reduction is off for those bodies (`BodyDesc::report_parts`)
so the part struck is the one named.

`ScenePhysics` makes a support graph for a placement the first time it is
struck: pieces whose bounds lie within 2 cm of each other touch, and those
within 5 cm of the kit's lowest bottom stand on the ground. After each step
(`break_from_hits`) a piece hit harder than the kit's strength comes loose;
then a breadth-first walk from the ground pieces finds what still stands,
and every piece not reached comes loose too (a roof whose walls are gone).
The standing pieces are rebuilt as one fixed compound; each loose piece
becomes a dynamic body of its own, up to 2 cm smaller across than drawn (so neighbours
don't wedge it) with a small tipping spin (so a wall falls rather than
balancing). A building's own falling pieces never break the rest of it, so
it does not shake itself apart. `impact` is the other way in: a blow at a
point over a radius, for kinematic movers (a giant's feet) that make no
contacts with fixed bodies.

As with all simulating, it is view state: loose pieces are drawn from
`piece_poses`, the scene is untouched, stopping (`reset_poses`) mends every
building, and a real edit of a placement (moving, resizing, deleting, a
kit change) mends it too, while a colour change keeps the break.

Known limits: `impact` walks every entity (fine for a town; a cell index
later); loose pieces stay in their kit's render cell however far they roll;
mesh and decal pieces never come loose (only cubes, spheres and planes
collide, and no sphere flattened under a centimetre); the part index assumes every piece built a shape; damage is not
saved (N4 part 8); pieces break whole, never into smaller bits.

### Falling trees (N4 part 5)

A scatter item can be a tree (`ScatterItem::tree`: trunk radius and height
at scale 1, and a strength, N·s, default 8000, grown with the scale
squared). `sim::Forest` holds the trees of the scatter cells near the
camera: the editor's `PhysicsState::feed_forest` hands it each cell's tree
points whenever the placer swaps in a new placement
(`ScatterPlacer::Cell::serial`), at most 32768 points looked at a frame (or
one cell), nearest cells first. Within `radius` (384 m, plus a cell) every
tree stands as a fixed capsule from half a radius above its foot (past what
is sunk) to its top, at most 65536 of them (a nearer cell takes them from
the farthest); past a cell more the standing trunks go, and a cell without
fallen trees is forgotten. Trunks are made and removed 1024 a sync at most
(about a millisecond; rule 9): the bodies of trees let go (far, evicted, a
cell placed again or gone) wait in a queue for that budget.

After each step `Forest::after_step` reads the same contact hits as the
breakable kits: a standing trunk struck at least its strength is removed and
made again as a dynamic capsule (wood, 700 kg a cubic metre) turning about
its foot away from the blow, faster for a harder one (0.4 to 1.5 rad/s).
Falling, it is an ordinary body: a tree it strikes hard enough falls in
turn, and a breakable kit's pieces it strikes come loose (`ScenePhysics`
sees the hit too). A hit's impulse counts the struck point's effective
mass (`PhysicsWorld` hits: mass and inertia), so a falling trunk's tip
strikes far softer than its whole mass, and the default strength keeps a
tree of the same size from felling its neighbour: no chain through a whole
forest. `impact` reaches trees as well (the distance is to the trunk's
segment, foot to top). At most 64 trees start falling a step, the hardest
struck first, 512 move at once and 16384 lie fallen; blows past that are
shrugged off (counted). A fallen tree at rest sleeps (costing the steps
nothing) and wakes, falling on, when what it rests on goes (a tree it leant
on felled, a building's piece come loose).

Fallen trees are view state like loose pieces: `Forest::fallen()` gives,
per cell, the placement serial and the fallen points' foot transforms (the
body's pose with the point's scale). `emit_scatter` draws a cell's fallen
trees after all its other points, so while the same trees are falling only
their instances are made again, in place (`rebuild_fallen`, telling the
renderer only that range changed, as a ground-only change does); a tree
starting to fall or standing up again rebuilds the cell once, at most 8
cells a frame (`SceneFrame::fallen_rebuilds_per_frame`; the rest wait for
the next frames, drawn as they were). A stale list
(another serial) is never drawn. Stopping simulating stands every tree up
again; a cell placed again does so for its trees; a new scene forgets the
forest's ids as the scene's bodies clear the world whole
(`ScenePhysics::clear(others)`).

Known limits: a tree falls whole from its foot (no stump, no splintering;
the destruction rings are N4 part 6), and the foot can slide a little as it
tips since nothing pins it; fallen trees are not saved (N4 part 8) and
stand again when their cell is placed again (a ground edit, the camera
leaving the placer's radius); trunks stand only near the camera, so a tree
farther than about 450 m never falls; the player's and a giant's kinematic
bodies make no contacts with fixed trunks (as with buildings: `impact` is
the way in).

### Destruction near and far (N4 part 6)

What breaks costs by distance from the camera, in rings:

- Near (within `ScenePhysicsSettings::simulate_radius`, 600 m, and at most
  `max_loose`, 4096, loose pieces as bodies at once): a loose piece falls
  as its own body (part 4), a felled tree as a dynamic trunk (part 5).
- Mid (past that, or past the cap): a loose piece comes down at once as
  rubble with no body (`ScenePhysics::rubble_at`): thrown along its push as
  far as half a second carries it (10 m at most), laid with its thinnest
  side down, turned about the up axis by a hash of its placement and index,
  on the first fixed body a ray down finds that is not its own building
  (what is falling, or may yet fall, would leave it in the air); at most
  `rubble_rays` (4096) rays a blow, past them, or when nothing is found,
  at its building's foot. A piece the physics world refuses a body comes
  down as rubble too. A tree whose
  trunk has no body (beyond about 450 m, or still waiting for it) struck
  by `impact` is laid down at once (`Forest::lay`): tipped about its foot
  away from the blow until its top meets the first fixed body a ray down
  finds there that is not a standing trunk (45 to 108 degrees over), at most `lays_per_impact`
  (2048) a blow, hardest first. The editor gives the forest the scatter
  cells the blow reaches first (`PhysicsState::impact`, nearest first, at
  most 65536 points), and keeps a far cell only while trees in it lie.
- Far: the voxel world follows the instances, so rubble and fallen trees
  show in the far field as they are drawn (nothing new).

Dust (`sim::Dust`): a collapse throws up a puff at the foot of what came
down (sized to it, 2 to 60 m), a felled tree one at its foot, a falling
tree landing hard (a quarter of its strength) one where it strikes, and a
laid tree one along its middle. A puff grows to three times its size over
its life (8 to 30 s, by size), rises 0.4 m/s, drifts with 60% of the wind
and thins (its dust across it kept, faded over the last 40% of its life).
Bounded: 512 puffs (a new one takes the faintest's place), 64 emits
between steps, and an emit inside a puff younger than a second swells it
instead (a street falling is one cloud). The renderer draws the 32 nearest
(`FrameInput::dust`, `render::DustPuff`): in the resolve pass each puff's
density (1 − r²/R²)² is integrated along the pixel's ray exactly, clipped
to what the pixel shows, lit by the sun (a mildly forward phase, doubled
for light scattered inside the cloud) and the sky and ground, over the
fog and clouds (over the flat background, without the sky, exposed and
tone mapped as it is not), thinning the screen reflection's weight alike.
A puff emitted into a young one swells it with its growth carried on, not
compounded. No history
(rule 2). Dust is view state: stopping or a new scene clears it.

Known limits: rubble and laid trees have no bodies (the player walks
through them, and they stay as laid when what is under them goes); rubble
lies flat to world up (half sunk on a slope); a far cell placed again (a
ground edit) stands its laid trees up, as near ones do; dust
ignores shadows (a puff in a building's shade is lit as in the sun) and is
drawn over the fog rather than in it; pieces still break whole (no
splintering); craters and footprints are part 7.

### Craters and footprints (N4 part 7)

`world::Dent` is a dent pressed into the terrain: a smooth bowl,
`-depth (1 - e²)²` for the elliptical distance e under 1, with a rim
`depth × rim × (1 - t²)²` out to 1.5 (`apply_dent`, `dent_offset`); a
footprint is the same stretched along a yaw. While simulating,
`PhysicsState::dent` (the `dent_ground` command) presses it into the
scene's terrain itself, so everything that follows the ground follows
with its own budgets: the terrain meshes, the physics tiles (a resting body
wakes as its tile is swapped), the voxel world, scatter (re-placed onto the
new ground). It is still view state: `world::GroundDamage` keeps each
dented tile's heights and version as they were before its first dent, and
stopping puts them back with `Terrain::restore` (the same versions, so
nothing reads as edited or unsaved and every cache stays good). Every
terrain command, every undo and redo of a scene change, a new scene's
copy for its undo, `save_world`, `save_scene` and `open_world`'s unsaved
check run on the ground as authored (`PhysicsState::GroundAsSaved`: the
dents lifted, then put back with their own versions, so nothing that
follows the ground sees a change); a tile such an edit changes loses its
dents, so a snapshot never holds one and nothing brings one back for good.
`simulate off` puts the ground back at once. A dented cell reads as
unsaved meanwhile, so it never unloads.

A cell placed again keeps its fallen trees that stand where they stood
(`Forest::set_cell`: the same x and z to the centimetre, size and model,
whatever their point numbers now: a dent adds or takes other layers'
points). A dent re-places the scatter of the cells it touches; the editor
gives the forest those cells as soon as the placer swaps them in
(`PhysicsState::refresh_forest`, the same frame, so felled trees are
never drawn standing), wherever they are, and the trees felled there stay
down (their bodies wake and settle into the dent; laid ones move with the
ground under their foot).

Cost: a dent walks the samples in its own turned bounds once (radius times
stretch is at most 200 m: about 1.4 million samples at the finest
spacing, a one-off), and keeps a copy of only the tiles it changes.

Known limits: rubble (no body) stays at the height it was laid; stopping
puts every dented tile back in one frame; damage is kept only by
keep_damage (part 8).

### Kept damage (N4 part 8)

Simulating is view state; `keep_damage` makes what it broke part of the
world. `world::CellDamage` (damage.h) is a cell's kept damage, beside what
was authored there: `KeptKit` records (a breakable kit placement's id,
kit and transform as they were, and each piece down with its world
transform) by the cell of the placement, and `KeptTree` records (a scatter
tree's foot x and z and size, and its lying transform) by the cell of the
foot. Craters are not records: `GroundDamage::keep` leaves the dented
tiles as they are, now the ground as authored (the command records their
heights and versions before and after, so undo puts each back exactly).

`Scene::set_damage(cell, damage)` holds it (copied with the scene, saved in
scene files as `damage`, in cell files as their `damage` key) and marks the
placements its records name as changed, so `ScenePhysics` re-syncs them: a
placement whose record still matches it (`kept_of`: same id, kit and
transform, a breakable kit) is made with those pieces down where the
record says, with no bodies (`Broken::Piece::kept`), and the rest standing
as one body (`standing_body`, shared with `loosen`). That holds whether
simulating or not; `reset_poses` leaves a placement broken only as kept,
and remakes one broken further back to its kept state. A real edit (moved,
resized, another kit) no longer matches: it stands whole, and the next
`keep_damage` drops the record. `Forest::set_cell` takes the cell's tree
records: the tree standing at the same x and z (to the centimetre), as
big, lies where its record says, with no body, and stays down through
`reset`; one fallen there now gives its place to the record. The editor's
`PhysicsState::follow_kept` gives the forest a cell it knows again at
once whenever the scene's kept trees there differ from what it was given
(`damage_edits` says when to look); a cell it does not know yet (streamed
in, wherever it is) waits until the placer has placed it and is given
within the feed's budget. Kept trees never count against `max_fallen`, and
a record that finds no tree now (a placement cut short) is kept for when
it comes back.

An undo or redo sets a cell's damage with `hold_empty`: an empty entry
held in the scene says "none here", so for a cell streamed out since, the
scene's side wins over the file's when it comes back, as an entity put back
by an undo does. Two cells may name one placement (a stale record kept
before it moved): `kept_kit(id, &placed)` finds the one made where it is.

Streaming treats kept damage as it does the terrain tile: read with the
cell (`load_cell`'s damage) and set into the scene on insert (one the
scene already holds, put back by an undo, is newer), compared with what
the file holds for unsaved and unload (`same_damage`: a cell with unsaved
damage never unloads), taken out of the scene on unload, and written with
the cell (a cell with only damage keeps its file; cleared, the file goes).

Rotations read back exactly when already unit (normalized again they would
move by a rounding, and a record would no longer be what was kept).

Cost: applying a record is the same work as making the placement (its
pieces counted against the sync budget). `keep_damage` walks the broken
placements and the forest's fallen trees once; follow_kept walks the
forest's cells and the scene's damaged cells only when the damage changed.

Known limits: `clear_damage` leaves kept craters (they are ground); a
record's placement deleted keeps its record (harmless: it applies to
nothing) until `clear_damage`; a kept piece has no body (the player walks
through rubble, as through far rubble); a kit's pieces changed since make
the record's piece indices the new kit's.

## Animation (N5 part 1)

`anim` (core only) holds skeletons and clips as plain data and the work
done on them each frame, so the importer, the editor and later the
simulation share one definition and none of it needs a GPU.

- **Skeleton.** Joints with names, parents (always before their children,
  so one pass from the start works out every joint), a rest pose as
  translation, rotation and scale, and inverse bind matrices; `root`
  places the root joints' parent. Model space is the file's own space.
- **Clips.** Per joint channels of translation, rotation or scale, each
  linear (rotations along the shorter arc at constant speed), step or
  cubic (glTF's Hermite spline, tangents scaled by the key spacing).
  `sample` starts from the rest pose and replaces what the clip animates,
  holding each channel's first and last keys outside them.
- **Skinning.** `model_matrices` then `skin_matrices` (model times inverse
  bind) per joint; a vertex is the weighted sum of its (at most four)
  joints' skin matrices applied to its bind-space place, weights in 255ths
  summing to 255. `skin_point` is the CPU reference the GPU pass will
  match. `pose_bounds` bounds a pose from the joints alone: each joint's
  vertices lie within the reach the import measured from it when bound,
  scaled by its skin matrix, so no vertex is visited per frame.
- **Player.** `AnimationPlayer` plays one clip (looping or once, at a
  speed, paused), cross-fades to the next over a time, or holds two clips
  blended (a walk and a run), the second one's phase following the
  first's when asked. It owns nothing but the clip numbers and times, and
  `PoseScratch` keeps evaluation free of allocation once grown.
- **Motion matching's database** (`motion.h`, N5 C7a).
  `build_motion_database` samples a character's chosen clips 30 times a
  second and keeps 27 numbers per frame: both feet's places and
  velocities, the hips' velocity, and where the character will be and
  face a third, two thirds and a whole second on. All are in the
  character's space: its root is the hips' place on the ground and their
  facing, averaged over a quarter second either side (hips sway with each
  step), so clips must carry root motion. Looping clips go round (the next
  round moved on and turned by what one round does, so a walk in a circle
  loops too); one-off clips cannot be jumped into within their last second.
  Each group of numbers is divided by how much it spreads and multiplied by
  its weight. `motion_search` returns the nearest frame that may be jumped
  into, skipping whole runs of 64 and 16 frames whose least and most
  already lie further off than the best found; it allocates nothing.
  Measured: 180,000 frames (100 minutes of clips) searched in about
  0.26 ms, built in 0.1 s; building belongs on a job. `find_motion_joints`
  finds the hips and feet by the usual rig names, `clip_loops` tells a
  loop from a one-off clip. A search takes how much the pose counts
  beside the way ahead (0 for a first frame, when no pose shows yet).
- **The matcher** (`matcher.h`, C7b). `MotionMatcher` keeps one frame
  playing, looks every tenth of a second for a better one (at once when
  asked, or when a one-off clip has ended, which is never stayed on) and
  jumps to it unless it is within a few frames in the same clip or less
  than a twentieth better. A jump never shows and two clips are never
  mixed: the difference between the pose left and the pose landed on, and
  between how fast each was changing, is kept per joint as an offset that
  a critically damped spring takes to nothing (halving in a tenth of a
  second). The pose it gives stands in place (`motion_pose`: the
  database's root taken out of every root joint), so whatever moves the
  character owns where it is; `root_motion` says how the clip would have
  moved it. `motion_ahead` eases a way ahead from the velocity now to the
  one wanted. With few clips, none recorded going from one to the next,
  being posed alike outweighs going the wanted way and a walk never
  becomes a run, so the matcher counts the pose half (`pose_weight`).
  Each group of a frame's numbers is scaled by its spread over the set,
  but the way ahead's places by no more than 0.35 m and the hips'
  velocity by no more than 0.4 m/s (what a set that stands and walks
  spreads): a run beside the walk would else make a walk's worth of way
  ahead count for little, and one sent walking stayed in its idle, whose
  pose is far from any stride's (found with the gates' person: idle,
  walk 1.4, run 4). With no slower clip than its walk a character
  slides in its idle until it nears the walk's speed (those clips: from
  about three quarters of it; without the run, from 0.85); it changes between walk and run with a wide band
  between (about 3.1 m/s up, 2.3 down for those clips). The editor
  eases the velocity it gives `motion_ahead` toward the goal's (half
  the gap in 0.2 s of posed time) while the goal itself is what is
  wanted: a goal that jitters does not flap between clips, and one that
  drops to nothing at once still plays a stop clip.
  The editor's use of it is under "Playing them" below.

The glTF importer reads one skin per file (a second is refused). Its
skeleton is the skin's joints plus any node between two of them (their
transforms count, as glTF says), ordered parents first; JOINTS/WEIGHTS
sets 0 and 1 are merged and cut to the four heaviest. Skinned meshes are
kept in bind space, as written (their node's transform is ignored, as
glTF asks); meshes that are not skinned but hang under a joint move
rigidly with it, others with the first root joint. `ImportResult::mesh` is
the character as its rest pose has it, so everything that draws meshes
unanimated (kits, scatter, the bind-pose preview) keeps working.
Validation refuses joint indices out of range, mismatched inverse binds,
key times that do not increase, values that do not match their keys,
clips over an hour, more than 1024 joints and more than 8 million keys.

Drawing them (`render`). A skinned mesh is built by
`geometry::build_skinned_clusters`: its original triangles alone, one
level, every group terminal (a pose moves the vertices, so errors measured
at bind would not hold), vertices merged only where their weights match
too. Asked for `levels` (N5 C1a), it builds the whole hierarchy instead,
errors and bounds in bind space: simplification only drops vertices, so
each level's vertices keep their own weights, and weight seams (corners at
one place with other weights) are protected from simplification, so no
coarse triangle takes a corner from across one (the sloppy fallback, which
ignores protection, is off for them). Where a depth's tree root
(`nodes[depth]`, every group of the depth and its worst error) projects
under the cut's threshold, nothing of that depth is drawn, so it needs no
pose (bounds widened for the pose must widen the roots alike; a depth
holding a terminal group has an infinite root error, so the renderer keeps
its own per-depth bounds without them, below).
Each character adds its own (`Renderer::add_skinned_mesh`: every
instance of one shows its one pose; its clusters must come group by group,
depth by depth, as the builder makes them), and it goes through the same
cluster path as everything else: its groups stream into the geometry pool
as any mesh's do (terminal groups first and kept, finer ones loaded when
asked for and evicted when unused), and a source buffer keeps its vertices
at bind with their joints and weights, cluster by cluster in the pool's
order. Copies of one character share its geometry
(`Renderer::add_skinned_copy`, C1c-1): the CPU clusters, the streamer's
laid-out groups (`GeometryStreamer::add_copy`, a shared `Layout`), the
groups, LOD tree and skin source on the GPU and the per-file skin data
(`SkinShape`); each copy has its own pose, refit cluster bounds, residency,
floor and place in the pool, so a crowd of one character costs its
geometry once (its levels, about twice the original triangles alone). The
original may go before its copies (the shared buffers go with the last).
The editor sends a skinned file's geometry with its import
(`MeshLibrary::Mesh::skin_sources`: one skinned mesh for the file or for
each of its parts, never drawn) and every animated entity's is a copy of
that, so the file's geometry does not go up in a frame (a copy makes
only its own cluster and residency buffers, and its groups stream in as
any mesh's): before, the first of a
character to be animated uploaded it then, 12 ms of the frame for a
43,760 triangle person in three parts (the living-world gate's uneven
frames on the owner's PC, each time the first of a look came near).
Where the renderer refused the source, the first animated copy is
uploaded as before and the rest copied from it. A source is kept for
as long as its file is loaded, animated or not: its buffers, its
coarsest groups in the geometry pool, one pose (at bind) and a place in
the renderer's list of skinned meshes (`skinned_meshes` counts it).
`set_pose` takes one skin matrix per joint and a sphere around the pose;
in the frame after, before anything reads geometry, `skin.slang` (a
thread group per cluster) writes the posed vertices and normals into the
mesh's place in the pool and refits each cluster's culling sphere to
them, so culling, raster, shadows and shading read posed geometry with no
change of their own. It runs for a new pose, and when groups start to show
(loaded, or the floor below lowered), not every frame; an instance whose
shown groups were not all posed (out of transient memory) is not drawn
that frame. Instance culling uses the pose's sphere.

Levels of detail for skinned meshes (C1b). The LOD spheres and errors stay
at bind. `set_pose` bounds how far the pose moves any vertex from bind
(`skin_lod.h` `pose_reach`: per joint, from the bind sphere around the
vertices it moves, |(A - I) c + t| + |A - I|_F r; a vertex blends joints
with weights summing to one, so it moves no further than its farthest
joint alone), and every LOD sphere, GPU (`MeshEntry::pose_reach`,
`projected_error`) and CPU (`LodView::pose_reach`), widens by it. The same
widening everywhere keeps the cut whole and monotonic; it covers where
things went, not how a pose bends them (errors stay the bind pose's, so a
stretched joint or a coarse triangle across a bent joint can look a little
past the threshold). The CPU cut (GPU culling off) tests the pose's sphere,
then takes the cut by the widened spheres, frustum off.
Only depths some view draws are posed. `GeometryStreamer` keeps a floor
per posed mesh: groups finer than it (terminal ones excepted) are hidden,
written "not loaded" in the residency table culling and `skin.slang` read
(`shown()`; parents are always one depth coarser, so what shows stays
closed under parents and the cut whole), so the pose pass skips them and
coarser clusters stand in, as for any group not loaded. The pose pass
dispatches only the clusters from the floor's depth on, plus runs of
terminal clusters finer than it. The camera aims the floor
(`Renderer::aim_skin_floors`, before the streamer's update: per mesh, the
nearest instance in view, the first depth whose bounds over its
non-terminal groups project over the threshold; none in view: the
coarsest). A request for a hidden group, or a usage mark on a loaded hidden
one (shadow culls ask for no loads but mark what they reach), lowers it at
the next update; it rises to the aim once no group below was used or asked
for in `k_keep_frames` (checked every `k_usage_interval` updates). A
character coming close draws coarser for a frame or two, as streaming
does. Shadow pages under a skinned mesh's instances are dirtied when its
pose or place changes. Skinned meshes are never left to the far field, and
the voxel world does not hold them (they bounce no light yet: a known gap,
until a coarse pose is voxelized). The editor imports skinned files with
their levels (cloth sheets stay one level). Morph targets (C3a) come in
with a skinned file (`SkinnedTriangleMesh::morphs`, sparse deltas per
vertex, gltf.h) and are carried through the cluster build to its vertices:
vertices merge only where their deltas match too, and morph seams (a
lip's corners, one per side at one place) are protected like weight seams
at every level. The renderer (C3b) keeps a morphed mesh's moves on the
GPU, shared by its copies: per skin source vertex a range (8 bytes) of
entries (20 bytes: the position's move in floats, the normal's in three
signed 10-bit numbers over -2..2, the target); `set_pose` takes a weight
per target (0, the base shape, until then; callers apply the rest
weights), uploaded after the joint matrices when any is not 0, and
the pose pass adds the weighted moves to the bind position and normal
before skinning. The pose's bounds and LOD reach widen by the
most the morphs can move a vertex (skin_lod.h `morph_reach`: their
weighted largest moves times the joints' largest stretch).

Skin (C4a). A material with a skin look (`Material::skin_look` into
`FrameInput::skin_looks`, packed in `gpu::Material::flags` as
`k_material_skin` and the look's index + 1 in bits 8 to 15) is shaded with
a two-lobe highlight (GGX at the roughness times 0.75 and 1.3, the second
15 %) and its diffuse light spreads under the surface. With
`RenderSettings::subsurface_scattering` on, resolve's `ps_spread` /
`ps_direct_spread` leave the skin's diffuse light (sun, sky, bounce, lamps;
times the look's strength, past the fog) out of the radiance and write it
to a target of its own; `subsurface.slang` then blurs it across and down
the screen (17 taps of the separable kernel from d'Eon's six-Gaussian skin
profile, `src/subsurface.h`, scaled by the look's scatter in metres at the
pixel's distance, at most 64 pixels; a tap off the skin or further in depth
than the scatter takes the middle's light) and adds it to the radiance into
that target, which stands for the radiance from there (screen reflections,
water, post). Its two full-size rgba16f targets are made on the first frame
whose skin spreads (the setting on, a lit view) and freed after 600 frames
without. Timed as
"subsurface scattering"; it runs only on frames with a skin material whose
look spreads (strength over 0). Approximations: the spread light is dimmed
by the fog's average over the channels, and falling rain or snow drawn over
skin does not dim it.

Skin through thin parts and detail normals (C4b). Light also passes
through skin where little of it lies between the point and a light (Jimenez
et al. 2010): the thickness is how far the point lies behind the nearest
surface the light's shadow depths hold, looked up a texel and a half into
the surface (an edge's taps land on it), taken at the point itself with
the push's sideways part put back (else slanted thin skin reads as none)
(`sun_thickness` in `shadow_pages.slang` on the finest drawn level its
pixel needs, the previous view's page standing in as for shadows;
`light_thickness` in `lights.slang` for lamps with a shadow tile). It dims each channel by the six Gaussians
of the same profile (`skin_transmittance`, red furthest; the look's
scatter scales the skin to it, as it scales the spread), times the
diffuse colour, the light, `saturate(0.3 - n.l)` (on through the shade side
a little past the terminator) and the look's `transmission`; the sun's is
dimmed by the clouds. It counts as diffuse light, so it spreads with the
rest. Unknown (nothing drawn, nothing held, or what is held lies more
than two texels beyond the point: lit straight on), nothing passes;
lamps without shadows pass none. Approximations: the sun's depths are
32-bit over 40 km, so thickness is known to a millimetre or two (lamps'
much finer; at the default scatter a step is some 1.6 mm of the profile,
so light through a slowly thickening part can band), and to a texel's
slope across the nearer surface (8 mm texels on the finest level). A look's `detail_normal` (linear, e.g. BC5)
is sampled at the uvs times `detail_tiling` and bends the shading normal
in the tangent frame turned to it, over the material's own normal map
(skin pixels rebuild uvs and tangents even without material textures);
streaming's feedback asks for it the tiling's levels finer than the
material's maps.

Eyes (C5a). A material with an eye look (`render::EyeLook`,
`FrameInput::eye_looks`, `Material::eye_look`; an eye is never skin) is a
mesh with uvs whose iris is centred at uv (0.5, 0.5): uv-based, so it
holds under any transform and on skinned meshes. `resolve.slang`'s
`eye_surface` bends the view ray at the cornea (index 1.376, its normal
leaning out toward the rim by `bulge`) and carries it down to the iris
plane, `iris_depth` (at most 0.45 of the iris radius: deeper would fold
the iris over itself at its near rim) below at the centre and none at the rim; the
material's textures are sampled where it lands, so the iris and pupil
shift under the cornea as the eye turns. `eye_colors` then draws the iris
and pupil (unless `painted`: the base colour texture holds them), the
limbal ring and the lids' shade toward the texture's edge, and makes the
whole eye wet: non-metal at `cornea_roughness`, lit by the cornea's
normal. Looks reach the GPU as `gpu::EyeLook` (48 bytes each, cleaned by
`to_gpu`), uploaded only on frames with eye materials; the material's
flags carry `k_material_eye` and the look's index in the bits skin uses.
Along v the frame is minus the normal maps' bitangent (which runs toward
smaller v), so the shift is the eye's whatever way its uvs lie or its
instance is turned. The cornea's turn over a pixel is added to
specular anti-aliasing's variance, so a small far eye's highlight widens
to its pixel. The iris is lit as the cornea is (no caustic inside it yet),
the normal map is read at the unshifted uv, and the parallax takes the uvs
to run evenly along the tangent and bitangent.

Eyes in worlds and the editor (C5b). `world::Material::eye`
(`world::EyeLook`, optional) is saved with entity, kit piece and scatter
item materials as an `eye` object (fields left out on load take their
defaults; values out of range, or skin on the same material, refuse the
file). The scene frame keeps an eye look beside each eye material
(`SceneFrame::eye_looks`, shared by eyes alike whatever their whites, at
most `render::k_max_eye_looks`: later ones draw plain until a look frees,
as skin's do), and a material with an eye is never given a skin look. Over
MCP `eye` sits beside `skin` on entities and scatter items
(`editor/src/material_maps.h`): setting one takes the other off.

Skin in worlds and the editor (C4c). `world::Material::skin`
(`world::SkinLook`, optional: none is not skin) is saved with entity, kit
piece and scatter item materials as a `skin` object (its numbers all written, the
detail map and its green-down flag only when set;
fields left out on load take their defaults; values out of range refuse
the file). The scene frame keeps a skin look beside each skin material in
its table (`SceneFrame::skin_looks`, shared by materials whose skin is
alike, whatever their colours; an unused material is not skin for the
renderer, so a character gone stops skin's pass; past
`render::k_max_skin_looks` later skin draws as plain, and the table is
made again once an unused material holds a look) and asks the texture library for
the detail map as a normal map (only on things with uvs), its renderer id
filled in when ready. MCP: `skin` on `create_entity`/`set_entity` and
scatter items (`skin_schema` in `editor/src/material_maps.h`),
`set_render_settings`' `subsurface_scattering` and
`last_frame.subsurface_scattering`; the panel has its toggle. The MCP
smoke turns it off outside `gi_gate` with the other full-size passes.

Playing them (editor). `MeshLibrary` keeps a skinned file's skeleton,
clips and bind-space clusters beside its rest-pose mesh, which every
entity drawing it shows until it animates. An entity animates when it
draws a skinned file and has something to play: its saved
`Entity::animation` (clip, speed, loop; `set_animation`, undoable, saved
in scene and cell files) or a clip a command plays in the view
(`play_animation`, `blend_animations`: view state, until `set_animation`,
the entity hidden or drawing another file, or a new scene). `AnimationState` follows the scene's
changes (its own copy of them, kept by the scene frame beside the
physics', and what the scene has not handed out yet just before the scene
frame is built, so no cell is built with a copy it should not draw), gives each animated entity its own copy of the character in the
renderer (a few a frame, so loading many never stalls a frame), moves
their `AnimationPlayer`s on by the frame's time (its own `animation`
phase, after physics; a clip started this frame starts at its start),
works out the poses on two workers (`parallel_for`) and hands the renderer
only poses that differ from the last one sent (a same pose would redraw
its shadows for nothing). The
scene frame draws an entity's copy instead of the file's mesh once it is
loaded and posed. At most 4096 animate at once: those animating keep their
place, a free one goes to the lowest id waiting, and the rest stand frozen
in the rest pose. Crowds are posed by size on screen (animation LOD, C1c-2):
time moves on for every character every frame, but the pose is worked out
every frame only for one whose pose sphere's radius is at least 100 pixels
on screen, and down to four times a second for one under 5 pixels; those posed less than
every frame share a budget of 256 poses a frame (at least 64 of them however
many are posed every frame), most overdue first, their phases spread by id so
a crowd does not pose all on one frame. While time stands still (paused, a
menu open) what moved since its last pose catches up within the budget. A pose a
command or a (re)start asks for always goes at once.

Morph weights play as the pose does (C3c). A clip's channel can animate a
morph target's weight (`anim::Path::morph`, the channel's `joint` naming
the target; glTF's `weights` channels are split per target and point at
the merged morphs). `AnimationPlayer::evaluate_morphs` starts from the rest
weights (`set_morph_rest`), sets what the main clip animates and blends
toward the other clip, or toward the weights caught when a fade began, by
the pose's own share, so a face fades and blends with the body. The editor
works the weights out with the pose, lays the ones `set_morphs` holds over
them (view state, as `play_animation`'s, let go when the saved animation
changes) and hands them to `set_pose` beside the joints.

Matched entities (C7c). `EntityAnimation::matched` (saved; `set_animation`)
names clips of the file the entity is motion matched among (`"*"` alone:
all of them) instead of playing one. `AnimationState` keeps one
`MotionDatabase` per file and list of clips, shared by every entity of
that file and list and let go with the last: built once (joints by
`find_motion_joints`, each clip a loop or not by `clip_loops`) on a job
system of its own with one worker, because the main thread helps the pose
workers' pool while it waits on it and would run a build itself. Until it
is built, and when the file cannot be matched (the joints not found, none
of the clips there, no frame to start from: logged once, reported by
`get_animation`), the saved `clip` plays as ever; one that cannot be
matched and has no such clip animates no more (its place goes to
another), and its database is kept until a new scene so it is not built
again each time it is asked for. A build not begun when its last entity
lets go is skipped. Once it is built each entity's `MotionMatcher` poses
it (they come to it 16 a frame, `matches_per_frame`: a first pose and
search each, which a crowd must not do on one frame): `advance` only adds
the frame's time up, and the pose job moves the matcher on by that sum
(a second at most) and searches, so animation LOD spaces a far crowd's
searches out and they run on the pose workers. The way ahead is the
entity's goal held from now on (`set_goal`, each frame, by whoever moves
it; none that frame: standing), not eased from how its clip moves: a
walker's body does not wait for its clip, the clips catch up with it.
The editor's main loop sets a walking walker's goal from the velocity it
is steered at, seen from where it faces (`goal_of`); a walker turns at
once to where it goes, so its goal has no turn yet and turning clips wait
for a mover that turns over time. A command's play takes over from the
matcher and `set_animation` hands it back; both are cuts (nothing fades
between the matcher and a clip player), as is the change from the saved
clip to the matcher when the database arrives. Morph weights rest while
matching (the matched clips' morph channels do not play; `set_morphs`
holds do). `speed` and `loop` are the saved clip's only. Nothing of the
matcher is saved: a loaded game picks a first frame again.

### Hair (C6)

Hair is strands, not cards (plan: C6, five parts). C6a is the data.
`geometry::Groom` (`giant/geometry/groom.h`) holds every strand as
`k_strand_points` (16) points, root first, in the space of what the hair
hangs on at rest, with the scalp's normal at the root and a seed for the
look. The first `guide_count` strands are the guides, the few (a few percent)
that are simulated; every strand follows its three nearest guides by
inverse-square weights (`StrandFollow`), and `pose_strand` moves each of
its points as the weighted guides' points moved from rest. Strands after
the guides come in an order whose every run from the first covers the scalp, so
drawing the first N thins the hair evenly (LOD by strand count).

`generate_groom(scalp, params)` grows a groom from a mesh, so nobody
models strands: roots spread over the triangles by area with a
low-discrepancy sequence (that is what makes prefixes even), each strand
leaving along the normal and bending down point by point (`droop`), with a
helix (`curl_radius`, `curl_turns`), a wander (`frizz`) and tips drawn in
to the nearest guide (`clump`). Guides are picked far apart from one
another among the first strands and moved to the front. Values out of
range are clamped; a scalp without area grows nothing.
`groom_from_strands` builds the same from lines made elsewhere (a file's
curves), resampled evenly along their length and shuffled.
Such curves come from a glTF file's line primitives
(`ImportResult::curves`) or from an Alembic groom (`alembic.h`,
`read_alembic_curves`): a small reader of our own for the Ogawa kind of
.abc file, with no Alembic library. It reads every shown curves object's
first sample, placed by the transforms above it, leaves out the curves
the file marks as guides (`groom_guide`, the Unreal groom convention: the
engine picks its own) and keeps the file's units and axes, which
`to_engine_space` turns (`FileAxes`: y up, z up, or Unreal's left-handed
z up; 0.01 for centimetres). Widths, colours, root uvs and animation are
not read yet. The file is never trusted: every offset and count is
checked against its size, and the work is bounded.
`groom_problem` says what is wrong with a groom from outside. All of it
is deterministic and CPU only; the simulation, drawing, shadows and the
editor's side follow in C6b to C6e.

The guides are simulated by `sim::HairSim` (`giant/sim/hair.h`, C6b), one
per groom, from the guides' rest points alone (the sim module does not
know grooms). Position-based, as the cloth is. Each fixed step a point
keeps its speed less `damping` (1/s, measured against its root's move, so
hair on a running head is not held back), falls, is dragged toward the
wind's speed (the share grows with how fast the air passes, capped so it
never overshoots), and is pulled back toward where it rests in the style
by a spring of `root_stiffness` Hz by the root down to `tip_stiffness` Hz
at the tip, taken implicitly (curls stay curls, long hair still swings;
finer steps sag a little less and swing longer, so a style is tuned at
the step it runs at). Then each piece is set to exactly its length from
root to tip, so hair does not stretch (sharing a piece's error between
its ends first, as cloth does, was measured to make hanging hair wriggle
and was left out). In that pass the point above gives up speed for the
move (Mueller's follow-the-leader
correction): without it a strand let go whips up higher than it began,
with all of it hanging hair wriggles for ever once a step's fall is large
against a piece's length, so the share is `1 - 0.7 cbrt(fall / piece
length)`, measured to do neither for even and uneven pieces of 3 mm to
6 cm at steps of 1/30 to 1/240 s (the range `step_seconds` takes). In a storm loose hair flutters, as hair does.

Capsules (`HairFrame::capsules`, at most 16, known by their place in the
list, taken to ride the head between frames; each guide tests only those
within its reach) are kept out in the strict pass: a point is put out by
the radius and the hair's `thickness`, held from sliding by `friction`
(by how hard it was pressed in), and its piece set to its length again, a
few times over, ending on the length. So pieces keep their length and
may lie a little into the stand-off; a point still pressed after that is
put out of the capsule itself, which is the one case where a piece
stretches (a capsule driven into hair faster than it can get out of the
way; a capsule faster than its own width a step can pass through). A
point is
never kept further out than it rested when the hair was placed, so a
head's capsule does not lift hair off its own scalp while a shoulder
brought against hanging hair still pushes it aside; the price is that a
limb crossing the rest shape at the moment of placing is let through
there until the hair is placed again, so hair is placed with the
character at rest.

Points live in the world's directions about the place the hair hangs
from (`HairFrame::position`, rotation, uniform scale), so moving the head
swings the hair and floats stay exact far from the origin; `points()`
gives them in the groom's space, ready for `pose_strand` and the
renderer. The hair is simulated up to the last whole step before now and
rides the head from there; each step sees the head where it was at that
step's time (between the last frame's place and this one's). So a frame
shorter than a step changes nothing, and frames of any length up to
`max_steps` steps neither stretch the hair nor show it a head faster or
slower than it is (time past that is dropped: the hair moves in slow
motion on such a frame). A new scale resizes the hair as it hangs. A
jump (over 5 m plus 100 m/s for the frame's time) or numbers gone wrong
carry the hair whole at rest (`HairStats::resets`). Moves
under a hundredth of a millimetre are not shown, so hair at rest keeps
its `version()`. Work is bounded: steps of `step_seconds` (1/240 to 1/30
s), at most `max_steps` a frame, nothing allocated after construction.
Not saved (view state, as cloth).

The renderer draws it (C6c, `src/hair.h`, `shaders/hair.slang`).
`Renderer::add_groom` packs a groom (16 bytes a strand for its guides,
weights and seed; 8 bytes a point, 16 bits an axis across the groom's
bounds) and sends it to the GPU over the next frames under
`groom_bytes_per_frame` (4 MiB); `groom_ready` says when it draws. A frame
names what hangs where in `FrameInput::hair` (`HairDraw`: the groom, a
place, turn and uniform size, the guides as `sim::HairSim::points()` gives
them or none for rest, a `HairLook`, a cap on strands). The nearest 256 in
view are drawn, in the lit view, after opaque shading and before water and
particles, timed "hair" (their strands are posed, and their shadow tiles
drawn, before shading: see below).

Strands are far thinner than a pixel, so hair is a matter of coverage, and
one surface a pixel (the visibility buffer) cannot hold it without history.
It has passes of its own, with nothing kept from frame to frame:

1. A compute pass poses each drawn strand: its 16 points moved as its
   guides moved from rest and put in the world. A second lights it, once:
   the sun through Marschner's three lobes in Karis's fitted form (R off
   the surface, TT through the strand, TRT the coloured highlight) plus a
   scattered term, shadowed by the scene's sun shadow at three places along
   the strand and by its own groom's hair at four (below); the lamps listed
   where it is; the sky's or the voxel world's light once; and the air in
   front over it. The result is 16 bytes a point (place, light as 9 bits a
   channel with a shared exponent). No later pass lights anything.
2. Ribbons facing the camera are made from the vertex id alone (no vertex
   buffer), half a pixel wider each side than the strand; each pixel takes
   the share of itself the strand's width covers, so a strand a tenth of a
   pixel wide is a smooth line a tenth as opaque. They are drawn twice:
   into a depth target of hair's own (the nearest hair), then into
   weighted sums (the blend mode particles use) where a pixel's weight
   falls off by e for each 3 mm (or two pixels' width there, if more)
   behind the nearest hair. So the colour is
   the outer layer's, averaged where strands share a pixel, and what shows
   through is the exact product of every layer, in any order.
3. The sums go over the light, and where hair covers more than half a
   pixel its depth is written into the scene's, so water, particles and
   post treat it as a surface.

LOD is by strand count (`hair_lod`): a groom draws as many strands as make
each about 1 / `hair_detail` (a render setting, default 3) of a pixel wide,
the first of the groom, each widened by the share left out, so it covers
the same; at least 64. A strand's 15 pieces are joined to 5, 3 or 1 while a
piece is under 3 pixels. A frame draws at most 2^18 strands (past it every
groom is thinned alike). `HairLook`: `melanin` and `redness` give the
colour (`hair_color`: white through blond and brown to black, ginger with
redness; `tint` for dye), `roughness`, `width` and `tip_width`,
`variation` by the strand's seed. Stats: `hair_grooms`, `hair_strands`,
`hair_segments`, `hair_left_out`, `hair_draws`.

Hair shades itself from the sun (C6d, `hair_shadows`, a render setting) by
deep opacity maps (Yuksel and Keyser). After the strands are posed each
drawn groom is drawn as the sun sees it into a tile of its own (128 x 128
of a 2048 x 2048 pair of targets, 48 MiB, made with the first groom added):
the tile spans the ball round the groom's hair as posed. One pass keeps
the depth nearest the sun at each texel, the next adds up (an additive
blend) how much hair lies within 2 %, 6 % and 18 % of the ball's width
behind that, and in all. A point's shade is read between those sums by
its own depth (four texels, blended), so it is none at the front (a
strand never shades itself; a point within a texel's depth of the front
counts as at it, so a sheet of hair struck at a slant does not shade
itself either) and deepens smoothly inwards. Past the third depth the
shade runs up to all the hair in the texel by half the ball's width, so
the middle of a very deep groom is shaded a little by hair behind it, as
is hair lying far behind a stray strand that is the front of its texel. The three lobes lose `exp(-hair before)`; the scattered term
loses less the lighter the hair (light hair passes light on), which is
what makes blond hair glow where black hair is dark. At most 4096 strands
of a groom go into its tile (its first, in pieces of three points or more,
each standing for the rest). Lamps (the first 32 listed in the strand's
cluster, with each lamp's own shadow looked up once at the strand's
middle) light it through the same lobes at six places along the strand;
they have no map of the hair, so against a lamp hair shades itself as a
ball round the groom's middle would (the side turned away dark).

Hair shades the world the same way (C6d part 2, the same setting). The
strands are posed and the tiles drawn before shading (`prepare_hair`,
timed "hair shadows"; the lighting and ribbons come after shading, timed
"hair"), and shading multiplies the sun on each pixel by `hair_shade`
(`shading_extras.slang`): for each of the 16 nearest drawn grooms whose
ball's column, as the sun sees it, holds the pixel, the hair before it by
that groom's tile, as `exp(-hair)`. So a fringe shades the face under it
softly and by how thick it is, a head of hair lays a hair-shaped shadow on
the shoulders and the ground (fading out over 40 times the ball's width
behind it), and nothing is drawn into the sun's shadow
pages (which would have to be redrawn whenever hair stirs).

Limits: one groom's hair does not shade another's; hair further than the
16 nearest grooms casts nothing on the world, nor does any hair on water,
particles, the far field or the voxel light; hair lying closer to the skin than a texel's depth (4 mm on a head 0.5 m
across: stubble, brows) casts nothing on it; hair out of view casts
nothing either, so a shadow on the ground goes when its head leaves the
view; the sun through skin (ears, the thin back-lit shine) is not shaded
by hair; only the sun has deep
shadows of hair;
the sky's light reaches inner hair as it does outer; a tile's texel is
4 mm on a head of hair 0.5 m across, so shade is soft at that scale; a
strand blown past the ball round its guides takes the shade at the tile's
edge; against a lamp a flat sheet of hair darkens away from the point
nearest the lamp; a strand's lamps are those listed at its middle; the
shadow passes are bounded but not free (at most 2^18 strands in pieces of
five, twice, and both targets cleared whole each frame), and are not yet
in a gate;
inner layers
take the outer layer's colour where they show through (a stray strand
in front of a lock of another colour tints its pixel); hair thinner than
half a pixel's cover leaves no depth, so water and particles behind it
draw over it; falling rain and snow are not drawn over hair (fog,
clouds and dust are); hair is in no reflection; a groom has one place (one
joint). At most 4 MiB of guides are taken as simulated a frame (the
nearest grooms'); the rest draw at rest.

In a world (C6e) hair is `world::Hair` on an entity: whether it is on, how
it grows (`geometry::GroomParams`' fields and `top`, the share of the
shape's height that grows hair), its look (`render::HairLook`'s) and its
motion (`moves`, `stiffness`, `damping`), saved with the entity only when
set and kept while off. The editor's `HairState` (`hair_state.h`) follows
the scene: for each visible cube or sphere wearing hair it grows a groom on
a worker from the top of the shape as the entity scales it (so scale
stretches the head, not the strands; the shape is cut at the height `top`
gives and its normals are the sized shape's), packs it there too
(`Renderer::prepare_groom`: packing 200,000 strands takes tens of
milliseconds), hands it to the renderer, and lists it in
`FrameInput::hair` at the entity's place and turn. A change to what it
grows from grows it again: one job an entity at a time, the old groom
shown and moved until the new one is grown and the renderer has all of it
(`groom_ready`), a result no longer wanted thrown away (hair changed every
frame keeps its old groom until the changes stop); look and motion change
at once, without putting the hair back at rest (`HairSim::set_motion`).
The nearest within 30 m with `moves` on are simulated under the weather's
wind (`sim::HairSim`), up to 16 of them and 4096 guides in all, stepped
side by side on two workers of their own; one already moving keeps its
place to 33 m and until another is clearly nearer, and at most two start
moving in an update, and at most two grown grooms are handed to the
renderer in one (each is a GPU buffer made; the first hair ever also makes
the renderer's shared hair buffers, once). The rest hang as grown. Entities wear hair lowest ids
first, up to 256 and 2,000,000 strands in all; the rest are left out.
Closing the editor does not grow what was still waiting. `get_hair` reads
it. View state: not saved, not undoable.

On a character (a mesh entity whose file has a skeleton) hair grows from
the skin: `hair.joint` names a joint (none named: the first with "head" in
its name), and the scalp is the original triangles the joint and the
joints under it move by half their weight or more, as they stand at rest
in model space (so y is up and sizes are true whatever way the file's bind
space or the joint's own axes turn, or a node above the skeleton scales),
about the joint's place, cut at `top`'s share of their height and sized by
the entity's largest scale. The groom is drawn at the joint as the body
is drawn (`HairState::place`, each frame before the draws are read: the
animation's pose once its copy shows, else the rest pose, under the
entity's drawn transform), turned as the joint has turned since rest. A
joint's own scale in a clip does not resize it, a mirrored entity turns
its hair the wrong way and a non-uniformly scaled one wears hair of its
largest scale. Hair answers its head a frame late (it steps after the
frame is drawn). While a groom is grown again for another joint the old
one stays on the joint it was grown round.
The body's 16 capsules nearest the joint at rest (`sim::HairCapsule`,
posed each frame) push the hair aside. Every entity's hair is drawn where
its entity is drawn: carried by the physics, a walker or a swing it goes
along (the same order as the scene frame). A mesh entity whose file has no
skeleton, or no such joint, grows none and `get_hair` says why
(`problem`: file not loaded, no skeleton, no such joint); it is looked up
again whenever the mesh library changes. Everything the joint's skin holds
above the cut grows hair, eyes and all: name a joint whose skin is the
scalp, or lower `top`.
In the editor the "+ Hair" button adds a head of hair (a sized ball), and
an entity's settings have "Wears hair" with its growth, look and motion,
a character's joint by name, and what the hair is doing (strands, growing,
moving, or why none grew); each change is a `set_entity`, so it undoes.
`tools/mcp_smoke.py` grows hair on a ball.

A file may bring its own hair: the glTF importer keeps line primitives as
curves (`ImportResult::curves`: a strip or loop is one curve, a list's
segments are joined end to start, or where they meet; where they stand at
rest in the file's space, a skinned mesh's own lines skinned to the rest
pose; none of no length; the first 262,144 and 16 million points), `MeshLibrary::Mesh::curves`
shares them, and an entity with `hair.from_file` wears them instead of
grown hair: `hair_strands` takes up to `strands` of them evenly, about the
joint's place at rest and by the entity's largest scale on a character
(the space the scalp is in), else in the entity's space by its scale, and
`geometry::groom_from_strands` makes the groom on the worker (`guides` and
`seed` choose the simulated ones; the growth settings are unused). A file
still needs triangles to be imported, and curves are root first as the
file orders them. Limits: mesh files without a skeleton grow no hair of
their own (they can wear their file's), and kits made from entities do not
carry hair.

Or hair wears a groom file (`hair.file`: an Alembic `.abc`, read by
`geometry::read_alembic_curves_file`), which wins over `from_file` and
grown hair on any entity that may wear hair. `HairState` reads a file on
its growing workers, never on the main thread (an 84 MB groom of 65,000
strands takes a few seconds from disk), once for all the hair that wears
it, and lets it go some 600 updates after the last hair stopped wearing
it (hair hidden and shown again, or put back by an undo, reads nothing
again), or at once when hair waits for room; its curves are freed on a
worker too, wherever the last of them is let go. The curves are kept in the file's own space: the file says neither
its units nor its axes, so each hair gives them (`file_scale`,
`file_axes`) and `hair_strands` turns and sizes the strands it picks on
the grow worker, which makes trying another setting a regrow and not
another reading. Until the file is read the hair is `reading`: nothing new
is grown or shown and what it had still shows. At most
`k_max_groom_files` (8) different files are held (those that could not be
read hold nothing and take no room); hair past them is `reading` too, and
`waiting`, until there is room. The curves then go the way a mesh file's
do: a groom is modelled round the head it is for, so on a character they
are taken as in its model space, about the joint the hair rides; on a
shape, in the entity's space. A file that cannot be read is the hair's
`problem`, and is tried again once the setting changes or nothing wears
it. A file changed on disk is read again only once its old reading has
been let go.

## Ballistics (W1a)

`sim::Ballistics` flies bullets through the physics world as point masses:
fixed steps of 1/240 s (the same flight at any frame rate), quadratic drag
(exact over a step for drag alone) then gravity, and one ray along each
step's chord. What a ray meets is a `world::Surface` (on `Material`: an
entity's, a kit piece's; the editor calls trees wood, characters flesh and
the ground's tiles dirt), and each surface has a resistance (joules a metre),
a ricochet angle and the share of speed a ricochet keeps (text-tunable,
`set_ballistics`). Under the ricochet angle it is mirrored off the surface;
otherwise its energy says how far it could go in (`reach = energy /
resistance`, at most 4 m), and a ray cast back at the same body's same part
(`RayFilter::body`, `part`: a Jolt shape filter on the compound's children)
from that far in finds the far side: found, it comes out there with the
energy left and meets what is next (another part of the same building is a
layer of its own, with its own surface); not found (it would end inside the
part, which a convex shape reports as a hit at once, or the terrain's height
field, which has no far side) it stops. A far side as near as the entry is
the entry seen from behind and does not count. A ray that starts inside
something (met at distance 0, or on a face it is leaving) never ricochets.
One step's ray goes through at most 8 surfaces, at most 4096 bullets fly and
a step() call casts about 16384 rays at most (fixed steps past that wait).
`target_of` names what each ray met as it happens (the entity and kit
piece), so hits applied after a piece is shot out still know what they
struck. Every meeting is a
`BulletHit` (speeds in and out, energy left there, the blow given), which
the editor turns into effects: dynamic bodies pushed by the momentum lost
(`PhysicsWorld::add_impulse`), and a breakable kit's piece worn
(`ScenePhysics::strike`: `blow_per_joule` of the energy, adding up to the
kit's strength, then shot out with what it held up; a frame's hits loosen
each placement once). `ScenePhysics::owner_of`
names the entity and kit piece behind a body and its part (`RayHit::part`).
Bullets fly with the simulation (after its step, so they meet the bodies
where they are now) and are cleared when it stops.

Characters (W1b) are met by their bones, not a body in the physics world:
at import `anim::fit_capsules` fits a capsule round the vertices each joint
moves most (in the joint's space: along the way to its children, or to its
vertices for an end joint; from the 2nd to the 98th percentile along it, as
thick as 70% of them lie from it). Each frame after the animation,
`editor::HitTargets` poses the capsules of every visible skinned entity
whose pose or placement changed (the animation's model matrices, or the rest
pose; `AnimationState::skinned` and `pose_serial` say what changed) into a
`sim::HitShapes`, a plain list beside the physics world (remaking Jolt
bodies every frame would cost far more). Bullets test it along each step
with an analytic ray-capsule test (the union of two balls and a cylinder:
in where it first enters one, out where it last leaves one) behind a
bounding sphere per character, and the nearer of it and the physics ray is
met. A character is one solid: its capsules overlap at the joints, so the
way out is where the ray leaves the last capsule it is still inside of (one
hit, named by the capsule it went in by). Only what changed is posed again
(nothing skinned changed: `HitTargets::update` returns at once), and an
animated entity is met in its pose once its copy is drawn (the rest pose
until then, as shown). Decor (`body` none) is not hit. Known limit:
`HitShapes::raycast` looks at every character's bounding sphere per ray (a
grid comes with crowds). Zones are named by rules on bone names
(`sim::HitZoneMap`, per file). Bullets meet the pose of the frame before
(the animation runs after the physics step).

## Gameplay rules (G1 part 1)

`gameplay` holds the rules of the game being built (FOLD, see
`docs/game-plan.md`): what items there are and what happens to them. It is
plain C++ data and functions with no GPU state, tested in ctest; the
engine decides how things move, render and get hit, `gameplay` what the
numbers are and what they mean.

- **Items** (`items.h`). An `ItemCatalog` holds every kind of item once
  (`ItemDef`: a stable id, a name, a kind, weight, base value, how many
  stack, whether it is worn, that is, has a condition from 0 to 1). It is
  loaded from text files (`"format": "giant-items"`), several into one
  catalog, all or nothing per file; an unknown field or a key given twice
  is an error, so a typo never passes as a default. Item indices follow the
  order of loading, so the game loads its files sorted by name; files and
  saves use ids. Numbers are written as the shortest decimal that reads back
  the same (0.73, not 0.7300000190734863), so files stay diffable. The game's files are `game/fold/data/*.items.json`
  and the tests load every one.
- **Inventories** (`inventory.h`). A backpack, a body, a crate or a stash
  is a list of stacks: an item, a count, a condition. Stackable items fill
  their stacks up to `max_stack`; worn items never stack, each keeps its own
  condition. Each stack has an id unique in its inventory and never reused
  (saves keep the next id too), so the UI and later rules can point at one
  stack. `set_condition` is the hook for repairs and wear. `remove`
  takes the worst condition first; `move` carries a stack's condition
  across. Weight is no hard limit, as in GAMMA: `burden` says whether the
  load is light, heavy or overloaded, and the player controller will slow
  down by it. Saved as text (`"format": "giant-inventory"`), items by id.

## Wind (E1 part 1)

`giant/core/wind.h` is the wind and how things move in it, shared by the
renderer, the editor and (later) the simulation. `world::ground_wind`
takes it from the weather: half the clouds' wind (the ground slows it),
gustiness 0.2 plus a fiftieth of the clouds' speed (a storm gusts at 0.8).
`WindClock` moves it on each frame (the editor's `WeatherState::wind`,
handed over as `FrameInput::wind`; without a weather, the default
weather's breeze): the gust pattern drifts at 0.8 of the wind and the sway
and flutter clocks run faster as it blows harder. Gusts are a sum of three
waves over a 64 m tile; the clocks wrap every 256 turns, and every
frequency used is a whole number of turns in that (rocking rates in
sixteenths), so nothing jumps when they wrap and floats keep their
precision for ever. The shader works camera-relative with the camera's
place modulo 64 m (`FrameConstants::wind_camera`); every phase is a whole
number of waves per 64 m, so the pattern at a place is the same from any
origin.

A thing sways when its material has `sway` or `flutter` (0 to 4 each,
`Material` in world and render). Its origin is its foot; `MeshEntry::top`
(the mesh's highest vertex) times its scale is its height. A point at
height fraction h moves along the wind by `height * h^2 * lean` (the lean
grows with the gusted speed and its square, rocks about itself at a rate
that falls with height, and is held to 0.3), a little across it, and drops
so it bends rather than stretches; `flutter` adds a small fast shake that
grows from the trunk outwards (leaves at the tips). `sway_offset` (C++) and
`wind_offset` (common.slang) are the same function, line for line;
`max_sway_offset`/`wind_reach` bound it. raster.slang moves each vertex as
it draws it and resolve.slang moves the three corners it finds the hit on
the same way, so the visibility buffer and shading agree; normals stay as
they rest (the bend is gentle). Culling (instances, clusters, the retest,
and the CPU's cut) grows each swaying instance's spheres by its reach.
Skinned meshes never sway (their `top` is 0).

Sway fades out over the last third of `RenderSettings::wind_distance`
(150 m), so the far field and the voxel world, which hold things at rest,
match what stands still out there, and cached shadow pages far off stay
valid. Near the camera, the renderer gathers swaying instances into 32 m
squares each frame (`sway_shadows`) and `ShadowPages::sway` marks the
pages under them on the levels whose texels are finer than the motion;
`plan` draws up to `wind_shadow_pages_per_frame` (32) of those again a
frame, longest drawn first, besides the usual budget, so the shadows sway
too, at a lower rate when many pages do. Known limits: that gathering
walks the frame's instances (a byte per material says which sway, so still
ones cost a load and a test); a kit's pieces each sway about their own
origin, so a tree should be one mesh with its foot at its origin.

## Cloth (E1 part 2)

`world::Shape::cloth` hangs a sheet from its entity's origin, in the
entity's x-y plane (`ClothSettings`: `width` across, `height` down, `pin`
top edge, top corners or left edge, `weight` per square metre,
`stiffness`); kit pieces and scatter items cannot be cloth, and the
entity's scale is unused. `giant/sim/cloth.h` moves it: a grid of points
about 0.15 m apart (3 to 32 a side, so at most 1024: the skinned meshes'
joint limit) joined by springs along the grid, across each square and over
two points (softer as `stiffness` falls), with position-based dynamics:
a Verlet step under gravity and the wind, then the springs' lengths put
back 6 times. Each sheet is simulated in its own space (wind and gravity
turned into it), so moving its entity moves it whole. The wind is
`wind_at` where it hangs (the gusts the trees show); it pushes each point
along its normal with the relative wind's normal part squared (a flat
plate), a little along its face, and in ripples that run away from what
holds it on the flutter clock, so a storm throws it far harder and faster
than a breeze and heavier cloth moves less. No point moves more than half
the grid's spacing a step; after the springs, tethers keep each free
point within 1.05 times its rest distance of the pins holding it (its
column's top, its row's pole, or both corners) and a last pass shortens
any spring over 1.1 times its length, so a 60 m/s storm stretches a 4 by
6 m sheet at most about 1.4 times between neighbours (blowing along its
face; about 1.1 face-on). A sheet whose numbers stop being finite is put
back at rest (`resets`).

Work is bounded (rule 9): the nearest `max_simulated` (32) sheets within
`distance` (60 m) of the camera move, at most `max_points` (8192) points
between them (the nearest always), in fixed 1/60 s steps, at most 4 a
frame; the rest hold their last pose. A sheet that has moved less than
0.02 mm a step for a second with no wind sleeps until wind, a push or its
entity moves it. 32 banners 1 by 2 m (112 points each) cost about 1 ms of
one core a frame; the editor spreads sheets over three workers.

The editor's `ClothState` follows the scene's changes before the scene
frame is built (as the animation does), makes each sheet its own skinned
mesh (`cloth_mesh`: the grid's points at rest, front and back faces, each
vertex weighted wholly to its point, so the joints are the points; at most
256, a few a frame) and, after the weather, steps the sheets and hands
the renderer the pose of each that moved: per point, a matrix from where
it rests to where it is, turned to its patch's frame (along the grid and
its normal). The renderer draws it as any skinned mesh: posed on the GPU,
culling spheres refit, shadows drawn again where it moved; it is never
left to the far field and the voxel world does not hold it. A cloth entity
draws nothing until its mesh is loaded; one given a new size (a drag in
the panel) keeps its old mesh, still, until the new one is loaded and
swapped in before the next cells are built, and a mesh the renderer
refuses is tried again only when its entity changes. `get_cloth` and
`push_cloth` follow the scene's latest edits first. `Cloths::push` (the `push_cloth`
command) gives every point near a place a change of speed, less towards
the edge; `Cloths::blast` throws the points away from a place, the blow's
impulse over the sheet's weight (at most 30 m/s): the `impact` command
does that (E1 part 3, below). Known limits: cloth passes through what it
hangs against (no collision yet), bullets pass through it untouched, and a
far sheet holds still rather than waving.

## Swinging things (E1 part 3)

Lanterns, signs, bells, lamps on chains: any cube, sphere, plane, mesh or
kit entity with `SwingSettings::on` hangs from a pivot `length` metres
straight above its origin and swings about it (`hinge` free, any way; or
`x`, only about its own x axis made level: a sign on two hooks), at most
`limit` degrees from hanging straight, weighing `weight` kg. Saved with
the entity (only when set); kit pieces and scatter items do not swing
(the kit entity swings whole).

`giant/sim/swing.h` moves them: each is a pendulum, its origin on a rod
from the pivot. Each fixed step (1/60 s, at most 4 a frame) gravity pulls
it, the wind where it hangs (`wind_at`, so the gusts the trees show)
pushes on its box as it is turned now (`size`: the area it shows the
relative wind, a box's drag, growing with the speed squared, never past
the wind's own speed in a step: a light thing in a storm stays stable),
its pivot slows it, then the rod puts it back (its length, the hinge's
plane, the limit) and its speed is what the rod let it do. So a storm
leans a lantern far over and throws it about in the gusts, a breeze barely
stirs it, and a sign swings only across its face. Shots (`knock`: the
bullet's momentum lost in it, over its weight, at most 20 m/s), pushes and
blasts (away from the blast, the impulse over its weight) change its
speed. Work is bounded (rule 9): the nearest `max_simulated` (512) within
`distance` (100 m) move; the rest hold their pose. One that moved under
0.02 mm a step for a second sleeps until the wind about it changes (by
0.05 m/s) or something knocks it, so steady wind costs nothing.

They have no meshes of their own: each is drawn turned about its pivot.
The editor's `SwingState` follows the scene's changes (as the cloth
does; the box is the entity's scale for primitives, its pieces' bounds
for a kit, its mesh's size once loaded), steps after the weather and
lists those whose pose moved more than 0.1 mm. `scene_frame` takes those
(and the physics' moved bodies) as `moved`: each cell keeps what each
entity drew (`Slot`: its instances in the cell), so only the moved
entities' instances are written again in place and the renderer is told
of that range alone (`CellRange::prev_version`, joined with the cell's
ground change in place that frame, if any; decals moved with them), and
the instance store uploads just that range at the cell's kept anchor; a
cell with anything else changed, or an entity drawing other than it did
(a piece's mesh not loaded), is rebuilt whole as before. The nearest four
times `max_simulated` are looked at a frame (sleepers waking), the rest
wait. Swinging entities' instances are marked
`render::Instance::moves`: the voxel world does not hold them (no brick
work as they move; they bounce no light, as skinned meshes, even far ones
holding still) and the far field never stands in for them. Their shadows move with them (the shadow
pages under moved instances are drawn again). While the physics poses an
entity (a dynamic body, simulating), its pose wins over the swing's.

Bodies moving are kept out of the voxel world the same way: a dynamic
entity whose body moved in one of the last 30 `update_poses` calls
(`ScenePhysics::moving`), and a breakable kit's loose piece likewise
(`PiecePose::moving`; rubble and kept damage lie still), are drawn with
`moves`; once still that long they are drawn again, into the voxel world
(once). So a piece drifting down a river, a crate bobbing, costs the voxel
world nothing for as long as it goes on moving, instead of its cell's
bricks being made again every frame.

`PhysicsState::on_hits` and `on_impact` (set by `connect_blows`) carry the
shots' hits and the `impact` command's blows out: a hit on a swinging
entity knocks it; a blow throws the swings and cloth in reach. The
commands `get_swing` and `push_swing` follow the scene's latest edits
first. Known limits: a swing does not collide with what it hangs by (the
wall behind a lantern), it does not spin about its rod, its physics body
(if any) stays where it hangs at rest, and chains are one rigid rod.

## Water (water plan part W1a)

Lakes, rivers and the sea are scene data (`world::Water` in scene.h, the
queries in `world/water.h`), saved with the scene and in a world's
world.json, made and changed by `create_water` / `set_water` /
`remove_water` (undoable) and listed and asked by `get_water`. A lake is a
level inside its shore (an outline, even-odd inside), a river a strip down
its path (each point's width and level, taken along it, flowing from its
first point), the sea a level everywhere: what is under the water is the
ground's (water is there only where the ground lies under the level, so a
sea's shore is where the ground rises over it). Water never moves the
ground: the bed is carved with the terrain brushes. Where bodies overlap,
the highest surface counts; a hidden body is not there at all.
`water_at(x, z)` answers what is at a point (level, a river's flow and
width), bounding-box first.

Floating and swimming (W1b): the editor's PhysicsState hands the scene's
water to `PhysicsWorld::set_water` while simulating, whenever what the
physics uses differs (kind, shown, level, flow, shore, path: compared, as
its edits come from many places, microseconds for ordinary water; a new
colour wakes nothing). The physics keeps a copy cut into parts (a lake's shore, the sea,
each river stretch), each with the ground it covers and its highest
surface, filed in a grid of 128 m squares (the sea and huge parts looked at
everywhere): a body finds the few parts under it (each river's nearest
stretch, then the highest surface, as `water_at`: past a waterfall the
upper level is not there), and one over all water costs a compare. Before each fixed step every awake dynamic body whose
bounds dip under the surface at its centre gets Jolt's buoyancy impulse:
the water it displaces under a flat surface at 1000 kg a cubic metre (so a
body lighter than that floats with that share under, a heavier one sinks,
and one of the default mass, made at water's density, hangs where it is),
quadratic drag, and a river's flow as the water's speed (logs drift down
it). Bobbing settles: a share (3 a second when half under, less for a body
heavier than water) of its speed against the water is taken off before
the buoyancy; only what floats (or nearly) is drawn towards a river's
flow, a sunk body being left to the drag (a block stays on the bed, a
round boulder rolls a little in a fast river). Water does not reach a
room: a body all under it with something fixed over it before the
surface and something fixed under it, at most 6 m apart (a tunnel or a
basement under a lake is dry; open water under a pontoon, a pier or an
overhang is not: the terrain's bed is no floor, though a fixed rock or
wreck on the bed under a deck still reads as one). Setting water wakes the sleeping bodies where it was
and where it is (lowered or removed, what floated falls).

A character with `swim_depth` (1.3 m, at most three quarters of its
height) of water over its feet swims, until 0.3 m shallower: no gravity,
through the water at its input up to `swim_speed` (1.6 m/s) relative to a
river's flow, `velocity.y` up (to where it rests, feet `swim_depth` under
the surface, no higher) or down; with none it floats up at 0.6 m/s. Up and
down change at 15 m/s^2 plus water's drag (0.5 v^2), so a fall into deep
water stops within a few metres. At the surface (not diving), swimming
into an edge out of reach of walking up (over its step height above its
feet), up to 0.4 m over the surface and shallow enough there to stand, it
pulls itself out, as onto a ledge climbed; a higher bank is climbed
(climb input, the default) or not left by, and climbing down into water
deep enough lets go. Shallower than swimming it wades, slowed from 0.3 m
deep to half speed at `swim_depth`. The same room rule keeps a character
in a tunnel under a lake dry. In play Space (held) is up and C down, given
without taking over the rest of an input set by command. The surface is
flat until waves (W2/W4) move it.

Drawing the surface (W2a): the caller hands `FrameInput::water_vertices`
(world positions, a flow, a look index), `water_indices` (triangles) and
`water_looks` (`WaterLook`: the colour deep water shows, absorption a
metre per channel, turbidity, how wavy), with `water_seconds` for the
waves' clock; `RenderSettings::water` turns it off. `pack_water`
(src/water.h) makes them camera-relative (at most 2^18 vertices and
triangles, 256 looks): a triangle with a bad index or a vertex that is not
a number is dropped (`RenderStats::water_dropped`), one past the caps left
out (`water_overflow`), a look out of range takes the default. In the lit view
only (not the light alone), after shading (and screen reflections) and before post, the pass
(water.slang, timed "water", two post draws; one when only the camera's
water is drawn, W3) copies the light into a
texture of its own and draws the water's triangles over it, each pixel
tested against the scene's depth (no depth target): the normal is a sum
of 8 travelling waves made from the wind (`water_waves`: the longest
where a sea under that wind peaks, 2 to 200 m, steeper in a storm; each
phase folded at the camera in doubles, so the waves are the same at a
place wherever the camera is, and a wave shorter than a few pixels fades
out); what lies under it is seen through, bent by the waves (straight
where the bent pixel shows something above the water), dimmed by
Beer-Lambert over the distance the view travels in the water and
replaced, as that grows, by the look's colour lit by the sky and the sun;
the sky (its ambient light round the mirror direction) and the sun's
glint (GGX) are mirrored by Fresnel's law (n 1.33). Seen from below: W3.

Lit as the scene is (W2b): the pass reads shading's own extras (the same
`gpu::ResolveExtras` upload, shading_extras.slang shared with
resolve.slang). The mirror is what the screen shows along it (screen
reflections on: the ray screen_trace.slang shares with reflect.slang), else
a cone through the lit voxel world (voxel reflections and GI on; skipped
where the screen's is sure; both skipped under thick foam), else the sky's air along it (the flat
hemisphere without the sky). The sun's glint, the moon's at night (it is
the light then), is shadowed by the sun's pages and the clouds; the lamps
in the pixel's cluster add highlights widened by their size, shadowed as
shading's are. On wavy water foam shows where the bed comes near (from
about 0.5 m straight down on the highest crests, nearly everywhere at
the very edge), lit by sun, sky and lamps (unshadowed). Then the air,
as shading puts it over the scene (aerial perspective, fog, clouds, what
falls, dust), over the water's own light; what is seen through it already
holds its own air, which is taken off before the water dims it, and the
screen's reflection is added over it as reflect.slang's is.

The editor draws its water (W2c, editor/src/water_meshes.h): each visible
body gets its own look (colour, absorption, turbidity, waves); a lake's
shore is cut into triangles at its level by ear clipping (a shore that
crosses itself is not drawn, counted in the stats panel and
`last_frame.water.broken_lakes`), a river is a strip down its path (each
point's width and level, widened at bends so each stretch keeps its
width, at most doubled; square ends, where the physics' stretches are
rounded), a sea is handed over as its level and look (the renderer
draws its rings, W5a). The pass draws over itself in order, so bodies go
lowest first (a
river by its highest point, so at a river's mouth the river shows over
the lake): where surfaces overlap the highest shows, as the physics has
it. Each body's triangles are kept and cut again only when its shape
changes (the scene's water compared each frame, as the physics' copy is;
a new colour, waves or lake level only puts the triangles together
again). Cutting a 4096-point shore takes a few to a few tens of
milliseconds (its corners that turn back filed in a grid, its edges
checked for crossing by a sweep along x, worst for a star whose every edge
spans its middle): a hitch while such a shore is reshaped, none in play.
The editor feeds the water in every view (the voxel world keeps it in
the debug views too, below); the water setting off skips the water pass
and takes the water out of the voxel world. The
waves' clock is the game's time (not a screenshot's; paused, it stands
still; it keeps up with physics stepped by a command). `set_render_settings` `water` turns it
off; `last_frame.water` counts triangles drawn, dropped and overflow.

Under the surface (W3): FrameInput::underwater names the water the
camera is over (the editor's camera_water: world::water_at at the camera,
off in a room under it, PhysicsWorld::water_reaches as the physics keeps
bodies dry there: something fixed overhead and a floor of its own within
6 m, the terrain's bed no floor, so under a pier is open water; skipped
while the camera is more than 2 m over every body's surface). Pixel by pixel, where the near plane is under its
level, the copy pass (ps_copy) dims what is seen by the water between
(Beer-Lambert, from the near plane to what is there, or to where the view
comes up through the surface) and adds the water's own light: single
scattering of its colour lit by the sun and the sky, the light dimmed by
the water above each point, so the view darkens with depth and is
lighter looking up; from above the deep water's own light is the same
model along the view bent into it, so in the sun it does not jump at the
surface (from above the sun on it is shadowed, from below not). So the screen splits where the near plane meets the
surface when the camera bobs at it, and nothing extra is drawn when no
part of the near plane is under (found on the CPU from its lowest corner:
then the copy is a plain copy, as before). The surface seen from below
lets the world above through Snell's window (the sky along the bent ray,
or what the screen shows that way if it lies above the water), and past
48.75 degrees mirrors the water under it (total internal reflection: its
own light, or what the screen shows along the mirror, a pool's floor,
through the water back), then the water between it and the camera. Caustics: the sun's ray to a
point on the bed came through the still surface at a point found by
refraction; the waves' curvature there spreads or focuses the light
(one over the determinant of how the bed point moves with the surface
point, the curvature taken three times for the finer ripples the eight
waves stand for, at most five times as bright; a crest gathers like a lens), the pattern
fading over 10 m of depth and with the waves too short for the pixel,
laid over the sun's share of the light from above (none under lamps with
the sun and the moon down) where the sun reaches (its shadows and the clouds'): from above on the bed seen through the surface, from below in
the copy pass. Approximations: the light already in the image keeps the
air shading put over it (a few metres of fog under water); the
in-scattered light is unshadowed; caustics also brighten what faces
away from the sun under water when shadows are off; no sun disc through
the window, nor its radiance gain; the sky seen dimmed under water
is exposed as drawn things are, a background's alpha set; what
lies above the window is looked up on screen as if far. `last_frame.water.underwater`
says when some of the view was under.

Running water (W4a): each vertex carries its flow (m/s), its rapids
(`WaterVertex::rapids`, 0 to 1) and its surface's slope (`slope_x`,
`slope_z`, rise a metre, at most 10, snorm16 on the GPU). The editor
gives each river stretch its own four corners and its own rapids, from
how steeply its surface falls (none under 2 cm a metre, the most from
10 cm, a waterfall's two points at one place too), and each point its
stretches' own slopes averaged, so the lighting leans smoothly where the
geometry folds (a waterfall's corners its own, the steepest: 10). Four
corners a stretch: the vertex cap (2^18) holds 65,536 stretches. The waves on running water are carried downstream as a
flow map's two phases would carry a texture: the wave sum taken where
the water was up to a second ago and a second ahead, from two starts
half of `k_water_flow_period` (2 s) apart, each faded out as it starts
again (the second shifted by a few metres so the two differ, both in one
loop over the waves), the blend's slopes and crests scaled back to one
sum's spread so it does not calm and pulse halfway, and coming in over
the first 0.1 m/s (slower, the two phases come together into still
water's waves, never seen starting again); the clock is folded on the CPU in double. Faster
water is rougher (taller, broader-glinting waves, up to 4 m/s, and
rapids more: running water ripples even with `waves` 0) and foams on its
crests from 2 m/s; rapids foam on most of their crests (white water).
Still, slow water never foams but at the shore. A sloping surface is lit
as it leans (its slope added to the waves'; the bed seen through it
bent only by the waves, not by its lean), and the caustics under it
take its roughness (not its flow). Still level water draws as before.

The sea (W4b): surf. Where water shoals over a shelving bed (any water
with waves), waves run in to the shore in bands: their crests along the
depth's contours, one every 0.8 m of depth, each coming in as the depth
it is at falls, one every `k_water_surf_period` (6 s, its clock folded on
the CPU in double), drawn as they break into foam from 1.8 m of depth (a
fifth at 1.5 m, all of it from 0.3 m, broken up by the crests), as high as
the wind makes them (`water_surf`: a calm's 0.15 laps, 1 from 8 m/s). How
much the bed shelves comes from the depth's change between neighbouring
pixels over the pixel's width on the surface (whole from 1 cm a metre,
none over a level bed: a pool, a flat-bottomed pond; fading out from 50
cm a metre, none past a metre a metre: an edge, a body's or a rock's
side, not a beach). The depth is the screen's: a floating body's or a
sunken rock's gently sloping top within 1.8 m of the surface gets surf
of its own.

The sea's swell (W5a). `world::sea_swell(wind)` gives four long sines:
three running with the wind, the longest about as long as the sea that
wind grows peaks (Pierson-Moskowitz, 40 to 250 m), one from far off
across them 1.4 times that, scaled so their significant height is the
grown sea's (0.3 m in a calm to 4 m in a gale), none steeper than k a
0.12. `world::SwellTrack` follows a changing wind without the waves
jumping (a sine's phase k x - w t moves far out and late on by metres of
wave with the smallest change of k): a set's lengths and directions are
held while the wind's stay within a tenth and 15 degrees, only their
heights following (easing, 10 s to come most of the way, so a sudden
wind steps nothing); past that a new set fades in over 20 s as the old one
fades out (eight waves while it does). A sea's height at a place is its
level plus the sum (`world::swell_at`, in double, with its slope) times
its `waves`; lakes and rivers stay flat. One set, two users. Physics
(`PhysicsWorld::set_swell(swell, seconds)` before each step, its clock
the given time less what is still to step, moved on by its own steps)
floats bodies on the risen surface and tilts the plane Jolt's buoyancy
takes by its slope, keeps floating bodies awake while they ride it (a
body sunk to the bed sleeps), wakes the water's sleepers each time the
swell has risen another quarter metre, and swimmers bob with it. The
renderer (`FrameInput::swell`) folds each wave's phase at the camera in
double (`water_swell`) and raises the vertices that ride it
(`WaterVertex::swell`, the seas' own) in the vertex shader, its slope
worked out again per pixel for the lighting. The renderer draws the seas
itself (`FrameInput::seas`: a level and a look each, at most 4): rings
round the camera made from the vertex id (no upload but the radii), fine
out to 2 km (each 1.03 times the last, 256 round: triangles about 3 % of
their distance across), then coarse to the horizon, about 113 thousand
triangles a sea; each wave fades out where the mesh cannot follow it
(under 6 triangles a wave, none under 3, and all of it from 1.5 to 2 km).
Physics keeps the whole swell everywhere, so a body far out bobs a little
against a sea drawn calmer there. The water pass has no depth, so the
rings are drawn far to near (a nearer crest hides the sea behind it),
after the water lower than the seas and before the rest
(`FrameInput::water_indices_under_seas`). A surface is shaded as seen from
below only when the eye is under it as it lies there (a crest seen from a
trough is seen from above). Under water, the line where the near plane
meets the surface follows the swell per pixel (the camera's water rides
it, `Underwater::swell`; the near plane's test takes the highest crest),
and the depth the water's glow and the caustics take is the risen
surface's. The editor's water clock is the game's: paused, the sea stands
still with what floats on it; a sea's `waves` reaches physics as soon as
it is edited. Wakes and rings round floating bodies wait
for the effects part.

Water in the voxel world (W5b). Lakes and rivers become voxels: the
renderer hands the frame's water triangles to the voxel scene
(`VoxelScene::set_water`; with the water setting off it hands none, so the
setting compares the whole picture with and without water). The scene
groups them by look into bodies (a colour and its triangles, the seas'
own vertices left out), each filed in 64 m squares (pieces: the triangles
whose ground plan meets the square, and their box within it). Whether
anything changed is `FrameInput::water_version` (the editor's WaterMeshes
counts its changes; 0: a hash of the whole, every frame, about 20 ns a
vertex); then each body's own hash says which did, and only those are
filed again (row by row, the squares under each triangle's span: a few
milliseconds on the main thread for a 4096-point lake a kilometre or two
across, while it is edited) and built again: the bricks under their old
pieces cleared, those under their new pieces built, so a brick is marked
only where water is. Building a brick adds the water's triangles over it
after its instances, gathered once from the pieces over it (the bodies as
they were then are kept by the build, so an edit elsewhere never restarts
it), a few dozen between looks at the clock (a lake of thousands of
slivers is built over frames, never in one). A brick waiting for room in the pool
comes back when there is room if water is over it, as if an instance were.

A water voxel is marked by its normal bytes both zero (`pack_water_voxel`,
`water_voxel`; an ordinary normal that would pack so is written as 255,
255, the same straight down) and reads as facing up. In a brick, water
gives way to a stand-in; it shows over the ground or anything else in the
same voxel where it covers at least half of the voxel's plan
(`voxel::k_water_share`, its clipped area), so a shore shows water and a
5 m river in a 16 m voxel of its valley shows the valley; where nothing
else is, any water covering part of it shows. Light cones and reflections that meet water from
above see it as a mirror: its diffuse light times (1 - Fresnel) plus the
sky mirrored at that angle times Fresnel (n 1.33); from below, its diffuse
light only. Water voxels block the sun and sky like anything else, so beds
and what is under water are darker in the bounce light (the water takes
the light, roughly). The water pass's own reflections pass through water
voxels (`k_no_water`: a sloping river's upstream would mirror itself). The
far field does not draw water voxels (they are not stand-ins): the water
pass draws every body at every distance.

The seas are not voxels (they reach the horizon): `Frame::sea_level` is
the highest sea's level (none, or the water setting off: -1e30). A cone or
reflection ray that crosses down through it before hitting anything stops
there and sees the same mirror over deep water's colour (WaterLook's
default: the seas' own looks are not in the frame); one from under it that
crosses up sees the water's glow, dimmed (the seabed's bounce light is not
the open sky's). The water pass's reflections ignore the sea plane (they
start on a surface riding the swell about it). Waves and the swell are not
in the voxels (they are flat at the body's level), and dry ground below
the sea's level is under the sea plane, as the sea is drawn there too.

Weather on the water (W5c). Rain rings the water where the sky is open
over it (the cover map's shelter, as for falling rain: without the cover
map, as without the voxel world, rain rings water under a roof too; none with
`precipitation` off or without the sky's weather): three layers of cells
0.5, 0.4 and 0.32 m across on the ground's wrapped place, each with a drop
a second landing by chance (`Sky::rain` times 0.85) at a hashed place and
moment, its ring spreading to 0.3 of the cell in 0.6 s as it fades (crests
3 cm apart, 1.2 mm high), faded out before the cell's edge. Nothing is
kept from frame to frame: the drops are hashed by their second within the
weather's 64 s clock on the water (`WaterPush::rain`, `rain_clock`).
Rings are drawn where a pixel has four to a ring's wavelength (as the
waves: near by, looking down); past that, and on water running faster than
half a metre a second (the rings would ride its flow), the rain roughens
the glint instead.

A strong wind's crests foam: whitecaps over the share of open water
Monahan and O'Muircheartaigh's fit gives (`water_whitecaps`: 3.84e-6
U^3.41, about 1 % at 10 m/s, 10 % at 20 m/s, at most 0.3, none under
3 m/s). Crests are 0.5 plus the mean of the waves' sines, spread normally
(a quarter with every wave drawn), so the foam is on those above 0.5 plus
the spread times the share's normal deviate (`water_whitecap_z`); past
0.97 (crests are clamped at 1) the line stays and the foam thins to the
share. Where the waves are too fine to draw (far out) the foam is the
share spread evenly (the crests' line used only once nearly every wave is
drawn). A noise drifting with the wind breaks it up (thin foam keeps its
share, solid foam loses about a tenth), each octave smoothed as the pixel
outgrows it. The waves
themselves already grow with the wind (W2a), the surf with it (W4b) and
the sea's swell (W5a). Snow falls into the water and is gone; ice waits
for a later part.

Sound and splashes (W5d, W5e) and wetting (W5f) follow below; the water
gate (W6, `tools/water_gate.py`) flies the N3 valley with a lake, a river
past the town and a coast, over and under the water, and holds the water
to 1.5 ms of GPU time.

## Particles (effects plan FX1a)

Smoke, sparks, spray and the like are particles (`render/src/particles.h`):
born on the CPU from `FrameInput::particle_bursts`, each written once into
a ring of `k_max_particles` (262,144) 64-byte records on the GPU, made at
init, and never simulated after. A particle's place at any age is a closed
form of how it was born: it takes the speed of the air it was born in (the
wind times its style's `wind`) at its style's `drag` per second while
gravity and buoyancy pull on it, so x(t) = x0 + air t + (v0 - air) E1 +
lift E2 with E1 = (1 - e^-kt)/k and E2 = (t - E1)/k (plain ballistics with
no drag; series to the 7th power where kt is under a half, so a little
drag over a long life loses no digits). The same age is always the same place:
nothing is kept from frame to frame, so particles cannot drift or flicker
without the history AGENTS.md rule 2 forbids. A burst given a `ground`
height has each particle's landing age found at birth (its height has one
top or bottom at most, so Newton within a bracket on one side of it); it
stops there. Births cost the CPU about 0.05 microseconds a particle and
that search about 0.3 more (measured on the build machine), so a frame is
capped at `k_max_particles_born` (16,384) births and
`k_max_particles_grounded` (4,096) of them with a ground: about 2 ms at
worst. A style's numbers are
bounded (gravity and wind shares to 100, buoyancy to 100 g, sizes to
1 km, colours to 1e4) so nothing overflows a float.

Births are deterministic: a particle's spread, cone direction, speed and
life come from hashing the burst's `seed` and its index. Bad numbers, a
missing or bad style, or a burst over 8 km from the camera leave its
particles out, counted in `RenderStats::particles_dropped`, as are those
past the caps. Positions are relative to an
anchor near the camera (floats); a camera over 4 km from the anchor drops
them all and moves it. Each frame's births are one span of the ring that
ends when its longest-lived particle dies (lives are at most 60 s); spans
end from the oldest, so the alive range is known without reading anything
back (some in it may have died: the shader skips those by age), and a full
ring cuts the oldest (`particles_cut`). The clock is
`FrameInput::particle_seconds` (the game's: the editor passes its game
seconds, so a paused game's stand still), or `delta_seconds` when that is
negative (at most 1; 0: still), and births go on whatever view
is drawn. A frame's births go up in one upload (at most 1 MiB, two copies
where they wrap); with no upload memory they are taken back and counted
as dropped ("particles" in the CPU sections).
Drawing them (FX1b, `shaders/particles.slang`, timed "particles") comes
after water and before post, in the lit view, over the light before
exposure (`RenderSettings::particles`). A compute pass takes each alive
particle once: where it is (the same closed form, plus its style's seeded
wobble), its size, opacity and colour over its life, and its light at its
centre: the sun shadowed (the virtual shadow map, the clouds' shadow), the
sky and ground round it or the voxel world's bounce light where it has
some, the lamps listed in its cluster (shadowed), its own emissive light,
then the air between it and the camera (aerial perspective, fog, clouds,
what falls, dust) as over a surface there. A soft ball is lit on the side
the camera sees and a quarter on its far side. Dead, out of the frustum or
of a bad style, it is not drawn. Then a quad for each (a puff facing the
camera with a lumpy edge its seed picks; a streak stretched along its
motion as seen, a sixtieth of a second long) is drawn at half resolution
into weighted blended transparency's two sums (McGuire and Bavoil 2013;
no sorting, so nothing pops as the order changes), each pixel testing the
scene's depth itself (no depth target), fading over its style's `soft`
metres into what lies behind and over the last half metre before the
camera. The sums are 32-bit floats (bright sparks near the camera
outgrow 16-bit ones). Last, the light is copied into a target of its own
with the sums brought up to full resolution over it, each full pixel
weighting the four half-size pixels round it by how near their scene
depth lies to its own (so a particle behind an edge does not bleed over
it); over the flat background (shown as it is, not exposed) the
particles are exposed and tone mapped there, as dust is. No temporal
noise or history: the same clock draws the same image. Known gaps:
particles under a water surface draw over it (the depth tested is the
scene's, without the water); lit at their centres, a big puff half in
shadow is lit evenly; a particle whose centre is off the screen while its
quad reaches onto it takes the lamps of the cluster at the screen's edge
nearest its centre.

## Effects (effects plan FX2, FX3, FX4)

Effects are world data (`world/effects.h`), saved with the scene and the
world's world.json like water. A `ParticleLook` is the renderer's
`ParticleStyle` by name (the world module does not see the renderer; the
editor turns one into the other). An `Effect` is a named list of up to 16
`Emitter`s, each a stream of one look: `rate` a second while it runs, a
`burst` when spawned, from a point, sphere, box or disc at an offset, in a
cone round a direction, at a speed, stopping on the ground or not. A spawned
effect runs its rates for its `duration` after its bursts; an entity's
`EntityEffect` (on, effect name, rate scale) runs it while on, turned with
the entity. A built-in library (smoke, fire, sparks, dust_kick, spray, mist,
embers, debris_chips, splinters, leaves, rain_splash, grit: a look and an effect of each name, made once per
process) is always there; the scene's own of a name stands in for the
built-in one (`find_particle_look`, `find_effect`), so a world can retune
smoke without naming a new look. Looks are bounded as styles are (and at
most 192 a scene, so with the built-ins they fit the renderer's 256 styles).
Effects name looks loosely: one naming a look found nowhere draws nothing of
that emitter (get_effects lists it in `missing_looks`), and removing a look
an effect still needs is refused.

The editor's `EffectsState` (editor/src/effects_state.h) turns them into
the renderer's particle bursts each frame, on the game's clock (paused:
nothing born, and those alive stand still: `FrameInput::particle_seconds`;
a frame not drawn hands its births and seconds to the next). It follows the scene's changes for entities whose effect is
on (never scanning every entity a frame) and keeps spawned effects
(`spawn_effect`, at most 256, held in place so a spawn never allocates; by
name, so an edit made while one runs is followed and one removed or renamed
ends it). Effects
farther than 1000 m from the camera run nothing; the rest go nearest first
within budgets of 8,192 births a frame and 2,048 that stop on the ground
(where there is ground under them)
(half the renderer's caps, shared with the events' spawns; an eighth goes to the weather, the rest is headroom), the farther ones'
left out and counted. A rate's fractions carry from frame to frame, a long
frame owes at most a second of them (as the particles' clock steps), a
spawn's stop with its duration, and each burst's seed hashes
the entity (or spawn), the emitter and a count of updates, so the same steps
give the same particles. Points and spheres are one burst each (a sphere is
the burst's spread); boxes and discs are up to 64 bursts of their own
points. Where particles stop is the terrain's height under each box or disc
point, under a sphere's middle less its radius, or where they were born when
that is under it; with no terrain there they fall on. Its lists are made at
their caps once. Effects are found by name through an index remade when the
scene's effects change, and only for those within reach. Each look keeps its
style slot while it is there (particles alive up to a minute keep their
index): a changed look is changed in place, a new name takes a new slot, and
a removed name's slot is reused only once the renderer's 256 are full, the
one gone longest first.

The world's events set effects off (editor/src/event_effects.h, effects
plan FX3), as spawned effects, so within the spawned cap and the budgets
above (nearest first): a shot's hit throws up what its surface is made of,
turned to the surface (sparks off metal; chips and dust off concrete, brick
and the rest; chips off glass; splinters off wood; a few leaves off
foliage; dust and chips off dirt; nothing off flesh), more for a round
leaving more energy (a rifle round's 3 kJ as built, 0.3 to 2 times); what
goes into water throws spray by its size and speed (a person jumping in as
built, up to four times); every new dust puff (`sim::Dust` keeps up to 256
as `DustEvent`s between takings, with what threw it up, rubble, ground or a
tree, and where from: the ground under the puff; emits swelling a young
puff, as a trunk bouncing, are none) throws chips off a collapse, leaves off
a felled tree, dust and chips off a crater or an uprooting, by its radius;
and the player running (3 m/s or more) on dry bare ground (dirt: the
terrain, or what is made of it; not wet past 0.3, snow past 0.2, nor in
water, nor carried) kicks a little dust every 1.6 m. Spawning allocates
nothing (spawned effects keep their effect's name in place). They chain after the sounds on the
physics' `on_hits` and `on_splashes`; the dust is taken each frame before
the effects update. `get_effects` counts them (`emitting.events`), those
left out past the cap too.

An effect may give light while it runs (`EffectLight`, effects plan FX4a):
a point lamp at an offset in the effect's own space, of a colour,
intensity, range and size, casting shadows or not, dimmed by up to its
`flicker` share. The built-in fire glows (firelight, 4 at 1 m, 8 m,
flicker 0.3), and a scene's own effect made under a built-in one's name
starts with its light. The flicker is three sines (1.7, 4.3 and 9.1 Hz) of
the effects' running clock (the game's seconds summed while they run, at
most a second a frame; not saved) whose phases hash the running effect
(entity id or spawn), so it is the same at the same time, apart frame to
frame only as the clock runs (no noise), each fire its own; paused, it
stands and the glow stays. `EffectsState` gives the lights of the effects
running within reach, dimmed by their rate below 1: an entity's while on,
visible and at a rate over 0; a spawn's for its duration, at least a
quarter second on the game clock (only that at rate 0: a flash from bursts,
the same at any frame rate), holding its spawned slot while it glows and
following its effect's light turned on or off meanwhile. Another scene's
lights go when it does. The nearest 256 a frame are given (the rest counted as
`lights_cut`), from lists made once; a casting one's shadow is kept by its
entity or spawn (ids with the top bit set; the lamps' have it clear). The
editor puts them after the scene's lamps in `FrameInput::lights`, last
update's as with the particles, so they bin, shade, cast and feed the voxel
GI as lamps do (a flicker changes no shadow: only a move or a new range
redraws one).

The weather gives particles near the camera too (editor/src/weather_effects.h,
effects plan FX4b), on the game clock (paused: none), each from a hash of a
running count so the same frames give the same bursts. Rain splashes on
what it falls on within 20 m: 600 a second in the hardest rain (by how much
of it is rain, as the weather draws it: `world::falling`), at most 64 a
frame, none on snow lying past half cover, each a crown of 3 tiny drops
(`rain_splash`) hopping up off the first surface a physics ray down from
100 m over the camera (or the ground there) meets (a roof, a deck, a slab;
not under the terrain), else the terrain. A splash at or under the water's
level (the physics' water, which has its own rings) is left out; so is one
whose ray begins inside something tall, and all of them with the camera
over 30 m under the ground there (a tunnel) or over 100 m above it. In a storm (wind from 8 m/s, the most from 20) leaves (40 a second
at most) and grit (`grit`, 120, only off dry ground: less by its wetness and
snow) are blown along from a disc of 25 m centred 12 m upwind of the camera,
already moving at 0.8 of the wind, at most 16 of each a frame, off the top
of what lies on the ground (a ray down from 3 m), none over water nor where
something is overhead (a physics ray up 100 m: indoors, under a bridge);
each stops on the lowest terrain along its way downwind as far as the wind
takes it in 7 s (sinking from sight over a rise, never resting in the air
over a fall). They go to `EffectsState::add_burst`,
within the weather's own budgets of 2,048 births a frame and 512 landing
(an eighth of the renderer's caps, apart from the effects'), all of a burst
or none. get_effects reports them (`emitting.weather`).

The effects gate (FX5, `tools/effects_gate.py`) flies the N3 flight over
the water gate's valley with smoke from every chimney, fires along the
street and the river, a breakable boathouse knocked into the river and
shots fired from the street, in a storm, simulating. It holds the flight
to 60 fps without micro stutter and the "particles" pass to 1.0 ms in the
street (caught up first), and checks every kind of effect went off (all
but grit, which the rain keeps down). The
gates that fly the storm count draws less the particles' two passes, which
come and go with what is alive.

## Navigation

`giant/nav/nav.h` says where people can walk and finds the way. The
walkable ground is a grid per world cell (64 m): 128 x 128 samples 0.5 m
apart, each with the height one stands at and whether one can. A sample is
walkable where the ground is no steeper than `max_slope_degrees` (40),
nothing is in the way from `step_height` (0.4 m) above it up to
`clearance` (1.8 m), and it is `agent_radius` (0.35 m) from what is
(obstacles, oriented boxes, grow by it). Low things (a kerb, a slab) are
stood on: their top is the height there. Where there is no terrain, the
ground is the lowest obstacle top with no lid too low over it (a floor
plane, a deck, not a footing under the floor). `build_tile` is pure (any
thread): about 1.6 ms a tile, sampling the terrain once on a grid a sample
wider than the tile for the slopes at its edges.

`PathSearch` is A* over the samples of the tiles there are (8 ways, no
step higher than `step_height`, no corner cut past a blocked sample), in
slices of `advance(nodes)` (rule 9: about 0.45 us a sample, so a frame's
1536 cost about 0.7 ms), from and to the nearest walkable samples within
`snap_distance`. What it knows of each sample is kept densely per tile.
Reaching a tile not built it stops in `missing` (that cell): build it and
advance again. A path found is pulled straight where the way is clear
(corners only where it turns). It gives up at `max_nodes` samples (about
0.8 km of open ground across); a hierarchy of tiles for longer ways is
planned.

The editor's `NavState` (editor/src/nav_state.h) keeps the map for the
scene: it follows the scene's changes (each fixed entity's boxes, shown or
not, as the physics collides with hidden ones:
cubes, spheres, planes as a slab under their top, meshes as a box inside
their bounding sphere, kit pieces not knocked loose; scatter trees'
trunks) by cell, and a built tile's inputs (the terrain's tile versions and
the scatter serials around it, its cell's entity stamp) are checked a few a
frame: tiles whose inputs changed are built again, two at a time on its own
two workers, from copies, so the scene changes freely meanwhile. Searches
(`find_path`) move on within `nodes_per_frame` a frame, asking for the
tiles they wait for first; the oldest unused tiles go past `max_tiles`
(512, about 40 MB; a rebuild is not a use). A search found over a tile
older than what it is built from is searched again once that tile is
built. A search that read a tile since rebuilt or dropped starts again
(`PathSearch` checks the versions of the tiles it read; at most 16
times, then it fails), and making a
found path (walking back, pulling straight) is spread over `advance`
calls like the search. A kit or mesh change looks at the entities using
it again, 256 a frame; a new scene drops every tile. The `find_path`
command builds what it needs now (bounded), rebuilding a stale tile its
path crosses and searching again (`stale` says one was left).
Dynamic bodies, swinging things, animated meshes, cloth and decals are not
in the map: what moves is avoided by what walks, not by the map. Known
limits: scatter trunks count only where the scatter is placed (within its
radius of the camera), so a far path ignores forests and tiles there are
built again as the camera nears; an entity covering more than 64 cells (a
floor under a town) makes every built tile stale when it changes; felled
scatter trees still block until their cell is placed again;
a mesh is a box (not its shape); one storey a sample where there is
terrain (a bridge over ground is stood on only if within a step).

### Walkers

`giant/nav/follow.h`'s `PathFollower` steers along a found path: towards
the next corner (turning for the one after within 0.6 m, or once nearer
it), slowing over the last 1.2 m and stopping within 0.25 m of the end.
Others near (the nearest 8 within 1.5 m past touching) push it away, the
closer the harder, and one ahead coming at it makes it step to its right,
so two meeting pass left side to left side. A corner is passed once it
is near or walked beyond (along the way to it), so a hairpin is walked
round. At the end, one standing there (or crowding it too) lets it stop as
near as it can come. Pure: the caller moves it.

The editor's `WalkerState` (editor/src/walker_state.h) makes an entity
sent walking (`walk_to`, body `none`, while simulating) a physics
character, steers each before the physics steps (others found by 2 m
squares) and poses each after (between the last two fixed steps, turned
to face where it goes). Poses go to the scene's drawing (the first rebuilds its cell, marked
moving; later ones move it in place; kept out of the voxel world); the scene keeps where it
was put. `walk_to` finds the way at once when it takes at most 64 tiles,
else NavState searches over the next frames. A walker that moved under a third of
what it was steered to over 2 s of simulated time looks for the way again
(four searches in all, then it has failed); it takes a search's answer
only once NavState has settled it (not waiting on stale tiles). Its
character is as wide as the entity (0.2 m round up to the map's agent
radius) and, for a cube
or sphere, as tall (up to the map's clearance); it carries the entity's id, so shots and raycasts name
it (it is hit as its capsule: no hit zones yet). Others it keeps clear of
are the walkers in the 2 m squares round it (its own first) and the
player. Stopping the simulation (even started again before the next
frame) or another scene lets every walker go; `step_physics` steers them
before each fixed step. Walker poses go to the drawing in a map of their
own (`scene_frame`'s `walkers`). Known limits: a
walker is drawn in the cell the scene puts it in (one walking far may be
culled with that cell); no animation is chosen for its pace yet; a stuck
walker's new search waits for frames (not step_physics).

## People (living-world plan LW1a)

The people who live in the world are world data of their own
(`giant/world/people.h`, the types in `scene.h`), not entities: thousands
of small records the scene keeps whichever cells are loaded, since people
cross cells. Three lists:

- `Place`: a named spot people go to (a home, a workplace, a bench) with a
  position, a radius, the way one stands there and up to 8 tags saying what
  it is for. At most 65536.
- `PersonLook`: how a kind of person looks: a skinned mesh file, a height,
  a material, parts' looks, hair and matched clips, exactly what an entity
  drawn for one is given. Many people share one, so a crowd costs a few
  meshes. At most 1024. A look's mesh is among the meshes a scene or a
  world imports on opening.
- `Person`: an id (its own for good: `next_person_id` only rises, and
  undoing a command does not lower it, since a save made meanwhile may
  hold the person), a name, a look, a home (a place, or none), where it
  starts, and a seed telling it from others of its look. At most 65536,
  kept by rising id, so one is found by halving.

`check_place`, `check_person_look` and `check_person` check one;
`check_people` checks them together (names unique, every look and home
there, ids rising under the next). Loading keeps the same limits, so a
loaded scene is one the commands could have made.

Saved in the scene's text and in a world's world.json under `places`,
`person_looks` and `people`; only what is there is written, so older files
read and write the same. People are many, so each is a row
(`[id, name, look, home, x, y, z, yaw, seed]` in `people.list`, beside
`people.next_id`), not an object. The world's streaming carries the
lists (and the routines' and behaviours' below) between the index and the scene as it does water, and counts a
change to them as an unsaved setting.

The commands (`people_commands.cpp`): `create_place` / `set_place` /
`remove_place` / `get_places`, `set_person_look` (which takes a look from a
dressed entity, so every way of dressing a character serves people too) /
`remove_person_look` / `get_person_looks`, and `create_people` (up to 4096
a call) / `set_person` / `remove_people` / `get_people`. An undo step
keeps what its command changed (the place or look at its index, the people
taken out and put in, swapped in one pass through the list), never a copy
of the lists: 65536 people copied twice a command would not do.
Commands run outside the history (C# scripts) may have changed the lists
since a step was recorded, so a step finds its place or look by name (the
index only says where one is put back) and leaves undone what would break
the scene: taking out a place or look people still have, bringing back
people whose look is gone (one whose home is gone comes back without one).

### Routines (LW3 part 1)

What people do through the day is world data too: `world::Routine` (a
name and 1 to 24 `RoutineEntry`s), at most 1024 in `Scene::routines` and
`WorldIndex::routines`, and `Person::routine` names one ("" for none). An
entry begins at an hour (`from`, 0 to under 24, rising through the list)
and holds until the next; the last holds over midnight until the first
(`routine_entry_at(routine, hour)` gives the one that holds,
`routine_entry_ends` when it ends). It says what one does then
(`activity`, a word as a place's tag is) and where (`at`): `stay` where it
is, its `home`, the `place` named, or a place with a `tag` (each person
its own: the nearest to its home), going there at `speed`. This part only
keeps them; part 2, below, has people act on them.

`check_routine` checks one, `check_people` that names are unique and each
person's routine is there. A place a routine names need not be there (a
place may be removed under it: the people stay where they are then), but
`set_routine` refuses one that is not there when it is set, so a typing
slip is told. An undo that would put back one more place, look or
routine than a scene keeps is left undone. Saved as `routines` in the
scene's or the world index's text (an entry's `at` and `speed` only when
not `stay` and 1.4); a person with a routine has it as a tenth field of
its row, one without is written as before, so older files read and write
the same.

The commands: `set_routine` (makes or replaces one whole) /
`remove_routine` (refused while people have it) / `get_routines` (with
`hour`: the entry of each that holds then), and `routine` on
`create_people`, `set_person` and `get_people`. Undo keeps the routine
before and after, as for a place or a look; people put back whose routine
is gone come back without one. An entity standing for a person is not
made again when only its routine changes.

### Following routines (LW3 part 2)

`PeopleRoutines` (`editor/src/people_routines.h`) has people act on their
routines while the physics simulates. Each frame, after
`PeopleTravel::update`, it looks at the next 512 people of the scene's
list (`looked_a_frame`). For one with a routine it takes the entry that
holds at the person's own hour: the time of day shown (`ClockState`),
late by the person's share of `spread_minutes` (default 20; by its id,
the same every day), so a village sets off over twenty minutes, not in
one frame. When what that entry says to the person (where to be and how
fast to go there, with its home for `home` and `tag`: a hash kept in a
slot beside the person's place in the list) is another than what it last
acted on, it acts: `stay` stops the trip the routine began for it;
otherwise the place is found (its home, the place named, or for a tag
the place with it nearest its home, nearest where it starts for one with
no home) and a spot of the person's own within 0.8 of the place's
radius, and `PeopleTravel::send` sends it there at the entry's speed,
unless it stands within the place or is on its way to that spot already.
The next entry at the same place is nothing new: the activity changes,
nobody is sent. At most 32 are sent or stopped a frame (`sent_a_frame`,
at most 128); the rest wait their next turn. One that cannot be sent (no
home, no such place, no road within `within`, or its way not found after
all: its trip's `failed` phase, seen when it is next looked at) stays
and is marked `failed`, told once, and is tried again when the places
change, the route graph is built again (`PeopleTravel::builds`) or its
entry changes.

A person sent by hand (`send_person`) is left to it until its routine
next says something else, and `stay` does not stop a trip the routine
did not begin (the slot keeps its trip's serial). Known limits: a place
moved after someone set off for it is not followed until that person's
next entry; `spread_minutes` changed while running has each act on the
entry its new hour gives.

Slots lie beside the people in the order of their ids. When people are
made or removed the slots are matched to them again by id in one pass
(an edit's cost), so the others keep what they acted on and are not sent
again.

Places are found by halving in two lists of (hash, place index) sorted
by hash: one of names, one of tags. The hash is seeded per editor, so a
scene's names cannot be made to share one. No copy of the places is
kept: every find is checked against the scene as it is (the name is the
same, the tag is there), so a list out of date finds too little and
never the wrong place. The lists are made again when the number of
places changes, or when one of the 256 places looked at that frame no
longer has the name, tags and position it had (a 32-bit sign of each is
kept): an edit's cost, like the route graph's. They are kept from one
run to the next. Until they are made again a place just changed may be
missed, or for a tag one taken that is no longer the nearest. The places
with a tag are listed in the order of their x, and the nearest is looked
for outward from the person's home both ways until x alone is farther
than the nearest found, so few are measured however many have the tag.
The answer is kept by (home, tag) in 1024 slots, so those of one home
measure once; at most 16384 tagged places are measured a frame, the rest
of the people waiting a frame. `doing` gives the place a person was sent
to (its name's hash is kept in the slot and found again in the list),
not where its entry would send it now.

`send` called this way (`in_frame`) takes the routes as `update` last
saw them: `PeopleTravel::update` now compares a stretch of the routes
every frame while simulating (not only while a search waits) and builds
the graph again when they differ, and a run starts by comparing them
whole (roads edited while stopped). Its refusals in a frame are a few
words (no memory taken); a trip that is sent takes memory for its way
and its place in the lists (a few small pieces each, at most
`sent_a_frame` trips a frame: a known cost). `RouteGraph::nearest`
looks square by square in rings outward, only among the squares that
have nodes, and stops at the first ring past the nearest found, so a
person by a road costs a few squares however far `within` reaches;
`within` here is at most 1000 m.

So that a village's morning costs one search, `PeopleTravel` keeps the
last 256 ways found (from one graph node to another; `Way`): a trip
between the same two nodes takes a copy of the way kept instead of
searching (`reused` in `get_travel`), without spending a search slice
for up to 256 trips and 8192 nodes a frame (past that, slices by its
length). Building the graph again
forgets them. Ways not kept are still searched for one at a time: many
people sent at once to places far apart wait their turn (a known limit;
`PeopleTravel::Settings::search_work_a_frame` is the budget).

Stopping the simulation, another run or another scene empties every
slot: everyone is back where the scene has them and begins its routine
afresh. `get_people` gives `doing` (the activity, the place sent to, and
`failed`); `set_routine_settings` sets and reads the settings with
`following`, `sent`, `stayed` and `failed`. Not saved, not undone.

Known limits (the next parts): the time of day only moves in play,
and people travel in real seconds whatever the clock's speed, so at 60
game seconds a second a ten-minute walk takes ten game hours; game saves
keep neither trips nor what each was doing; everyone with one home and
one tag shares a place.

### Activities shown (LW3 part 3)

A person look says what its people show while they do an activity:
`world::PersonLook::activities`, up to 32 `LookActivity` (`activity`, a
routine's word; `clip`, one of the look's file; `speed`; `loop`), each
activity once, saved with the look and set whole by `set_person_look`.

`PeopleState::act` (each frame, after `walk`) goes through the people
that have an entity. One that is where its routine has it (no trip, no
walker walking, not lying as a ragdoll; its entry acted on and not failed, and
either a `stay` or the person within the radius, and 6 m, of the place
it was sent to: `PeopleRoutines::activity`, which gives nothing while
routines are off or the world is stopped) and whose look names a clip
for that activity plays it: `AnimationState::play`, as a command's
play, fading in over 0.3 s. It takes over from the motion matching,
which is given back (`AnimationState::restart_saved`) at once when the
person walks again, does something its look has no clip for, or the
world stops. At most 8 clips are begun a frame (each poses its
character at once), at most 2 of them on entities not animated yet
(each makes its copy of the character, beside the animation's own few
a frame); ending one is never put off. A clip the file does not have (`AnimationState::playable`,
asked before playing so that no error text is made) is not tried again
that run, or until the look's list changes; with the file not read yet
it is tried again after 2 s. A play taken away (`set_animation` on the
entity by hand, the file read again) is seen the next frame
(`by_command` gone) and begun again after 2 s; a clip played on it by
hand (`play_animation`) is not told from the routine's and is left
until what the person does next changes.
`set_people_settings` reads `acting`: how many play their activity's
clip now.

What one does is the entry it last acted on: a person whose turn to be
sent has not come yet still does the last entry's activity (so nobody
eats at the mill for the frames before it sets off). An entity made
again under the same id (its last deleted, or taken when the world
stopped and made again the same frame) starts from its saved animation,
not from what the last one played. A change to a look's `activities`
alone leaves the entities standing: those playing a clip are matched
again and begin the new list's.

Known limits: going from the matcher to a clip and back is a cut (the
matcher's pose is not the player's to fade from); a person given back
to the matcher shows its rest pose until the matcher takes it up, 16 a
frame (`matches_per_frame`), so hundreds ending together (the world
stopped, routines turned off) take some frames; a place list edited
can end and begin a clip again within a few frames (the lists catch
up); nobody sits down or
turns to face anything first (the place's `yaw` is not used yet); what
is played is the look's, not the place's (a bench and a bed play the
same `rest`); an entity made again (the camera gone and back) begins
its clip from the start, a once-only one too.

### Game saves keep the people (LW4 part 1)

A game save made while the world simulates has a `people` section: for
each person that is not where the scene has it (`at`: x, y, z, yaw), is
on a trip (`trip`: `to`, `speed`, `within`, the `place` asked for by
name), and for those what each had acted on of its routine (`did`: what
it acted on, `said`, a hash of the entry's words; the `entry`; the
`place` it was sent to; `failed`; `own_trip`, the trip being the
routine's), in the order of their ids. One that has not left home
this run is not written (its routine looks at it afresh and finds it
where it should be); everyone that has travelled is, at every save and
autosave. A save with nobody still writes the section, empty, so that
a slot saved again never loads the people it had before. The list is gathered on the frame as plain
structs (`GameSaves::SavedPerson`: one pass over the scene's people,
two lookups each and, for those written, the names of the places they
go to copied; counted in `gather_ms` with the rest of the gathering)
and made into text on the save's job.

A load starts the simulation again (a new run: everyone is where the
scene has them, the routines begin afresh) and then puts the people
back, `GameSaves::people_per_frame` (1024) a frame, of them at most
`people_sent_per_frame` (64) that are sent on (each a trip made, its
ends looked for among the road's nodes), in
`GameSaves::place_people`: `PeopleTravel::put` stands one where it was,
`PeopleTravel::send` (the in-frame kind: the roads as last seen) sends
it on to where it was going, the way looked for again from there, and
`PeopleRoutines::restore` tells its routine what it had acted on, so
that the routine neither sends it a second time nor takes its trip for
somebody else's (it is told nothing when the entry says something else
now: the routine changed since, and is acted on afresh). One whose
routine's trip cannot be sent again (the roads are others) is left to
its routine, which sends it afresh; one whose own trip (a command's)
cannot is still told what its routine had done; one whose routine says
something else now has the routine's trip stopped where it is. A
person on the roads may be part way along a long stretch, farther
from the road's nodes than the `within` it was sent with:
`PeopleTravel::send` looks for the node it joins at within
`k_join_within` (4096 m) for one going along the roads, one stopped
part way along them, and one a loaded game put back (the others, at
home or where a trip ended, keep `within`). While any wait,
`PeopleRoutines::hold` and `PeopleState::hold` are set: nobody is
looked at by the routines (one not put back yet would be sent from its
home) and nobody has an entity (one would stand at home and then be
elsewhere). A person gone from the world since is skipped; the
simulation stopped before all are back skips the rest. A save made
while some still wait writes those as the save they came from had
them. A section that is not as written (ids not rising, a place out of
the world, a speed no trip may have, an entry no routine may have) is
refused with the whole load. `get_save` tells `people`,
`people_applied`, `people_skipped` and `people_waiting`.

The state of its behaviour a person is in is kept too (LW6 part 5):
`in` {`behaviour`, `state` (by name), `seconds` in it, `settled`},
and `did.own` when what it had acted on was the state's own. One in a
later state than the first, or doing what a state says, is written
though it stands where the scene has it; one at home in its first
state still is not (a loaded game begins it there, and tells of it
as beginning; one in its first state that moved is written, and goes
on in it). Loaded, it is in
that state with the seconds it had (one hiding for a minute hides the
rest of it), its trip is the state's again, and nothing is told of it
as beginning. A behaviour that has no state of that name now, or a
person given another behaviour since, begins afresh, and a trip that
was the state's is not gone on with. What it had done not put back
(its routine or the state says something else now, or its trip could
not be sent again), it is in its state still and acts on it again,
neither arrived nor failed until then. A save without `in` (an older
one) loads as before. What alarmed people is not kept: a loaded game
is calm.

Known limits: a trip is sent again from where the person was, to the
nearest road node, so one saved in the middle of a long stretch walks
straight to the nearer end of it first (back, if that is nearer), and
one more than 4096 m from every node (a stretch over 8 km with no node
between) cannot be sent on and stands where it was; with 65,536
people all away from home the gathering copies as many place names on
the save's frame (`gather_ms` tells); one that was waiting for its way, or
whose way had failed, is where it set off from; a person whose routine
or its entries changed between the save and the load is told nothing
(its routine sends it afresh); what the hours' spread had not yet sent
is sent at its own time as before; the clips people play (LW3 part 3)
begin again from their start.

### What people hear (LW5a)

`PeopleSenses` (`editor/src/people_senses.h`) is the people's hearing.
While the world runs, a noise (`noise(at, loudness, kind)`: where, its
level at 1 m on the audio's scale, and what it was) spreads at 343 m/s
and is heard by every person it is loud enough for where the person is
(`PeopleTravel::where`: near the camera or a kilometre off, with an
entity or without). The level falls with the distance as the audio's
does; under `hear_loudness` (0.2) nobody notices, so a rifle's report
(100) is heard to 500 m and a shout (3) to 15 m.

What one heard raises its alarm, 0 to 1: a noise at `alarm_loudness`
(2) or over alarms fully, a quieter one in proportion, and each takes
its share of what calm is left (1 - (1 - a)(1 - b)), so a few far
shots add up. The noise that alarmed most is remembered (`kind`,
where it came `from`, its `level` at the ears). Alarm eases back in a
straight line, a full one over `calm_seconds` (30), worked out from
when it was last raised: nothing is done a frame for the calm.
`get_people` gives `alarm` and `heard`; what people do about it is
LW5c.

Noises come from the shots fired by command (the report at the muzzle,
as loud as the listener's: `AudioState::shot_loudness`), and through
`connect_senses` from bullets striking (`on_hits`), blasts
(`on_impact`: 10 times the root of the push, a push of 100 N s as a
rifle) and splashes (`on_splashes`), and from `make_noise`.

Cost: at most 64 noises are kept (a louder one pushes the quietest
out); while any spreads, `looked_a_frame` (512, at most 16,384)
people are gone through a frame (5,000 people in ten frames: on the
owner's PC 4,096 a frame cost 0.6 ms a frame for as long as a shot
spread, the living-world gate's whole budget for the people), each against the noises kept (a
distance each; 262,144 such tests a frame at most, so fewer people
when `looked_a_frame` is set over 4,096 and many noises spread at
once), and none when it is quiet. Each
time through the list a person hears the noises whose front reached
it since the time before, so each hears each once whatever the list's
length, up to two passes late (a front that arrives just after a
pass began waits for the next: 20 frames for 5,000 people, a third of
a second at 60 fps, and 256 frames for 65,536, over four seconds:
worlds that full want `looked_a_frame` raised, or people kept by place,
which the crowds stage may bring); a pass
begins only once time has passed, so the first noise after quiet is
heard on the first pass. A noise is made by the clock as it is then:
the commands and the physics' hooks bring the clock up first
(`sync_senses`), so one made in a run's first frame or within a
`step_physics` is not lost. What each heard is a slot beside the scene's
people (matched again when people are made or removed), so hearing
takes no memory on the frame. Stopping the simulation calms everyone.

Known limits: nothing stands in a sound's way (a hill or a wall lets
it through; only the listener's own ears have the audio's bending and
muffling); weather neither carries nor drowns it; footsteps and
voices make no noise yet (LW5c gives people voices); alarm is not in
game saves; a person made while a noise spreads may miss it; where a
noise has reached goes by where a person is when looked at, so one
moving fast towards it may miss it and one moving away hear it twice
(about its speed over sound's: under half a percent for a walker);
`connect_blows` called after `connect_senses` takes the hooks back
(connect the senses last).

### What people see (LW5b)

`PeopleSenses` also keeps what there is to be seen: the player
(`PeopleSenses::player`, set by the editor each frame to the camera,
its eyes, while playing: a place of its own, never crowded out) and up
to 16 named things, each a point (`watch` / `unwatch`;
`show_to_people` and `hide_from_people`). While there is any,
`seen_a_frame` (1024) people's eyes are looked through a frame, in
turn: a person sees a thing within `see_distance` (60 m, at most 300)
of its eyes, within `see_angle` (120 degrees across) about the level
way it faces (a cone: something high above or far below is out of
it), or within arm's reach (1.5 m) whichever way it faces, and with
nothing between. Its eyes are 1.6 m over the ground where it stands
(the terrain's height there, as its entity stands; its own height
where there is no terrain).

What stands between is asked of `PeopleSenses::blocked`, set by
`connect_senses`: a ray through the physics' bodies (characters let
through: its own, the player's) and then the terrain. The ray stops
the thing's `clearance` short of it (0 for the player and by default;
about a body's half size for a thing shown at the middle of its own
body). At most `rays_a_frame` (32, at most 128) rays are cast a frame,
and none where seeing could change nothing (a thing that would alarm
less than one already seen this look, or than a person who holds
nothing by sight is alarmed already); when they run out the look stops at that person and thing
and goes on from there next frame.

Seeing alarms by nearness: fully within a fifth of the distance,
falling to a tenth at its end. The alarm is held there (`Slot::held`)
until the person is next looked through and no longer sees the thing,
however long a round of looks takes; from then it eases as after a
noise; seeing only something less alarming than what it saw, it
eases from where it was down to that. Seeing never lowers an alarm at
once. What was seen is remembered as what
was heard is when it alarms more than the noise remembered
(`get_people`: `heard` with kind `sight`, `from` where the thing was,
`seconds` since it was last looked at and seen).

Cost: while playing (or while anyone's alarm is still held),
`seen_a_frame` people are gone through every frame (where each is, and a distance to each thing; the facing and
the ground under it only for one with something near enough), plus
the rays. What is shown is forgotten when the simulation stops or
starts again, with another scene, and when the senses are turned off.

Known limits: one point a thing (a body half behind a wall is seen or
not by its point); a person given an entity is looked through from
where its trip has it and the way its path goes, not where its
walker has stepped aside or turned, and one lying as a ragdoll still
sees; with many people facing the player a round of rays takes long
(2000 people in view at 32 rays: about a second before the last
notices); light and weather do not shorten sight; far from the camera
the physics holds no bodies, so only the terrain hides things there;
a person does not see other people or bodies yet.

### What the alarmed do (LW5c)

A routine says what its people do while alarmed as it says what they
do through the day: `world::Routine::alarmed`, up to 8
`RoutineAlarm`s, each an alarm `over` (over 0, to 1, rising through
the list) and what one does then (`does`: a `RoutineEntry` but for
its hour: an activity, and stay, home, a place or a tag, at a speed).
Saved with the routine (`alarmed`, written only when there are any);
`set_routine` takes it.

`PeopleRoutines::update` is given the senses. For a person whose
routine has any, it asks the person's alarm (`PeopleSenses::alarm_at`,
the slot beside the person: no search) and, when it is at or over one,
takes the last such in place of the entry the hour gives. From there
nothing is new: what it says is acted on as any entry is (a trip sent,
or the routine's own trip stopped for `stay`), its activity is what
the person does (`get_people`'s `doing`, the look's clip for it), and
when the alarm has eased under it the hour's entry says something
else again, so the person goes back to its day. So `{over 0.2, watch}`
and `{over 0.6, flee, at home, speed 5}` has one stop and watch at a
noise, run home from a shot or the player close by, stop when it has
calmed some, and set off to work again when calm.

One on an alarmed entry leaves it only when its alarm is 0.05 under
the entry's `over` (`k_alarm_margin`), so an alarm hovering about an
`over` does not have it start and stop. An alarmed entry with nowhere
to go (no home, no such place) is acted on as `stay`, not told as
failed (and not tried again while it holds, should such a place be
made); one that cannot be sent (no road near, no way) is told as
failed and stops the routine's own trip, so it does not walk on with
its day. Staying says nothing of a speed (`said`), so going from one
`stay` to another only changes what the person does.

Game saves keep a person on an alarmed entry as any other (the
entry's number counts on past the day's). Alarm itself is not saved,
so a loaded game is calm: the hour's entry is then something else to
act on, and the trip the alarm began is the routine's to stop or
turn.

Bounds: none of its own. People are looked at in their turn
(`looked_a_frame`, 512) and acted on within `sent_a_frame` (32): each
change of what an alarm has one do takes one of those, and when a
frame's are spent the look waits there. So a shot heard by 5,000
people with alarmed entries takes about 2.6 s to reach the last of
them, and holds up the hour's changes of those after them meanwhile;
each `over` crossed on the way back to calm costs the same again.

Known limits: one does not turn to face what alarmed it; there is no
taking cover (a place with a tag is the nearest thing: `at tag`); a
person with no routine does not react; an alarmed entry that sends
one somewhere takes the place of a trip sent by hand (`send_person`),
`stay` leaves such a trip be, and calm again the hour's entry calls
the person back to its day; one that fled and turned its back on the
player calms, walks back, sees the player and flees again (about
every 25 s while the player stands there); a look's clip for the
alarmed activity plays where the routine has the person be, so one
that has run home goes on playing `flee` there while still alarmed
(name a clip that suits both, or none).

### The People panel (LW6a)

The editor's People window lists the scene's people, filters them,
shows what one does and why, and has the camera follow one. It is
made of three things in `editor/src/people_list.h`, and the commands
give the AI the same.

`PeopleList` finds the people that match a filter: text (in a name, a
look, a home, a routine or a behaviour, letters' case aside, or a
whole id), only
the alarmed, only those on a trip (one that failed is not). A scene
has up to 65,536 people, and who is alarmed or travelling changes as
the world runs, so the list is gone through 4,096 a frame, again and
again, while the window is open: what is shown is the last whole time
through (the first 1,024 that match, ids rising, with how many match
in all), so it is a frame behind for up to 4,096 people and half a
second or so for the most a scene holds, and never costs more than
its share. Each frame goes on from the id last looked at, so people
removed or put back meanwhile neither hide nor double anyone.
Another filter or scene begins again at once, and so does a window
that was closed or behind another tab. Two vectors kept at their full
size swap; the list allocates nothing a frame.
`get_people` takes the same filter (`text`, `alarmed`, `travelling`)
through the same `PeopleList::matches`.

`person_why` says in words what a person does and why: what alarmed
it and how long ago (a noise's kind, or what it saw), what its
routine says (the hour's entry and when it began, or the alarmed
entry and the alarm it holds over, or that it could not be sent), and
where its trip has got to (looking for its way, so far along, or why
it failed). What the routine says is what it last acted on, so just
after an alarm rises or eases the two may differ until its turn
comes. The panel shows it for the one picked (made anew a few times a
second); `get_people` gives it as `why`. It is for reading: the fields beside it (`doing`,
`alarm`, `heard`, `trip`) are the ones to act on.

`follow_person` has the viewport camera follow a person:
`EditorState::follow` (the person, the scene it is of, a distance),
and `follow_person_camera` each frame, once the people's trips have moved
on for it (so the camera is where the person is in the frame drawn,
a screenshot's too), puts
the camera that far behind the person, 1.5 m over the ground under
it, along the way the camera looks, and never under the ground; so turning the camera (the mouse,
`set_camera`'s yaw and pitch) looks round the person and a position
given is not kept. Following ends when the person is removed, another
scene is taken up, a game is loaded or play starts (the `play`
command lets go itself, so a play begun and stopped between two
frames ends it too; it does not come back when play stops). It is view state, as the camera is.

### Behaviours as data (LW6 part 2)

A behaviour says what the people who have it do as things happen to
them, as a small state machine kept as world data:
`world::Behaviour`, a name and up to 16 `BehaviourState`s. One with
the behaviour is in one of its states, the first to begin with. A
state says what one does while in it as a routine's entry does (an
activity, and stay, home, a place or a tag, at a speed), or nothing
of its own, and then the person's routine says (the hour's entry, or
an alarmed one). It has up to 8 `BehaviourTransition`s, each naming
another state and taken when all of its 1 to 4 `BehaviourCondition`s
hold, the first such in the order written:

| `is` | holds when |
| --- | --- |
| `alarm_over`, `alarm_under` | its alarm is at or over, or under, `value` (over 0, to 1) |
| `sensed` | what alarmed it most of late is of `kind` (shot, impact, blast, splash, voice, other or sight: no other word is taken) |
| `arrived` | it is where the state sent it (or the state sends it nowhere) |
| `failed` | it could not be sent where the state says |
| `after` | it has been in the state `value` seconds of the simulation |
| `hour` | the time of day is from `value` to `until` (over midnight when `until` is the less) |

So a `day` state that goes to `flee` on a shot, a `flee` state
(activity flee, at home, speed 5) that goes to `hide` on arriving,
and a `hide` state that goes back to `day` after two minutes once
its alarm is under 0.1 is a villager who runs home from gunfire and
comes out again when it is quiet. A routine's `alarmed` entries are
the short form of this for "while this alarmed, do that"; a behaviour
is for what has an order to it.

`Person::behaviour` names one ("" for none). A scene or a world keeps
up to 256 (`Scene::behaviours`, `WorldIndex::behaviours`), saved as
`behaviours` and only when there are any; a person's row has its
behaviour's name after its routine's (the routine's "" when it has
none), and only for one that has a behaviour, so files without
behaviours read and write as they did. `check_behaviour` checks one
(names, what each state does, each transition to another state of
it, each condition with what it needs and nothing else) and
`check_behaviours` the lot with the people; a file that fails either
is refused, saying where. A state's named place need not be there,
as a routine's need not.

Commands: `set_behaviour` (make or replace), `remove_behaviour`
(refused while people have it), `get_behaviours`; `create_people`
and `set_person` take `behaviour`, `get_people` gives it and filters
by it. All undoable through the same `PeopleChange` as places, looks
and routines, with the same care for commands run outside the
history.

### People act on their behaviours (LW6 part 3)

`PeopleRoutines` follows behaviours as it follows routines, in the
same pass (a share of the people a frame), since what a state has
one do is done the way an entry is. A person is followed when it has
a routine or a behaviour. Its slot keeps the state it is in (the
first, on each run) and when it entered it.

When one is looked at, the transitions of its state are gone through
in order and the first whose conditions all hold is taken: one a
look, so a chain of states takes a look each (people are looked at
many times a second). What each condition asks:

- `alarm_over`, `alarm_under`: the senses' alarm for it now.
- `sensed`: what alarmed it most of late (`PeopleSenses::heard`,
  asked once a look and only when a condition wants it); calm,
  nothing is.
- `arrived`: the state has been acted on and no trip begun for it is
  still under way. One the state sent nowhere, or that was there
  already, has arrived.
- `failed`: the state has been acted on and it could not be sent
  (no such place, no home, no road near), or its trip found no way.
- `after`: the simulation's seconds since it entered the state.
- `hour`: the person's own hour (late by its share of
  `spread_minutes`, as its routine's hours are).

Then the state says what it does. One with its own activity is acted
on as an entry is, in place of whatever the routine says, alarmed or
not: sent to its home, a place or a tag's nearest place, or stopped
where it is. One without leaves it to the routine, the hour's entry
or an alarmed one, as if it had no behaviour; with no routine either,
its own trip (one a state or its routine began, not one sent by
hand) is stopped and it stands. A state's own that cannot be done
does not leave one walking on with what came before: the trip is
stopped, and `failed` holds for a transition to catch.

A state's own is acted on before the state is left: while its turn
to be sent has not come (`sent_a_frame`), its transitions wait. It
is acted on each time the state is entered, though it says what was
last acted on, so states that fail into each other go round a look
each, and each tries again. When the places or the roads change,
one that had failed is acted on again, and neither `arrived` nor
`failed` holds until it has been.
One that goes from a state's own to a state whose routine entry
cannot be done does not walk on with the state's trip either.

`get_people` tells the state (`state`: its name, its place among the
behaviour's, seconds in it) and whether what it does is the state's
own (`doing.own`); `person_why` and the People panel say it in
words; `set_routine_settings` counts `behaving` and `changed`. The
look's clip for the state's activity plays as for any activity.

A behaviour replaced while the world runs: one in a state whose place
in the list is still there stays at that place, else begins again in
the first. Another behaviour removed or added does not disturb it.
A game save keeps the state (LW6 part 5, below).

### Scripts hear what people do (LW6 part 4)

So that a game adds its own to what people do, what happens to a
behaviour is told: a person began one (in its first state), or went
from a state to another.

- `PeopleRoutines` keeps the last 16384 in a ring made once (person,
  behaviour, the states by their places and their names' hashes, the
  simulation's seconds), each with a serial that counts on for as
  long as the editor runs. Nothing is allocated when one happens. The
  names are found when one is asked for: one whose behaviour is gone
  since, or has other names for those states now, or of another
  scene, is not told.
- `get_person_events {after, limit}` lists them oldest first, with
  `after` to go on from, `latest` (the last that happened) and
  `oldest` still kept: what the AI, or a tool, polls.
- C# scripts are told the same as `Script.OnPersonEvent(in PersonEvent)`
  before the frame's `OnUpdate`, in order, from when they were loaded:
  at most 256 a frame (many at once are told over the frames after, so
  a crowd that flees does not make one long frame; when more than the
  ring keeps are waiting, the oldest are not told, and a warning says
  so once). The script host
  copies the names before any script runs (a script may run commands
  that change the behaviours), and the C# side keeps each name's
  string and gives the same one again, so telling allocates nothing
  once the names have been seen.

A script answers by commands, as ever: `send_person`, `set_person`,
`set_behaviour`, a sound. A person whose behaviour is taken away is
not told of (there is no state it went to). A game save does not keep
the events.

### The chat with Claude (LW6b part 1)

The editor's own assistant: the person writes what they want, Claude does
it with the editor's commands. Three pieces, each knowing as little as it
can of the others:

- **The conversation** is C# (`scripts/Giant.Chat`, built to
  `managed/chat` with the libraries it brings), because the official
  client for Claude's service is a C# library and the engine has no HTTP
  of its own. `Conversation` keeps what is sent to Claude (the person's
  messages, Claude's replies handed back whole, thinking included, and the
  tools' results) and the lines the person reads, and works one message at
  a time on pool threads: a request, then the tools the reply asks for,
  then the next request, until Claude has no more to do, the person stops
  it, or 40 requests have gone (`MostSteps`). It asks for Claude Opus 5.5
  at medium effort, 16,000 tokens a reply, with the request's unchanging
  start cached and the service's own fallback for declined requests.
- **The tools** are C++ (`editor/src/chat_commands.cpp`): `find_commands`
  scores every command by the words asked for (found where a word of the
  command starts with one: "rain" in "rainfall", not in "terrain"; one in
  the name counts four times one in the description; words under three
  letters and "the", "and" and the like find nothing), `describe_command` gives one
  command's description and schema, `run_command` runs one through
  `CommandRegistry::execute` as a button would, so it is checked against
  its schema and lands on the undo history. Three tools instead of several
  hundred: the commands' descriptions alone would be most of every request.
  The chat's own commands and `quit_editor` are neither found nor run.
- **The bridge** (`editor/src/chat_bridge.cpp`) joins them through the
  script host's .NET (`ScriptHost::function` finds a static method of
  another assembly, which loads apart from the scripts with its own
  libraries). .NET and the chat start with the first message; reading,
  stopping or clearing a chat that never started answers without them.

Threads: commands may only run on the main thread, and a request to
Claude takes seconds. So the conversation never calls the editor: a tool
call waits in a queue, and `ChatBridge::update`, once a frame after the
scripts (and while the window is minimised), runs one (one a frame: a
command may take a while) and hands the result back to the waiting task.
Everything the editor calls returns at once. A stop reaches a waiting call
before it is taken; one the main thread has taken runs to its end and its
true result goes back.

What keeps a conversation whole whatever happens: Claude's reply and the
results of every tool it asked for are added to it together, after the
last tool, or not at all. Tools run only for a reply that ended by asking
for them: one cut off (at the length limit, or any other way) may have a
tool's arguments cut short, so none of its tools run and it is not kept;
words alone that were cut are kept, so "go on" works. A reply with nothing
to hand back (thoughts alone, empty text) is not kept. A reply the service
declined is not used at all: the person is told, and when it was the
message itself that was declined (no tool had run yet) the message is taken
back too, or it would be sent, and declined, again with every later one.
Stopped work answers the calls not yet run as "not run". A cleared
conversation is a new object: the old one's late lines and tools go
nowhere.

#### The person's say over Claude's changes (LW6b: ask before changes)

A command may come from Claude in the chat, from an MCP client or from a
script, and a person who is not a programmer cannot judge a command by its
name. So two things are theirs to allow, in the editor's window and by no
command (the same rule as a mod's scripts, LW7 part 3d):

- **Claude's changes.** `ChatGate` (`chat.h`; `EditorState::chat_gate`)
  holds the person's setting `asks` (the Chat panel's box; on with a
  window, off with `--hidden`, where no one could answer and whoever drives
  the editor has every command anyway; `--chat-asks` forces it on for the
  tests of the waiting). While it asks, `run_command` for a command that is
  not read-only checks the arguments against the schema (the person is not
  asked about what could not run), then holds the command instead of
  running it: `run_chat_tool` answers `held`. The panels show the question
  (`draw_questions`: the command, its description's start, the arguments up
  to 16,000 bytes with the count of what is left out). The buttons call
  `ChatGate::answer`; nothing runs there. `finish_held_chat_tool` then runs
  the command if it was allowed, exactly as it would have run, or gives
  Claude "the person did not allow this". "Allow everything for this
  message" lasts until the message's work ends, is stopped or cleared, or
  another is sent (`ChatGate::chat_asked` and `chat_updated`, which the
  bridge calls). Read-only commands, searching and describing never ask.
  `load_scripts` is let through only when it would do nothing but ask in
  its own window (`ChatGate::asks_itself`: a file not yet allowed, in an
  editor that asks); where it would load at once (a file allowed before,
  `--allow-scripts-by-command`) the gate asks as for any change.
- **Between the two sides.** The C# conversation still never calls the
  editor: `RunTool` returns 2 for "held", the conversation keeps that one
  call (`_held`) and each frame asks `HeldTool(0)` whether the person has
  answered (an integer compared: no strings cross while it waits), then
  `HeldTool(1)` for the reply (`held_chat_tool`). Nothing else of that message runs
  meanwhile. A held call can be stopped like one still waiting: Stop and
  Clear cancel it, the editor drops the question at once (`ChatBridge::ask`)
  and the conversation confirms on its next turn (`HeldTool(2)`); a stop
  that arrives while the editor is deciding to hold is caught when the
  hold is recorded. Once the person has answered and the main thread has
  taken the call, it runs to its end as any taken call does. `get_chat`'s
  `asking` tells an MCP client what is being asked.
- **Script files.** `load_scripts` (`script_commands.cpp`) loads a file
  only when `EditorState::scripts_consent` is `by_command`
  (`--allow-scripts-by-command`, automated runs), or the file, by where it
  really is on the disk (`script_file_key`: links resolved, so a link
  pointed elsewhere is another file), is in `scripts_allowed`: the files
  the person allowed in this run of the editor (64 at most, the oldest
  asked about again) and the one given with `--scripts`. Otherwise it sets
  `scripts_request` and answers `asked`; `answer_scripts_request`, the
  window's button, loads it. With no window (`none`) it refuses. The list
  is not kept between runs: a build's output is a file that changes, and
  one question per file per session is the price.

One question is shown at a time (a mod's scripts, then a script file,
then Claude's change), in one window helper (`question_window`): it does
not take the keyboard or a click as it appears, and its yes can be pressed
only a moment after it appeared (1.5 s for scripts, 0.5 s for a change),
so a click already on its way is not an answer; a question asked again
after it was answered waits anew.

Known limits: the setting is not kept between runs (it is on at every
start); `capture_screenshot` is read-only, so it is never asked about,
yet writes a picture into the `screenshots` folder; arguments longer than
16,000 bytes are shown from their start only (JSON keys are in alphabetical
order, so a long early field can push later ones out of view: the window
says how much is left out); a script file is allowed by where it is, not
by what it holds; "Allow everything for this message" covers file-writing commands
too; the question shows the arguments Claude gave, not what the command
will do with them (a path is shown as written); what the person allowed
runs against the scene as it is when they answer, not as it was when they
were asked.

#### Pictures (LW6b part 4)

A command's pictures (`CommandResult::images`: a screenshot) go to Claude
with its result. `execute_for_claude` keeps those Claude's service takes
(PNG or JPEG, at most 4, each at most 3.5 MB: the service's limit is 5 MB
as base64) in `ChatToolReply::images` and tells Claude how many it left
out. The bytes do not go through the reply's JSON: the bridge keeps the
last tool's pictures, and the C# side copies each (`ChatApi::tool_image`)
right after the call that ran the tool, the held one's finish included.
The conversation hands a result with pictures back as a list (its text,
then an image block for each); one without stays a string. Every request
carries the whole conversation, so its pictures are bounded
(`Conversation.MostImages` 6, `MostImageBytes` 8 MB): when a new result
goes over, the oldest results that carry pictures are written again as
text with a note that a picture was there (`LetOldPicturesGo`), down to
half of each bound, so that it happens once in several pictures and not
with every one. One result's pictures are held to `MostImageBytes` too
(the later ones are left out and Claude is told), so the newest result
always fits. That rewrites earlier messages, so the service's cache of the
conversation is read only up to the first of them once, each time it
happens; and since a thinking block's signature holds only for the
conversation as it was when Claude thought it, the rewrite takes all
thinking so far out of the conversation as well (`Talk.Unthought`: what
was said and the tools called stay). When the service refuses a request
(`AnthropicApiException`), every picture is taken out the same way: a
picture the service will not take must not refuse each message after it.
A failed command's pictures are not sent. The Chat panel and `get_chat`
say that a picture came, never its bytes. A screenshot's PNG is packed
(`giant/core/png.h`: a filter chosen per row, one deflate block of fixed
codes), which keeps a window-sized one well under the bounds.

#### Replies as they are written (LW6b part 5)

A request asks for the reply as a stream (`CreateStreaming`); the SDK's
own aggregator (`BetaMessageContentAggregator`) makes the whole message of
it, which is handled as before (one without a stop reason, a stream that
ended early, is thrown as broken). Meanwhile each text delta is appended
to the talk's `Writing`, which `get_chat` gives as `writing` and the Chat
panel draws under the lines; a delta moves the stamp, so the panel reads
again only in frames in which words came, and it asks only for the words
it does not hold yet (`writing_id` names the answer, `writing_from` is
how much of it the reader has: a long answer is not written out whole
with every word). The words become the `claude`
line in one step with `writing` being emptied, so nothing shows twice or
is missing for a frame. A declined reply's words are withdrawn (nothing
of it is kept). When the work ends otherwise before the reply is whole (a
stop, a broken connection, an error), what had come stays as a line
ending in " …" and the note after it says Claude will not remember it:
it was never part of what Claude is sent. The panel follows what Claude
adds (the growing reply, and its lines) down only while its view is at
the end; the person's own message always scrolls there.

The key (`Keys`) is read from `ANTHROPIC_API_KEY`, else the first line of
`claude_key.txt` in the person's own settings folder, with every message
sent (so a corrected key is used at once). It is handed to the client
library and nowhere else: not logged, not in `get_chat`, never in the
project. The library sends it to Claude's service, or to the address in
`ANTHROPIC_BASE_URL` when that variable is set (the tests set it, to
their stand-in). The chat is no part of a game: it lives in `managed/chat`
apart from the scripts' API, for game packaging to leave out when there
is such a thing.

Tests: the conversation against a stand-in for the service inside the
process (`editor/tests/managed/ChatTests`: what is sent, tools on the
calling thread, results together, declined and cut replies, stopping,
clearing, the step limit, errors, another model taking over), the tools
and commands without .NET
(`editor/tests/test_chat.cpp`), and the editor end to end against a local
stand-in server (`tools/chat_smoke.py`: no key, the key file, a corrected
key, a command found, described, run and undone, what the service was
sent). None of
them reaches Claude's real service.

The Chat panel (`EditorPanels::draw_chat`, LW6b part 2) keeps its own
copy of the lines (`ChatView`) and reads `get_chat` only when
`EditorState::chat_stamp` moved: the bridge bumps it when the C# side's
stamp (a count of the lines added and the words arrived, and whether
Claude is working, returned by its per-frame update) changes, and on every send, stop and clear, refused
or not (`ChatStamp`; never on a read, or the panel would read every frame).
A frame
in which nothing happened costs the panel one comparison. New lines are
read from the view's cursor, a few pages a frame; lines the conversation
no longer keeps (cleared, or older than its 2,000) leave the view too. It
draws the newest 300 (wrapped text is measured as it is drawn). Sending,
stopping and clearing are the chat's commands.

### Mods (LW7 part 1)

A mod is a folder beside a world, `<world>/mods/<id>/`, with a manifest
`mod.json` (`docs/mods.md` is the guide for people who make them).
`giant/world/mods.h` is the manifest (`world::Mod`: id, name, version,
author, description, `needs` with a least version each, `after`), its
strict text form (`load_mod`, `save_mod`: unknown keys are errors, as for
scenes), the world's list of the mods that are on (`load_enabled_mods`,
`save_enabled_mods`: `<world>/mods/enabled.json`), and the load order
(`order_mods`). The order puts each mod after all it needs and all it asks
to come after, and breaks every other tie by id, so the same mods load the
same way on every machine. A mod that is on and cannot load is left out
with one reason (a need that is missing, off, too old or itself left out;
a circle of mods waiting on each other, each circle named by its own mods;
a second mod of an id): nothing is left out silently, nothing that needs a
mod left out is loaded, and one that only asked to come after it is. What
a mod is told does not depend on the order of the list given.

The editor reads the folders only when asked (`editor/src/mods.h`:
`read_mods`, `set_mod_on`, behind `list_mods` and `enable_mod`), never in a
frame. A folder's name must be its mod's id; folders that are no mod are
listed with why (links to folders among them; names beginning with `.` or
`_` are passed over), and a mods folder that cannot be read through is
said, not shown as empty. Names and descriptions hold no control
characters: they are shown to people and to Claude. A list of enabled mods that cannot be read turns none on
and is never overwritten. At most 256 mods, manifests of at most 64 KB.

A mod's `world.json` (format `giant-mod-world`, LW7 part 2:
`world::ModWorld`, `load_mod_world`) holds places, person looks, routines
and behaviours, read by the same readers as a world's own.
`world::apply_mod_worlds` lays the mods over a world's lists in the load
order: by name, a thing there already is replaced whole, any other added.
Each change is recorded (`ModChange`: added, replaced the world's, or
replaced an earlier mod's, which is a clash), and a mod goes in whole or
not at all: after each mod the lists are checked as a world's are
(`check_people`, `check_behaviours`), and a mod that fails is undone and
given a problem.

A mod's people (part 3) are only added. Their ids are not the mod's to
choose: a mod's file numbers them from 1, and in the world number n of
the mod in slot s is `mod_person_id(s, n)`, from `k_mod_person_base`
(2^30) on, 65,536 ids to a slot. A slot is a mod's for good
(`world::ModsOn::slots` in `enabled.json`, given by `enable_mod` when a
mod is first turned on and kept while it is off), so the same person has
the same id whatever else is on and in whatever order mods load, which
is what a game save needs to find it again. The world's own ids stay
under the base: `check_people` holds `next_person_id` past the world's
own people only and never over the base. `apply_mod_worlds` puts each
mod's people among the world's in id order and refuses the mod, whole,
when it has no slot, when the world already has a person in its range,
or when its people use what is not there. At save, `world_own_people`
leaves out the people mods brought (by id) and names those changed;
`world_own_people_are` answers "anything unsaved?" without copying
65,536 people.

A mod's kits (part 3c) are laid by name like the other things
(`ModThing::kit`): one the world has is replaced whole, so its
placements build the mod's pieces; any other is added. `world_own` tells
the world's own kits at save, the save lists only their meshes
(`open_world` imports the mods' kits' meshes itself), and it is refused
while a world's own entity (of a loaded cell; the others are as their
files have them, saved when this held) or scatter layer places a kit
only a mod brings (a scatter layer placing it would stop the world
loading; an entity would place nothing). When no mod brought a kit, the
scene's kits are the world's as they are, in the scene's order. A
placement's damage is recorded by piece number, so a mod's pieces are
kept out of the world's records: `keep_damage` leaves the broken
placements of a mod's kit as they were (`mods_kits_not_kept` counts
them), and `GameSaves::world_identity` hashes the mods' kits after the
world's own, so a game saved with a mod's kit is told as another world's
without it (and the other way round). Known limit: damage the world kept
on its own kit is shown on the pieces of a mod's kit that replaces it,
by number.

A mod's own files (part 3b) are named `mod:<path in its folder>` in its
`world.json` (a look's or a kit piece's mesh, a look's hair's file, their
materials' maps and skin detail). A mod may be anyone's, so it is not trusted with the rest
of the machine: `check_mod_file_path` holds every path a mod names, its
own or not, inside its folder (no `..`, `.`, empty part, backslash or
colon, so no root, drive or server whose opening alone would tell it of
the player; nothing Windows reads as something else: a part ending in a
dot or space or beginning with a space, a device's name, `< > " | ? *`;
no control character), and `load_mod_world` refuses a file that breaks
it. When a mod that names files of its own is laid, the editor looks
through its folder without following anything and leaves the mod out if
there is a link in it (a link could lead a file, or a glTF's buffer or
image beside it, anywhere), then `resolve_mod_files` turns each into the path the editor opens
(`<world>/mods/<id>/<path>`), so the scene only ever holds ordinary
paths and a mod folder can be copied whole. A path without `mod:` is one
from the folder the game runs in, under the same rules. (A glTF's own
buffers and images were already held to its folder; `gltf_buffer_files`
now leaves out any outside it too, so none is even looked at.) Known
limits: on Linux a world folder with a backslash in its name has its
mods' own files looked for under another spelling and not found (the
editor writes every path with forward slashes); a look or kit a mod brought, kept by `close_world` and saved as a new
world's own, has the resolved path, which breaks if that folder moves.

The editor lays the mods when a world is opened (`lay_mods`, from
`WorldStreaming::open`), over the scene only: `WorldStreaming`'s index
stays what `world.json` holds. So that saving with mods on keeps the
world's files its own, the things mods set are kept as they left them for
as long as the world is open (`ModsLaid`), and `world::world_own` tells
the world's own from the mods' by name: a name a mod set is the mods',
whatever was done to the thing since (the file's own of that name is kept
as it was, and a change to the mods' thing is reported, not saved); every
other name is the world's, as it is now. So what a save writes depends
only on the scene, never on the saves and undos before it, and a mod's
thing can never become the world's by accident. `save_world` writes
those, lists the world's own looks' meshes and not the mods' (`open_world`
imports the mods' looks' meshes itself), refuses when the world's people
would need what only a mod brings (the world must load without its mods),
and `unsaved_settings` compares the world's own so found with the file's
(for kits, the scene's own when no mod brought one, as the save writes).
Laying keeps an undo list per mod instead of copies, and the mods are
laid before the old world is closed. Turning a mod on or
off changes only the list: the scene changes when the world is next
opened (`list_mods` reports `reopen`, also when a mod's `world.json` was
written since), which keeps the undo history honest. Known limits: game
saves do not record which mods were on (only the mods' kits are part of
what tells a save's world); a world closed with `close_world`
keeps the mods' things as plain ones (people among them, under their
mod ids: saved as a new world's own, the same mod then finds its ids
taken and is left out with that reason).

A mod's scripts (part 3d) are a C# assembly its `mod.json` names
(`Mod::scripts`: a `.dll` by a path in its folder, under
`check_mod_file_path`). The script host runs several assemblies side by
side: the one `load_scripts` loaded and those added
(`ScriptHost::add_scripts`, `unload_added_scripts`), each in its own
collectible load context with its own `await` queue, updated and told of
people's events one after another. A script is a program with the
person's rights, which a `world.json` is not, so the editor runs a mod's
scripts only when they were allowed on this machine:
`world::ModScriptsAllowed` (the world's whole folder path and the mod's
id) kept in the editor's settings folder (`mods_scripts_allowed.json`
beside the chat's key), never in the world, so a world from someone else
runs nothing until allowed. `open_world` finds what runs (`mods_scripts`:
the mod loaded and not left out, allowed, no link in its folder, the
assembly there) into `EditorState::mods_scripts` with the world's session,
and the main loop's `ScriptRunner` adds those assemblies while that
opening's world is the scene's (`mods_scripts_key`: the session and a
stamp) and unloads them when it is not (closed, opened again, or the
scene replaced), from a copy of the list, since a script may run
`open_world` as it starts (a load broken off that way is done again the
next frame). `load_scripts` refuses a file in the open world's mods
folder (`in_mods_folder`), so the allowance cannot be walked around
that way. Who may allow is `ScriptsConsent`: in the
editor with its window, `enable_mod` with `scripts: true` only leaves a
request (`EditorState::mods_scripts_request`) and the panels ask the
person, whose button writes the list (`answer_mods_scripts_request`).
One question at a time: a request for another mod while one waits is
refused, so what the person reads is never changed under them, and the
window takes no keyboard focus and enables its Allow button only after a
moment on screen. That answer is on purpose not a
command (the one exception to rule 7): a command may come from the chat's
AI, an MCP client or a script, and a mod's own text could talk an AI into
it. `--allow-mods-scripts-by-command` lets the command allow, for
automated runs; a hidden editor without it allows none. Forbidding
(`scripts: false`, or turning the mod off) needs no asking, writes the
list and takes the mod out of what runs at once
(`forbid_mod_scripts_now` moves the stamp: the rest are loaded again).
Known limits: allowing is by folder and id, not by the assembly's bytes
(a mod once allowed stays allowed when its files change, or when another
world with a mod of that id is put in the same folder); one assembly per
mod; the scripts are loaded on the frame after the world is opened, on
the main thread; mods' scripts added beside the world's own hear the
people's events those have yet to be told, a little from before they
started. `load_scripts` is under the person's say too (LW6b, "The
person's say over Claude's changes" above): a file a command names is
loaded once they allowed it in the window.

Whole text files are read and written by `giant/core/text_files.h`
(written beside and renamed over); game saves keep their own writer, which
also flushes to the disk.

Not yet: Claude's words arriving as they are written
(replies come whole), shortening a long conversation (clear it when
Claude's service says it is too long), and counting the tokens of requests
another model took over (`get_chat` counts the answering model's). A C#
script can still run the commands Claude may not (scripts are the
person's own code).

### Nobody appears close by in view (LW4 part 2)

`PeopleState` is told what the camera sees (`PeopleState::view`, set by
the editor each frame from the camera's turn, its field of view and the
picture's shape: `view_of`; a cone about the way it looks, to the
picture's corner). With it:

- One the camera sees (within the cone and 5 degrees) is given its
  entity only farther off than `embody_distance`, out to
  `view_distance` (200 m, `set_people_settings`); so are those up to 26
  degrees outside the picture, which a turn would show next. Whoever is
  walked or looked towards is there long before it is near.
- While the world runs, one nearer than 0.85 of `embody_distance` is
  given its entity only while unseen (`unseen` counts those waiting to
  be out of sight). Stopped, nobody moves: what appears is an edit's
  doing, and is made at once.
- An entity the camera looks towards (to 34 degrees outside the
  picture) stays until it is `view_distance` and the gap between
  `release_distance` and `embody_distance` away; any other goes past
  `release_distance`, as before, out of sight.
- When what is seen changes at a stroke anyway (a scene taken up, the
  simulation stopped or a game loaded so that everyone was put back,
  the camera put more than 50 m off in a frame, the people turned on,
  a look made or removed, `most` raised), the list is gone through
  once with those near made whether seen or not; a pass in which
  someone near still waited for its ground is not that pass (for 600
  frames at most).
- One whose entity went by an edit to it or its look, by being
  deleted, by `most` being lowered or with its ground is made again
  where it stands, seen or not: that one, nobody else.
- When `most` entities are held, the farthest held gives its up
  (`swapped`) for someone near, if it is beyond `embody_distance`, or
  for someone 20 m nearer than it: those held are the nearest there
  are, so a crowd far down the road never keeps the people beside the
  camera from being there, and those walked towards have theirs before
  they are near. Those held are looked through once a frame and once
  more after each that gave its up, at most `made_a_frame` a frame.
- What those rules let through is counted: `appeared_in_view` and
  `vanished_in_view` are the entities made and taken away close by in
  the camera's view while the world runs (nearer than 0.85 of
  `embody_distance`). The living-world gate flies its town and passes
  only if neither rose.

A person given its entity part way along its trip does not start from
a standstill: `WalkerState::walk_to` takes how it moves already
(`moving`: along the way at its pace), for the first walk of an entity
made within the last second whose trip was under way (it is at its
pace from the first step; from rest it takes three).

Known limits: what stands between is not asked (one behind a wall
counts as seen); a fast turn can show people being made at
`embody_distance` and beyond, two a frame, and the view is the last
frame's, so a turn of over 300 degrees a second can show one being
made near; with tens of thousands of people the list takes up to 64
frames to go through, and one that comes within 0.85 of
`embody_distance` in view before its turn waits to be unseen; a
camera flown at over 3 km/s counts as put somewhere each frame; one made in view is made at
`embody_distance` at the nearest, where it is some 30 pixels tall in a
1080-line picture (0.85 of it for one that crossed before its turn): there is no fade yet (it would need a dither in the
visibility pass, not history); a mirror or water showing what is
behind the camera shows those made there; the clip a walking person
begins with is the matcher's choice for its pace, not a set phase.

### People near the camera (LW1b)

`PeopleState` (`editor/src/people_state.h`) gives the people within
`embody_distance` of the camera (60 m) an entity each, made from the
person's look where the person stands (on the ground there, when the
scene has a ground tile): its mesh, the look's height as its scale,
material, parts' looks, hair and matched clips, no body, and the person's
id in `Entity::person`. Its entity id is the person's own
(`world::person_entity_id`: 2^52 + the person's id), the same every time
the person is given one, and outside the ids the scene hands out, so
looking round a world never changes the `next_id` it saves; the scene
keeps such entities after all others in its list, so the others' places
(what an undo puts back, the order a scene is written in) never depend on
who happened to be near. Past `release_distance` (80 m) the entity is taken
away (those let go together leave the scene's list in one pass). One of
the scene's own entities added while people stand costs the same as
before: the first person's entity goes last and the new one takes its
place. The scene's own ids stop at 2^52 - 1 (files holding more are
refused), people's entities have those from 2^52. So is one whose person was removed or changed, or whose look changed
(it is made again from what is there now), and one in a cell whose ground
is still on its way from the world's files. Each frame it looks at 1024
people (the list is gone through over frames), makes at most 2 entities
and holds at most 256 (those near enough beyond that are counted as
`waiting`); looks are compared one a frame.

Most of a wide world's people are far from the camera, and asking
where each is (its trip found, its place along its way worked out)
every time the list comes round is the larger part of what this costs
with the living gate's 5,000 (measured there: the pass took several
times what the entities held and walked did). So one found more than
16 m beyond where an entity would be made is passed over from then on,
by a compare in a list as long as the people's: until the camera has
moved half of the gap beyond where an entity is made (counted frame to
frame; a camera put elsewhere counts as having moved that far) or the
travel's clock has run long enough for the person to cover the other
half at the fastest anyone was yet sent (`PeopleTravel::fastest`; a
clock run fast shortens the wait as it should). Between the two nobody
can have come within reach. What moves a person at a stroke ends the
wait: its position edited (compared), another run or scene, a person
put somewhere (`PeopleTravel::puts`: a loaded game's people), someone
sent faster than anyone was, the people turned off and on, or entities
made at another distance.
`passed_over` (`set_people_settings`) counts those passed over in the
last frame.

An entity with `person` set is the editor's, not the world's data: scene
text and cell files leave it out (the person is saved), and the world's
streaming does not count it among a cell's entities (no unsaved edit, no
file loaded for it, not unloaded with the cell). It is not in the undo
history, and `set_entity` refuses it (the person and its look are what to
change). One deleted by hand is made again; one an undo brings back when
nobody is held for its person (the person gone since) is taken away (only
the entities changed since the last frame are looked at); another scene (new, loaded, brought back by
undo) starts with none held and any that came with it removed.
`set_people_settings` sets the distances and budgets and reports the
counts; `get_people` names each person's `entity`; `get_entity` its
`person`.

Known limits (the next parts of the plan): people stand where they start
(routes LW2, routines LW3) and are not walkers yet; an entity may be made
or taken away in view (LW4: out of view or behind the fade); the distance
is from where the person starts; with more than `most` near enough, those
earliest in the list are made, not the nearest; an entity stands at the
ground's height as it was when made (ground sculpted under it since does
not move it until it is made again); a person's `seed` changes nothing
yet; a look whose mesh file is not imported draws nothing; places and looks cannot be renamed; the
lists are whole in world.json, not in chunks by cell (10 to 12 MB for 65536
people as the file is written, indented).

## Routes (living-world plan LW2a)

The world's roads and paths are a coarse graph the scene keeps beside the
people (`world::Routes` in `scene.h`, checked by `giant/world/routes.h`):
`RouteNode`s (an id that only rises, a point on the ground; at most 65536,
by rising id) and `RouteLink`s between two of them (the lower id first, a
pair once, by rising pair; a width and a kind, `path` or `road`; walked
both ways; at most 262144). It is for travel farther than the walkable
tiles that are loaded: people far from the camera will move along it by
the clock, people near walk the ground from node to node. Saved in the
scene's text and a world's world.json under `routes` (`next_id`, and a
short list a node `[id, x, y, z]` and a link `[a, b, width, kind]`: the
text gives a number a line, about 25 MB with every node and link there can
be, rewritten whole by a save); only when there is anything, so older
files read and write the same. The world's streaming carries it between the index and the scene and counts a change as an
unsaved setting.

`giant/nav/route.h` finds the way. `RouteGraph` is built once through the
nodes and links: every link an edge from both its ends, filed by node, and
the nodes by the 64 m square they are in for `nearest`. An edge costs its
length times its kind's pace (a road 1, a path 1.3 a metre) plus 3 a metre
risen or fallen (`RouteSettings`). `RouteSearch` is A* over it in bounded
slices (`advance(budget)`: rule 9; the budget counts nodes taken up and
links looked along, and a slice allocates nothing but the way's list when
found: the open list's room is taken when the search is made; costs are
summed in doubles), the straight line at the quickest pace
as its guess, which never overestimates, so the way found is the cheapest.

The commands (`route_commands.cpp`): `create_route` lays a node at each
point and links them in order (a point within `join` metres of a node
there already, or of an earlier point of the call, uses it: roads laid one
after another meet at crossings), `link_route_nodes` / `unlink_route_nodes`,
`set_route_node`, `remove_route_nodes` (with their links), `get_routes`
and `get_route` (from and to a point or a node). An undo step keeps the
nodes and links its command took out and put in, never the lists, and
after one is done or undone no link is left whose end is gone (commands
run outside the history may have removed it), and a step that would leave
more nodes or links than a scene may hold does nothing. Undoing a change
to a node or link such a command has since removed brings it back as it
was before the change (a node without the links it had). A few nodes or
links changed are changed where they are in the lists; a command that
changes nothing makes no step.

Known limits: no links are made from the terrain itself; `get_route` and
`create_route` (to find the nodes its points join) build the graph within
the command, a few milliseconds with every node there can be, and
`get_route` searches whole there too; a search takes its lists' room when
made (three by node, one by link; `RouteSearch::start` begins another in
the same lists); a link is a straight line between its nodes whatever the
ground between.

### People travelling (LW2b part 1)

While the physics simulates, a person sent somewhere (`send_person`: a
place's name or a point) travels there along the routes. The editor's
`PeopleTravel` (`people_travel.h`) keeps one `RouteGraph` for everyone,
built from the scene's routes when a person is sent after they changed (a
command's cost), and one `RouteSearch` whose lists every search reuses.
`send` finds the nodes nearest the person and the goal at once (and says
so if there is none within `within` metres (4096 m for a person already
on the roads, who may be far along a long stretch), or if the two are in parts
of the roads nothing joins: `build` floods the graph once to number its
joined-up parts, so no search is spent on a goal that cannot be reached);
the way between them is
looked for over frames, one search at a time, `search_work_a_frame`
(default 4096) of search work a frame in slices of 256, the rest waiting
their turn in order. While any wait, the scene's routes are compared with
those the graph was built from (their numbers each frame, 2048 nodes and
links a frame) and the graph is built again when they differ: an editing
cost, never a running game's. A trip whose node went, or whose ends the
change left in parts nothing joins, fails saying so when its turn comes;
entries of trips stopped or sent again since are passed over without
spending a slice (256 a frame at most).

A trip is the way as points (where the person was, the nodes, the goal;
the first or last node left out when the next is nearer than the road's
own way to it) with the metres to each, a speed, and when it set off.
Where the person is at any time is worked out from the clock (`where`:
the stretch found by halving), so thousands travelling cost nothing a
frame; `looked_a_frame` trips (default 1024) are looked at a frame for
having arrived or their person having gone. One arrived (or stopped:
`stop_person`) stays there: `PeopleTravel` keeps where each person sent
since the simulation started is (a few buckets of that map are looked
through a frame for people since removed). Room for everyone's trip is
taken at the first send of a run and a person's place is made when it is
sent (the command's cost), so a crowd arriving in one frame allocates
nothing; a trip's way is allocated when found, a few a frame at most. The clock is the physics' simulated time
(steps counted), so `step_physics` moves people as it moves bodies and a
paused game holds them; a command brings the clock up to the physics' now
first (`sync`, from the one `clock_of` the frame uses too), so one sent straight after `step_physics` sets off from
then. The scene's `Person` is never changed: stopping
the simulation (another run, or another scene) puts everyone back.

`PeopleState` asks `where` for each person it looks at, so the entity of
one who has travelled is made where it has got to, facing along its way.
While a person has an entity its trip is held (`hold`): it no longer goes
by the clock but as far as the entity has walked.

### Walking the way (LW2b part 2)

Each frame `PeopleState::walk` looks at those it holds (at most 1024). One
on a trip is made a walker (`WalkerState::walk_to` with a path given, so
no search): its feet, the way's points ahead and the point 40 m on
(`k_walk_ahead`), straight between them. Roads are laid on the ground, so
the physics character keeps it there, out of walls and up steps, the
walkers step round each other, and a matched character's animation walks
as any walker's does. When it is within 6 m of that stretch's end the
next is given, before the follower slows (the walker keeps its speed);
16 walks are begun a frame at most. A trip has a serial: one sent
elsewhere while it walks is given its new way at once.

How far it has got is its feet's nearest point on the stretches that
begin within 3 m (`k_walk_reach`) of where it had got to: the one it is
on, and those a cut corner or a short link has it on already. It never
goes back, counts 3 m on a frame at most (a way that passes near itself
is not jumped along), and nothing when 20 m off its way. The trip is told
(`PeopleTravel::walked`), so `where`, `get_people` and the camera's
distance follow the entity. Within 0.3 m of the end along the way (and
3 m of it over the ground), or stopped by its follower on the way's last
stretch within 6 m of the end (others stand there already), the trip is
over where it stands (`arrive`). Only on that last stretch, within 3 m of
the end, does one standing in the way end a walk
(`nav::FollowSettings::pack`, `walk_to`'s `pack`): anywhere else it is
stepped round. A walker stopped anywhere else waits 2 s and is given its
way again.

A walker that failed (stuck four times; after the first it looks for its
way over the walkable map to the stretch's end) waits 5 s and is given
its way again; if its detour took it more than 20 m off the way it has
got no farther and tries the same stretch. (Stuck is judged by how much
less of a walker's path is left after 2 s against what it was steered to
go, not by how far it is from where it was: a way that turns back on
itself is walked through, and one shuffling on the spot is stuck.) One
sent elsewhere meanwhile takes its new way at once. One whose walker was taken (struck down as a ragdoll, its
entity hidden, `stop_walker`) waits 2 s, and none is given to one lying
or hidden. People leave the last 64 of the 512 walkers for `walk_to` and
for those getting up; one with no walker to be had stands and tries again
2 s on.

Whenever a walking person stops being a walker with its entity kept (it
arrived, `stop_person`), the scene's entity is put where the walker stood
(`stand`: `Scene::modify`, a change of that one entity), so nothing jumps
back to where the entity was made. An entity let go (the camera left) has
its walker stopped at once, since the next entity made for that person
has the same id; the trip then goes on by the clock from as far as it
walked. Stopping the simulation lets every walker go and puts everyone
back: an entity whose person was elsewhere, or that was put where it
walked to, is taken away and made again (`PeopleTravel::resets` tells),
the rest stay. Game saves leave out the walkers and the ragdolls of
people's entities, and a load places none an older save names (the
people themselves are kept: "Game saves keep the people").

Known limits (the next parts): the way between points is walked straight
(a road through a wall stops its walker: it looks for a way round to the
stretch's end, which one pressed against a wall often cannot start (no
walkable ground under it), and tries again every few seconds); a trip's height is the straight
line between its nodes for those not seen; one stopped by `stop_person`
is kept on its way's line, up to 20 m from where its entity stands; game
saves do not keep trips or where people have got to; nobody sets off by
itself (routines: LW3); everyone goes at one speed whatever the link's
kind, 20 m/s at most on foot; `send` compares the scene's routes whole
with the graph's (about a millisecond with every node and link there can
be: a counted revision when routines send many a frame); stopping the
simulation frees every trip in that frame.

## The living-world gate (living-world plan LW8)

`tools/living_gate.py` peoples the N3 gate's valley: the town's and the
hamlet's houses each a home, 56 villages down both sides of the valley
(eight houses, a well and a field each), roads joining them all, and
5,000 people with a home and a routine each (farmers and townsfolk:
sleep, work, a midday meal, home again; alarmed, they run home). It
opens that as a world, simulating with the clock running a day in ten
minutes (`simulate` with `clock`: the scene's clock moves on while
simulating as it does in play, a view flag in `EditorState` that
stopping by command forgets and a game save does not keep), and:

- before dawn reads everyone asleep at home;
- flies the N3 flight as the valley wakes and sets off, firing a shot
  in the town's street: 60 fps without micro stutter as the other
  gates; the people's share of the main thread (the frame phases
  `travel`, `senses`, `routines` and `people`, which main marks after
  each of the four updates; their medians summed) under 0.5 ms a frame
  in the flight's worst sample; nobody appearing or going close by in
  view (`appeared_in_view`, `vanished_in_view`: nearer than 51 m), and
  nobody far off giving its entity up for want of room (`swapped`,
  `waiting`);
- reads who of the townsfolk is out as the shot is fired (on their way
  or at the market), and that those run home (told to flee, and on
  their way home or there); calm again they are back at work for the
  morning's reading;
- mid morning reads everyone at work, saves the game, lets the day run
  to the midday meal, loads the save, and reads everyone where they
  were, doing what they did, at the hour it was.

A flight is so many frames and a day so many seconds, so the gate
measures the frames' pace before it flies and sets the clock's speed
for the flight alone so that the flight lasts the hour from 5.8 (at 60
fps about 120 game seconds a second, the day's own being 144): the
valley sets off, the townsfolk are out and the shot falls among them
at any frame rate. Each of the day's readings is taken again until it
holds or its hour is past (those late on their way arrive), and is
judged by routine, so the few townsfolk are not lost among the
farmers. A day in ten minutes is 25 seconds an hour while people walk
by the real clock, so the gate's fields and markets are within about
80 m of home and its people walk at 3 m/s. The clock moves by each
frame's real time and people by the simulation's steps (four a frame
at most), so under 15 frames a second the day runs ahead of them: a
machine that slow fails the midday reading. Off a real GPU
(CI) it runs the same day twice as long with four villages and a
shorter flight, and checks all but the speed and the people's budget.
The gate's own arithmetic (the valley's plan, the checks of the day, of
the people's cost and of a loaded game) is tested without an editor by
`tools/test_living_gate.py`.

What the gates set aside (all of them: `gate_common.py`): a frame held
by the desktop handling the window. Windows runs a window's messages
itself while its title bar is held or it is moved or sized, and the
editor's loop stands still inside `poll_events` until it lets go (five
seconds of one frame on the owner's first run of this gate). The window
says when it was itself handled (`Window::take_handled`: moved, sized,
minimised or restored, focus come or gone, or asked to be drawn again
twice or more in one poll, which SDL does every 10 ms while Windows
holds the messages, so a title bar held still shows), the frame's record keeps it
(`FrameTimes::window`, `window` on a slow frame of `get_engine_info`),
and a frame over 25 ms with that set and half or more of it in the
`events` phase is not one of the flight's: left out of its times, and
said with how long it was (the frames after it, catching up, still
count). It cannot hide the engine's own work, none of which runs in
`events`; it does set aside any stall of the desktop's that comes with
such a sign (a display change, a window taking the focus). One with no
sign still counts, and the report says what `events` means. With the 99th percentile over its limit the report
also tells the frames over that limit which are no hitches (their
phases, and the leg of the flight), whether or not there were hitches.

The trips' share: each trip looked at for having arrived finds its
person by where it was last found in the scene's list
(`Trip::person_at`), not by a search, while nobody is added or removed.

## Crowds (crowds plan)

Only people near the camera have a body: an entity with its own posed
copy of the character, a walker, senses (`PeopleState`). Everyone farther
is not drawn. The crowds stage adds a middle tier, "figures": no entity,
no skeleton, one instance of a baked pose each.

### Baked poses (CR1)

A baked pose is a skinned file held in one pose as a small plain mesh
(`giant/geometry/pose_bake.h`, `editor/src/pose_bakes.h`). Three steps
of plain data, on a worker:

- `simplify_skinned` cuts the file's bind mesh to a few thousand
  triangles once. The simplifier only drops vertices, so each one kept
  has its own place, normal, uv and skin weights, and the small mesh
  poses as the original does; every pose of the file is made from it.
  It works from the file's original triangles (its skinned clusters
  hold the coarser levels too: those of depth 0 groups are taken), with
  morph targets at their rest weights (`apply_morphs`). A file drawn in
  parts is cut part by part, each to its share of the triangles by
  size, with the corners where parts meet (`seams_between`) kept while
  the target can be reached with them; when it cannot (more than 1.25
  times the target left), seams and locks are let go and the smallest
  loose pieces dropped. Shape is what the cut keeps: normals count
  little and uvs not at all (a figure is seen small, by its outline;
  the weights `build_clusters` uses for meshes seen close cost a person
  several times the shape error). The pose's `error` is the
  simplifier's own measure in metres; it grows fast below a mesh's
  natural size, so the default 3,000 triangles is where a person of
  about 11,000 still reads a few millimetres.
- `bake_pose` moves that mesh into the pose: the clip sampled at the
  time, the joints' skin matrices, and each vertex as the renderer's
  skinning has it for a sound mesh (broken ones differ in detail: a
  tangent or normal that vanishes or is not finite) (`skin.slang`: the weighted sum, the normal made
  unit, the tangent kept perpendicular, a mirroring joint turning the
  bitangent, a place that is not finite left at bind). The pose is the
  clip's own, as `play_animation` shows it: a clip that moves its hips
  through model space bakes moved (its `center` says where), unless
  it is asked for in place (CR3, below).
- `build_clusters` makes it a mesh like any other, with its levels of
  detail.

`PoseBakes` keeps the poses (1,024 at most), by file, clip, time to the
millisecond, triangles and whether in place; the same asked for again is the same pose
(a failed one is tried again). A bake is a job on its two workers; the
file's figure cut down to a size is made by the first bake that needs
it (a mutex: the file's other poses wait for it) and kept for the
file's other poses of that size until no pose is of that file any
more. The main thread only hands finished meshes to the renderer,
`uploads_a_frame` (2) a frame and, after a frame's first, no more
than `triangles_a_frame` (8,000) triangles; a command's `wait` is the exception (a
tool's: the calling thread helps bake and everything is handed over at
once). A file imported again has its poses baked again from the new
file; one whose file has lost the clip or is no longer skinned fails
and says why. A file with many small parts can come out over the
triangles asked for (each part gets at least 16, and may stop at 1.25
times its share); `seams_kept` says whether every part reached its
share with its joins to the others in place. Commands: `bake_pose`, `get_baked_poses`,
`clear_baked_poses`. The figures below draw them.

Known limits: a clip's morph channels are not baked; the figure is cut
at bind, so a coarse triangle across an elbow bends as a flat one.

### Figures (CR2)

`PeopleFigures` (`editor/src/people_figures.h`) draws the people who
have no entity, out to `distance` (400 m): each a figure, the instances
of its look's baked pose put where the person is (where its trip has it
by the clock, on the terrain there where the terrain has a height),
with the look's materials and height. That is everyone farther off than
those with entities, and anyone near who has none (over `most` of
those, or waiting to be out of sight). A figure has no entity,
skeleton, walker or senses. A look's people stand in one pose: the
start of `clip` ("idle") of the look's file, its rest pose where the
file has no such clip, cut to `triangles` (1,500). The pose is asked
for once someone of the look is within `distance` (four poses a
frame, standing ones first), and its people are made figures once it
is baked.

A frame's work, bounded: the poses' meshes are taken as they are now
(a pose baked again, or forgotten, takes its figures' meshes with it
that frame: a mesh's id is never drawn after it left the renderer;
main hands finished poses over, then updates the figures, then builds
the frame, and commands run before all three); those given an entity
stop being figures (the entities held, 1,024 at most, are looked up
among the figures); 256 slots are checked for their person having left
the scene; and `scanned_a_frame` (1,024) people of the list are looked
at: one within `distance` with no entity and a pose is made a figure
(`made_a_frame`, 64, at most `most`, 4,096), one drawn and now past
`distance + keep` goes, one elsewhere than it was drawn (or whose
ground changed) is put there. One look of the scene's is compared with
what its figures were made from a frame. Only an edit does more at
once, each a pass over the slots (16,384 at most): a look changed,
`most` lowered. A pose baked (in play too, the first time it is
needed), baked again or forgotten has one pass a frame point the
figures showing it at what they show now, those alone sent again.

Figures keep their slots (the lowest free first, doubled from 256 as
more are needed), and `scene_frame` takes them as `FigureDraws`: the
slots, and a version for each chunk of 64. A chunk whose version
changed is built again whole into a cell of its own, placed among the
scene's cells like any other (as many instances a slot as the most
parts a figure in it has; holes where one draws fewer or nothing, put
where the chunk's first figure is, since the renderer keeps a cell's
instances as floats about its first). A look's instances in a pose are
made once a call by the same code as an animated entity's (the pose's
meshes in place of the file's, each part with its look) and copied to
each of its figures in that pose, so a chunk costs 64 copies and one
coming, going or moving re-sends 64 slots. The textures in use are
gathered again only when a chunk draws with other materials than it
did, not when one in it moved or changed pose. They are marked `render::Instance::moves`
(out of the voxel world; drawn as meshes at every distance), and their
materials are counted like any instance's.

On by default (`set_figure_settings`) since the crowd gate (CR6)
measured what they cost on the owner's machine; the setting is the
view's, saved nowhere, so a packaged game draws them too. Limits: changing
`triangles` or a clip leaves the poses asked for before in `PoseBakes`
(1,024 in all, `bake_pose`'s too) until `clear_baked_poses`; a pose
that could not be had is asked for again only after a file is imported
or the poses are cleared; poses are not cleared when another scene
or world is opened, so a long session over many worlds with other
looks creeps towards the 1,024 (past it a new look's far people are
not drawn: `looks_without`). A game's loading screen waits for the
poses being baked (`poses`), but a look's walk and activity poses are
first asked for when someone of it walks or acts, in play.

### Figures walk (CR3)

A look has up to 49 poses, each asked of `PoseBakes` when someone
within `distance` first needs it (4 a frame; standing ones before any
other, a walk's all together when its first walker sets off, and no
other pose is had while it would take the room kept for a standing
pose of each look that has none yet): standing, the walk's
(`walk_poses`, 8: `walk_clip` at even times through it, baked in
place), and one for each of the look's activities (the clip's start,
or its end for one played once). A figure points at the meshes of the
pose it is to show, or at the standing one's while that is not baked
or cannot be had (no such clip in the file, no room for more poses),
so it is never drawn as nothing for want of a pose; when a pose comes,
goes or is baked again its look's figures are pointed again in one
pass over the slots. `scene_frame` makes a look's instances once a
call for each pose met (by the list of meshes a figure points at).

In place: `PoseBakes` takes out of a pose what the clip has carried
the hips over the ground by its time, as a share of the clip's whole
travel (`PoseBakes::travel`: the hips at its end less at its start,
found by name as motion matching finds them), so the step's sway and
rise stay and the figure stays over its spot. That travel is also the
ground a round of the walk covers: a figure `gone` metres along its
trip shows pose `floor(frac(gone / (round * height)) * walk_poses)`,
so feet keep to the ground at any speed, a trip begun again begins the
round again, and nothing is kept for a walker but its slot. A clip that
goes nowhere (made walking on the spot) is given `stride` for its
round.

Those travelling are kept in a list and put on in turn, `moved_a_frame`
(2,048) a frame: where `PeopleTravel` has the trip by the clock, on the
ground there, facing along the way, in the pose it has come to. Each
chunk one is in is built again that frame (64 slots copied). One whose
trip is over is put where it ended and leaves the list. Whether one
is travelling, at an activity or hidden is seen when the list of
people comes round to it (the same pass that makes and removes
figures): `PeopleRoutines::activity` says what it does and where, and
it counts as there when told to stay, or within the place's radius and
6 m (`PeopleRoutines::there`, with the slack the entities' clips use:
a person shows the same as a figure and as an entity).
One at the `hidden` activity ("sleep") there is no figure.

Limits: a rig whose hips are not found by name is not baked in place
(its people stand as they go); a file imported with clips of another
length leaves the poses of the old times in `PoseBakes` until
`clear_baked_poses`; changing `walk_clip` or `walk_poses` makes
everyone a figure afresh; a figure turns at once at a bend of its way
and changes pose with no blend (far off, eight poses a round); one setting off or
arriving shows it up to a pass of the list late (a second with 65,536
people), jumping to where it is; with more travelling than
`moved_a_frame` each moves every few frames; every frame with one
moving gathers the frame's cells again and re-sends its chunks (with
a thousand walking that is in the crowd gate's frame times, CR6).

### A figure and its entity change places (CR4)

A person in reach of both is a figure or has an entity, never both.
Main updates the entities first (`PeopleState`), then
the figures, then draws: one given an entity stops being a figure in
that frame, and one whose entity was taken away
(`PeopleState::let_go`, the frame's) is made a figure in that frame,
where it is and as it is (walking its trip from where its entity had
walked to, or at what it does), not when the list comes round to it
(`taken_over` counts them). Neither only as anyone may be: one at the
`hidden` activity is not drawn, and one whose look has no standing
pose yet, or with `most` figures kept, waits for the list as before.
Those taken over are made beside `made_a_frame`: as many as were let
go that frame, which is a few as the camera moves and at most every
entity there was (`set_people_settings`'s `most`, 256, at most 1,024)
when the people are turned off or `most` is lowered, each a person
looked up, picked and put. An entity just made shows what its routine
has it do from its first frame: the activity's clip is begun in the
frame it is made (it stood so as a figure), where before it stood a
frame first. Clips are begun a few a frame (eight, two of them on
entities not animated yet, as new ones are), so with more entities
made a frame than that (`made_a_frame` over 2) the rest stand until
their turn, a frame or a few; one whose file is still being read, or
with no room to animate one more, stands until it can.

Where the two change places is `PeopleState`'s rule, unchanged (LW4):
near the camera only while unseen, in view only farther off than
`embody_distance` (60 m, out to `view_distance`, 200 m), given up past
`release_distance` or the kept view. With figures on
what that rule hides is smaller: a person waiting to be out of sight
for its entity, or over `most` entities, is drawn as a figure
meanwhile rather than not at all.

Limits: the pose is not carried over. A walking figure's pose is by
the distance gone and its entity's by the motion matching, so the step
may differ at the change (in view only past 60 m); an entity on a trip
is let go where it walked to, which may be a little to the side of its
way, where its figure stands. The entities' count and distances are as
they were: retuning them for the new tier waits for the crowd gate's
numbers from the owner's PC (CR6). Figures are on by default since
that run (below). `tools/living_gate.py` turns figures off all the
same, to measure what it measured before.

### Variety and what figures cost (CR5)

A look comes in colours: `PersonLook::tints` (at most 16, saved with
the look as `tints`, `set_person_look`) are the base colours its
people have, each person the one its `seed` gives
(`world::person_tint`: the seed modulo their number), in place of the
look's material's (`world::person_material`: what that material
draws, so of a file in parts the first part; the others keep the
file's materials or their own looks). Both tiers ask the same two functions, so a person is
the same colour as a figure and as an entity: `PeopleState::entity_for`
gives the entity that material, and a figure's slot points at it
(`FigureDraw::material`: `PeopleFigures` keeps each look's material in
each of its tints where it stays, since a material's textures are
looked up by where its maps are). `follow_figures` makes a look's
instances once a pose as before and gives a tinted figure's first part
the table's material for its tint, looked up once a call for each (a
look draws with at most 16 materials more).
A look's tints changed puts its figures again in their colours and has
its entities made again, as any change to what a look draws does; a
person's seed changed shows when the list comes round to it.

Several looks were there from CR2 (each look its own poses, up to
1,024 in all). Detail far off needs nothing new: a baked pose is a
cluster mesh like any imported one (`geometry::build_clusters`), so
the renderer's cut draws a figure 300 m off with the few triangles its
size on screen needs, whatever `triangles` the pose was baked with.

Not here: whether figures cast sun shadows. They do, as every
instance does, and nothing in the renderer leaves one instance out of
the shadow pages; what that costs with thousands of them moving the
crowd gate measured on the owner's machine (CR6: the GPU's passes
with the figures and without, `shadows` on and off, and the shadow
pages drawn, waiting and behind): 0.04 ms for 3,000, so there is no
switch.

### The crowd gate (CR6)

`tools/crowd_gate.py` fills a square within the N3 gate's town site
(40 m by 70 m, east of the houses) with 3,000 people: 2,000 standing in
nine blocks of three files, and 1,000 walking the ten lanes beside and
between the blocks. Each lane has a place at either end for each way,
0.4 m to that way's side of its middle; a walker's home is one, and
its behaviour (one a lane and a way: two states, each begun as the
other's trip arrives) has it walk to the place at the other end, home,
and across again without end. They start spread along their lanes, so
the lanes hold a steady stream both ways, the two passing side by
side, whatever the clock does. There are no roads: the trips go
straight. Three looks of five colours each, fifteen in
all.

With the figures turned on and everything else as it is by default
(256 entities, 60 m), it measures from 30 m over the square's edge
what the figures add on the GPU: the timed passes with the figures
and without, with the sun's shadows on and off (four states, three
rounds, medians), so that what of the figures' cost is their shadows
is told apart (what they add with the shadows on less what they add
with them off: the figures cast shadows like anything else, and
whether they should is decided by this number), and the shadow pages
drawn, waiting and behind a frame with them and without. The clock
runs at the world's own pace (a second a second): the sun turns the
quarter degree at which every shadow page is drawn again once a
minute, where a day run fast would have the gate measure that and not
the crowd. Then it flies in from 460 m (beyond where figures
are drawn), down a lane at head height between the two streams and
once round the square up to 80 m, turning from the lane's heading to
the square's middle over the first third of the round.

It passes at 60 fps with the other gates' limits over the flight and
each leg; all but a twentieth of the people drawn from the lane on
with no figure waiting for room; every pose asked for baked (each look's standing one and its walk's)
and a quarter of the walkers or more walking at once;
nobody appearing or going close by in view; the figures under 2 ms on
the GPU and no more shadow pages behind with them than without; the
people's four phases (picking and placing the figures is in `people`;
drawing them is in `render` and shows in the frame times) under 1 ms
of the main thread in the worst sample; and no overflow. The 2 ms and
1 ms are first budgets, set before any run on a real GPU. The first
run (RTX 5090, 2560x1440, 2026-11-20) passed: all 3,000 drawn, the figures
0.30 ms on the GPU (0.04 ms of it their shadows), the people's work
0.67 ms median and 0.75 ms worst, no hitches. On that, figures are on
by default; the entities' count and distances stay as they were.

On CI (the null device) 360 people are put through the same on a
shorter flight, without the speed and the costs. Its arithmetic (the
square's plan, the flight, the checks) is tested by
`tools/test_crowd_gate.py`.

## Sound

`engine/audio` (core only) hears the world. `Mixer` (mixer.h) plays a
sound as taps: copies of it each late by a delay, scaled, through a
one-pole lowpass and panned (equal power) from the side it is heard
from, plus a share into one reverb tail (four damped combs into two
allpasses an ear, Freeverb's lengths) whose length follows the latest
play. It renders on the audio thread; the game thread hands it work
(plays, sounds, the listener) under a lock the audio thread only tries
(if it cannot have it, it renders with what it has), and nothing is
allocated there once its sounds are in. 64 voices of 16 taps at most:
past that the one with the least still to play (its loudest tap not
over, or the tail it still feeds) fades out over 5 ms for a new one no
quieter, else the new one is refused; a play louder than 4 at the ear
is scaled down as a whole (its echoes keep their balance); samples are
bounded to 4, decaying states flush to 0 (no denormals), and a soft
limiter keeps the output under 1. Only a play that feeds the tail sets
its length.

`trace` (propagation.h) works out how a sound made at a point is heard at
the listener through a ray cast the caller gives (the world as sound
meets it: a hit's distance, normal and hardness, 0 to 1). The direct way
is late by its length over 343 m/s, falls as 1 / distance (and 0.5 dB a
100 m the air takes), loses its highs with distance (a lowpass falling to
about 4 kHz at 1 km, 1.5 kHz at 3 km: far shots rumble) and, when the
cast from the listener meets something first, is quieter and duller
(occluded). Echoes are first-order: rays from the source spread evenly
over the sphere (64 by default); each spot they hit that faces the
listener and that the listener can see sends back its diffuse (Lambert)
share, so a big cliff answers loud however far it is; echoes within 12 ms
are merged (one wall, not a flutter: grouped by the first of them) and
the loudest kept; nothing later than 60 s is kept. The tail
follows from the rays too: those that hit nothing are sky (absorbed),
their mean path is the place's size (Eyring's formula gives its length,
0.1 to 8 s) and its level grows with the hardness and the reflected
energy near the listener. Cost: the rays, plus one cast for each that
hits and one for the direct way (at most 513).

`sounds.h` makes sounds until there are recorded ones (a rifle's report,
a supersonic crack's N-wave, a click, a tone) and reads and writes WAV
(16-bit PCM or 32-bit float, any rate, resampled; mixed to mono).
`platform::AudioOutput` opens the speakers (SDL3, stereo float 48 kHz)
and pulls the mix from its own thread; with no device the caller renders
it itself.

The editor's `AudioState` (editor/src/audio_state.h) holds the mixer
(on the speakers when the editor is shown; hidden runs stay silent and
render the mix with the frames), the listener at the camera (the
player's eyes in play), named sounds, and the world as sound meets it:
the physics' ray within 1 km (bodies, kits, the ground made near, but
not characters: the listener stands in the player's capsule; each
surface as hard as it is made: concrete 0.9, wood 0.5, dirt 0.3, leaves
0.1, the ground harder the steeper) and the terrain marched past it
(steps of half the height over the ground, 0.5% to 2% of the distance,
halved onto the surface: a cliff 2 km off answers; the listener is the
camera as each command runs). `play_sound`, `record_audio` (the sound alone into a WAV),
`load_sound`, `stop_sounds`, `get_audio`.

Part 2. A blocked direct way bends over or round what blocks it: from just
off the face where the listener's ray met it, points 1/8 m to 256 m up or
to either side (doubling) are tried until one sees both ends, then the
reach is halved nearer the edge (at most 84 casts), and the shortest found
is heard from there, later by the detour and quieter by Maekawa's screen
(about 5 dB grazing, 13 dB at a 0.3 m detour, 23 dB at 3.4 m, at most 30)
and duller the longer it is (only the lows bend far). It is heard that
way when that is louder than through (`occluded_gain`, 0.02: about 34 dB,
a solid wall's, and dulled to 900 Hz); else, or with no way found, it is
heard through. `EchoSettings::air` counts distances that many times over
for what the air takes: the editor sets it from the weather shown (1 dry,
+1 in full rain, +2 in full snow, +1 in thick fog, at most about 4 in
practice), and snow lying on the ground takes up to 80% of what
upward-facing surfaces give back.

`shot.h` hears a shot's crack. The flight is taken straight along the aim,
slowing by the bullets' quadratic drag (v = v0 e^(-drag s), so a rifle
round stays supersonic for about 1.3 km). A resolved shot's flight ends
where it stopped or glanced off (what it went through does not cut it);
a flying one's where a ray along the aim meets something. The shock left
at each point reaches the listener at its time of flight plus the way
from there over the speed of sound; the first of those (a narrowed search:
the arrival is convex along the flight) is the crack, from where the Mach
angle holds (cos = c / v): before the report, and from upstream of where
the bullet passed. None when that is at either end (behind the shooter,
outside the cone, or past where the flight was cut). It falls as
distance^-3/4 (an N-wave) and with the air. The editor's `fire` (resolved
or not) is heard: the report at the muzzle (loudness 100 x sqrt(energy /
3100 J): a rifle 100, a pistol about 40) and the crack (60 x sqrt(mass /
8 g), muffled when the listener cannot see where it left the flight), both
reported in its result. Past 8 shots in a frame a report is traced with
the direct way only (no echoes, no bending). While simulating, each hit
taken (on_hits, after connect_blows's) knocks where it struck: at most 8
a frame, none past 1 km, each 16 echo rays within 300 m and no bending
(about 33 casts). Known limits: a crest narrower than the march's step
there (about 5 m a km off, near the ground) can be stepped over; echoes
are first order only (no flutter between two walls, no echo of an echo);
diffraction takes one edge (not a way over two ridges) and the crack does
not bend; the flight's drop under gravity is left out of the crack, and a
flying shot's flight is cut at the first thing its aim meets (a thin
board it would go through, or a glance off, are not followed; a resolved
shot is); the far ground past the terrain's tiles is not met; there is no
Doppler, and wind does not carry sound.

The water's sound (water plan W5d). The mixer plays up to 8 sounds round
and round (`Mixer::set_ambience`: a slot's sound, gain, lowpass, the way
it is heard from and how spread round it is), set each frame by the game
and eased on the audio thread over about a quarter of a second (a slot
given another sound, or none, or stopped by `stop_all`, fades the old
one out in about a quarter second, then the new comes in from silence),
and can muffle the whole mix through one lowpass (`set_muffle`, eased
over a tenth of a second). `sounds.h` makes the water's loops (their
ends cross-faded into their starts): little waves lapping (8 s), a river
running (6 s: rushing noise and bubbles), under water (6 s: a low rumble
and a bubble now and then). The editor's `WaterHearing` looks round the
camera each frame, 2 of 16 ways out (all 16 on the first frame and after
a jump of over 8 m: another world, a game loaded; 13 places along each to 48 m, 1 m
apart near and 9 m far, so a pond a few metres across 30 m off can be
missed: it would be faint; the rest kept from earlier frames and heard
again from where the camera is now) through the physics' water
(`PhysicsWorld::water_at`: its index of the bodies, the sea with the
swell now; kept in step with the scene whether simulating or not, so it
is heard while editing) and the terrain (dry where the ground is over
the water: an island): a lake's or the sea's shore where water meets land
along a way (a river's banks looked past), heard by 1 over the distance
to it (2 m at least) and louder with the wind and the body's `waves` (the
sea more), the nearest river each way by its speed ((speed / 2 m/s)^1.5),
and lapping all round when the camera is on the water (not in a room just
under it). `AudioState::set_water` plays them with the muffle in one call
(one lock; slots 0 to 3: lapping, the sea's own lapping
duller, the river, under water) from where they are, dulled by the
weather's air, and under the surface (where the water reaches: not in a
room under it) the whole mix is muffled to 400 Hz with the deep's
rumble. Under is the camera 5 cm below the physics' surface there (the
swell's crest counted where it is now); the picture's under-water view
switches on the renderer's own test, so near the surface in a swell the
two can part by a few centimetres. `stop_sounds` stops what is playing;
the water carries on (perhaps after a brief dip), set again each frame
from the water still there.
`get_audio` reports `ambience`, `muffle` and the `water` heard. Rain on
the water is not heard yet.

Splashes (water plan W5e). The physics notes what goes into water in
each step (`PhysicsWorld::water_entries`, `sim::WaterEntry`): a dynamic
body seen awake and dry the step before (none of its own shape, as
turned, under the surface: 12 bytes a body, by its id's index; one in a room
under the water is not dry) and wet
now, or a character whose feet went under, going down at 1 m/s or more
both by itself and against the surface (`k_entry_speed`: a sea's swell
falls and rises). A body asleep is not seen, so one woken wet, pushed
down, or made under water makes none; riding a falling swell or met by a
rising one, none; a character walking down stairs or a slope into it,
none (its own fall speed counts, not how a step down moved it, nor a
lift or sinking raft it stands on); held or
carried in and let go, none. A body that splashed does not again for
half a second; a sea steeper than seas stand (2 m waves 40 m long) throws
a light raft clear now and then, and it slaps down about once a wave.
Each entry has where it broke the surface (the middle of what is under
so far: a log end first, at that end), its speed down (against the
surface) and across, mass and size. Not yet: kinematic bodies, a
character held or carried in. The editor's `PhysicsState` passes them on
after each step (`on_splashes`), and `AudioState::splashes_from` plays at
most 8 of a step's and 16 a frame, the loudest where the listener is first (energy over
distance squared): a splash (1.2 s: the slap, the spray falling back, the
low thump of the hole closing, bubbles) or, for something small (under
2 kg or 0.25 m), a plop, as loud as 0.25 sqrt(m v^2 / 2) (v^2 the speed
down squared and a quarter of the speed across squared: a 300 kg crate
from 4.5 m about 29, a person off 2 m about 9.9, a stone about 1), with a
knock's echoes (16 rays within 300 m), none past 1 km. `get_audio`
reports `splashes` and the last one's `splash_loudness` and
`splash_plop`. Spray and rings on the water wait for the effects part.

Wetting (water plan W5f). The physics lists what had some of it under
water in each step (`PhysicsWorld::in_water`, `sim::InWater`: dynamic
bodies awake, and characters, with the surface's height over each), and
gives a body's bounds in its own frame (`local_bounds`) and a
character's description (`character_desc`). The editor's `WetThings`
(in `PhysicsState`, after each step) remembers each with how far up it
got wet: a body along the side of it that was up as it went in (a crate
floating level is wet to its waterline; one turned over in the water,
that side no longer up, all over), a character to the water's depth over
its feet. Out of the water it dries over 90 s of play, the line kept
(back in within 9 s of coming out, the line kept too; later, from the
new waterline); a
body asleep is as it was (sunk to the bed or floating in a lake: still
in the water). 256 at most, the driest out of the water making way;
cleared when simulating stops. Each frame every one is a wet decal (a
box in its frame from just under its bottom, or just over a character's
feet so the ground is left out, up to the line with the fade above it,
following it as drawn), after the scene's decals; not the player (no
body to see). The shading pass takes the most of the wet decals over a
pixel and wets it with the rain (the wetter counted, not both), after
the voxel cones' dry roughness is taken (the cones follow the dry
roughness: wetting adds none; `wet_surfaces` off leaves wet decals on).
`get_physics` reports `wet`. Not yet: drips, wet voxel colours,
kinematic bodies; what stands near a wet thing inside its box (the
ground under a crate's corner) is wetted with it.

## Game saves (E4 part 1a)

A game save is what a play made of the world, kept apart from the world:
save_world's files (world.json, cells, kept damage) are never written by
it. `engine/save` (core only) keeps slots; it knows nothing of the world,
only text chunks by section name and by cell.

A slot is a folder. `save.json`, its manifest, names the world it is of
(its name and a 64-bit FNV-1a hash of its index), when it was saved, the
play's own seconds, its generation G, and a chunk file for each section
(`sections/<name>.<G>.json`: the player, the weather) and for each cell
that holds anything (`cells/<x>_<z>.<G>.json`). A big world's save holds
only the cells play reached, and a save after a load or a save writes only
the chunks that changed; the rest are carried by name.

A commit writes its chunks under a generation past every one the folder
holds (so no name a manifest uses is ever written again), keeps the old
manifest as `save.prev.json`, then writes `save.json` beside and renames
it over: the rename is the commit. A crash at any point leaves the last
whole save (with only partial or unnamed files more). Each file is
flushed to the disk before it is renamed (fsync, FlushFileBuffers and a
write-through move), and the folders after, so a power loss cannot leave
a manifest naming chunks the disk never got. The manifest is read back
before anything is written: a commit never writes one its own loader
refuses. Opening falls back to `save.prev.json` when `save.json` is
missing or bad, but never past a newer engine's save, which is not
written over either. After the commit,
files neither manifest names are cleaned away, and so are partial files.
A base older than the slot (a stale writer) is refused, and so is a base
from another slot (its files are not there). The manifest is strict:
unknown keys, a newer format, numbers that are not whole or not in
range, bad names (lowercase letters, digits, '-' and '_', so folders that
ignore case cannot make two one, and no Windows device names), and file
names outside the
slot's folders or naming another cell are errors, so a save from
elsewhere can never reach outside its folder. Commits, opens and reads
block on the disk: the caller runs them on a job (rule 9). Limits: 2^20
cells and 256 sections a slot, 64 MiB a chunk.

The world's side is the game's damage layer (`Scene::set_game_damage`,
`game_damage_in`, `clear_game_damage`, `shown_damage_in`): damage by cell,
shown over the kept damage (a cell the game holds is the game's, whole),
never saved with the scene or a world, copied and moved with the scene.
`kept_kit` finds the game's records first, then the kept records of cells
the game holds nothing of; the forest's feed and its follower read
`shown_damage_in`, so a game's felled trees lie and its broken buildings
stand broken exactly as kept damage does, within the same budgets.
Holding or freeing a cell marks its kept records' placements changed too
(they are hidden or shown again). An empty cell is not held: a game breaks
more than the world kept, never less. Streaming is untouched: it reads and
compares the kept damage only, so a cell with game damage unloads as usual
and the game's stays held. keep_damage and clear_damage are refused while
a game's damage is shown (what the bodies and trees show is not the
world's then); get_physics counts the game's apart (`kept.game`).
`save_cell_damage` / `load_cell_damage` and `save_weather` /
`load_weather` give a cell's damage and the weather as text of their own
(a save's chunks).

Part 1b: the editor's `GameSaves` (editor/src/game_saves.h) keeps slots
beside the open world, in `<world>/saves/<slot>/`; streaming never reads
there. A save gathers at once, as copies, what to keep: by cell, the
damage shown (`PhysicsState::game_damage_now`: the game's layer with what
the bodies broke and the forest felled now, gathered as keep_damage
gathers them, touching nothing; a cell they add to starts from what it
shows, so the world's kept records there are kept with it, and records
whose placement moved since go), and sections `camera` (where it is,
where it looks, and whether play is on) and `weather` (the weather it is
going to, not a blend under way, and the ground now). Its job makes the
text and commits it; a save to the slot last saved or loaded (the same
world session and scene) writes only the cells whose damage changed, and
drops the cells that hold none now; after a failed save, or the base slot
deleted, the next is whole. A world is named by its folder and a hash of
what the records depend on (its kits, scatter and ground samples, not its
lighting, weather, cells or next id), so a save of a world whose kits or
scatter changed is refused unless forced. A load's job opens the slot and
reads and checks every chunk (cells must hold only what is in them); then
play and simulating stop (what they broke since goes), the game's damage
is cleared and the save's set, 64 cells a frame, and last the camera
(where it was and looked; the fly speed stays), the weather (shown over
the scene's own until set_weather, clear_weather, an undo of them, or
close_game) and its ground come back, and play starts again if it was
on. One save, load or delete runs at a time, each on the job; a result
begun for another scene or world session is dropped, and a new scene or
another world forgets the game (and its play seconds).

Part 2a: the ground a game dented. A save's cell chunk also holds, as
`heights` (rows of exact text, as terrain tiles have them), the whole tile
of each cell whose ground a game shows dented (`PhysicsState::ground_now`:
a simulation's dents and a loaded game's ground). Loading presses each in
(`PhysicsState::press_game_ground`, a second `GroundDamage` under the
simulation's: its tiles as authored kept aside), at most 8 tiles a frame
before that cell's damage; a tile not in the scene yet waits and is pressed
in as it streams in (256 waiting looked at a frame). Dents simulated over
the game's ground go back to it on stopping. `GroundAsSaved` lifts both
layers (dents first) for every write of the world (save_world, save_scene,
terrain edits and their undo), and nests; a tile edited otherwise since
(a sculpt, an undo) is the world's again and leaves the game.
`close_game`, another load, a new scene or world put the ground back as
authored with its versions. keep_damage and clear_damage are refused while
a game's ground is shown or waiting. Known limits: a game tile keeps its
cell loaded, and its far chunk's saved tiles with it (as dents do while
simulating, but for the whole game); get_world counts game tiles as
unsaved cells (save_world and open_world's check do not); each terrain
edit lifts and puts back every game and dented tile; a forced load of a world with other ground
samples skips its ground (`skipped`); a tile the world no longer has waits
until close_game; ground_now copies each dented tile on the frame.

Part 2b: the bodies and autosave. A section `bodies` holds whether it was
simulating and, while simulating, each dynamic entity the simulation
moved (`entity`, `position`, `rotation`) with where the scene had it
(`of`): stale poses (an entity edited since) are not saved, and a
loaded game's not put back yet are saved as they were. Loading simulates
again if it was (or plays), then puts them back with
`sim::ScenePhysics::place` (the body there at rest, posed), 4096 a frame
round-robin over 8192 looked at, once no bodies wait to be made (at most
120 frames held so: a body knocked about meanwhile is still put where the
save had it); one whose
entity is not in the scene yet, has no body yet, or whose ground (the
tile, or a game's still waiting) is not in yet waits, one moved in the world since (its transform not
`of`) or not dynamic any more is skipped, and all of them go if that
simulation stops. Autosave: while playing, every `autosave_seconds` of
play (300; set_autosave, 0 off), a save to slot `autosave`, not while a
save or load is under way or the player is being put down;
bodies still waiting are saved as they were; it has its own base (a save to
the user's slot after it is still of changes), it shows in get_save's
`slot`, `doing` and the last save's numbers as any save, its errors are `autosave_error` (apart from `error`), and each
save's gathering on the frame is timed (`gather_ms`). Known limits: a
waiting body's entity that never streams in (or was deleted from the world
since) waits until the simulation stops, counted in `bodies_waiting`; past 4096 bodies a frame, those put back
first may land among those not yet; a body fallen past 1e8 m is not
saved; a save made playing loads playing, and stopping play then stops
simulating (whether it was on before play comes with the player, 2c); autosave's gathering is the same as a save's (every game cell,
every dented tile copied on the frame).

Part 2c: the player. While playing and put down, a section `player` holds
its `feet`, `velocity`, `climbing` (the wall's normal, or null; not while
pulling itself up a ledge), `carried_by` (an entity, or null), and what
stopping goes back to (`returns_to`, the camera, and `was_simulating`).
Loading starts play with `PlayState::start(..., PlayerStart*)`: held as
play starting is until what it stands on is in (at its feet, not the
camera), then put there (`set_character_position`), moving as it was
(`sim::PhysicsWorld::set_character_velocity`), on its wall again
(`take_hold`: as walking into it, the wall it faces out of) or carried
again (the carrier's body, if made by then; a carrier whose saved pose is
put back later carries it again from its saved feet once it is); stopping
play after a load goes back where the saved game's play began. A save made
while a load's player is still being put down holds that load's player. Known limits: a carrier whose body is not made yet is
not taken hold of again; a jump asked for, the eased eye height and the
input held (keys, set_player_input) are not saved.

Part 2d: the walkers. While simulating, a section `walkers` holds each
walker's entity, `feet`, `yaw`, `velocity` (its character's), `target`,
`speed`, `phase` (searching, walking, arrived, failed), `reason` and `of`
(where the scene had its entity: one moved in the world since is not this
save's). Loading lets every walker go with the simulation, then, once
simulating again, makes each again where its feet were
(`WalkerState::resume`), at most `walkers_per_frame` (64) a frame and 1024
looked at, each once its entity has been in the scene a frame (its cell's
bodies and ground seen by the physics), the ground under its feet is in
(a game's too) and no bodies are being made or put back (that held at
most 120 frames of it being in). A walker that was searching or walking looks for
its way again from there; one arrived or failed stands there as it was.
One given a body, hidden, moved in the world or sent walking since is
skipped; a section naming one twice is refused. Known limits: a walker
whose feet stand on terrain not streamed in at all is not held for it (as
walking is not); the path it was following is not saved (looked for
again: it may take another way round), nor how often it was stuck.

Known limits: inventories and the clock are not saved (neither is in the editor yet); damage
in a cell that streamed out while playing is not in the save (the
simulation lets it go there too); a save gathers every cell the forest
knows at once (a frame's cost grows with the cells loaded); list_saves
reads each slot's manifest on the frame.

## Game UI (E5 parts 1 to 4)

The game's own UI, apart from the editor's ImGui panels: `engine/ui`
(core, rhi; nlohmann_json privately). Part 1: fonts, text and the canvas.
Part 2: screens from data. Part 3: HUD pieces. Next: menus (pause, save
and load, settings), which also route the real mouse and keys to screens.

- **Fonts** (`ui::Font`): TrueType bytes (stb_truetype, the copy Dear
  ImGui ships, compiled static in `font.cpp`) made into signed distance
  glyphs, packed (stb_rect_pack) into one RGBA8 atlas: white, the alpha a
  distance, 0.5 on the outline, `spread` pixels (8 at the 40 px it is made
  at) to 0 or 1. Basic Latin, Latin-1, Latin Extended-A and a few symbols
  (dashes, quotes, bullet, ellipsis, euro, arrows, minus, times): 330
  glyphs of the default font, 1024 x 512, about 150 ms, once, when it is
  loaded (never mid-frame). A character it lacks draws as '?'. Kerning:
  the font's kern table between the glyphs held, plus GPOS between Basic
  Latin's. A 4 x 4 solid block in the atlas lets rectangles share the
  text's draw call. The default font is Roboto Medium (Apache 2.0, from
  ImGui's misc/fonts), built into the engine (`embed_file.cmake`). Only
  fonts the project ships are loaded: stb_truetype trusts its input.
- **Text** (`ui::measure`, `Canvas::text`): UTF-8 read strictly (bad
  bytes, overlongs and surrogates as U+FFFD), kerned, line breaks, wrap at
  the last space within a width (a word wider than it broken where it
  stops fitting), align left, centre or right, line spacing, outline and
  drop shadow. The baseline is snapped to a pixel.
- **Canvas** (`ui::Canvas`): rectangles, frames, lines, images and text
  in pixels into one `rhi::UiDrawList`; consecutive draws with the same
  texture, shade, outline and clip share a command; a clip stack; at most
  2^18 vertices a frame.
- **RHI**: `UiDrawCommand::shade` (`UiShade::plain` or `sdf`), `outline`
  (distance units, 0 to 0.5) and `outline_color`; ui.slang turns the
  distance into coverage over `fwidth` of it (an edge about a pixel wide
  on screen at any size) and lays the outline under the fill.
- **Editor** (`GameUi`): drawn over the scene, under the panels, while
  playing (`set_game_ui` "always": in the editor too, for screenshots;
  "never"). One UI unit is a pixel at 1080 lines high (scaled by height /
  1080). For now: a crosshair while playing (dark-edged), toasts at the
  bottom ("Saved" or "Autosaved" when a save is written; `show_toast`), a
  "Saving…" or "Loading…" mark while one is under way. `get_game_ui`
  reports vertices, draw calls, build_ms and the font. It is built after
  the frame's commands, so a command's change (set_game_ui, show_toast)
  shows from the next frame: a screenshot wants a frame between.

- **Screens** (`ui::Screen`, part 2): a tree of widgets loaded from JSON
  (the format is in `screen.h`): panels, text, bars, images and buttons.
  Loading is strict: an unknown key, a wrong type, a colour that is not
  `#rrggbb[aa]`, a button with no id or two widgets with one id is
  refused, saying where (`screen.children[2] ("hp"): ...`); at most 4096
  widgets, 32 deep. Laid out (only when the values, size or scale
  changed since it last was) in two passes, both linear in the widgets: sizes up from the leaves (text measured, a row or column the
  sum of its children with gaps and padding, `size` 0 fitting what it
  holds), then places down from the root: by `anchor` (a point of the
  parent), `pivot` (its own point put there) and `offset`, or stacked in a
  `row` or `column` (across which the anchor still says where, and
  `stretch` fills). The root fills the screen. Units are pixels at 1080
  lines (times the scale), so a screen fits any window and aspect. Text
  filled in is at most 4096 bytes a widget and 64 KB a frame across all
  screens (the rest not shown), which bounds a frame's measuring and
  drawing however a screen is written. `visible` names a value (`!name`:
  while it is not); a hidden widget hides all it holds (not drawn,
  clicked or focused, and takes no room in a row or column).
- **Values** (`ui::Values`): numbers, text and flags by name, set by the
  game; text fills `{name}` and `{name:2}` from them (`{{` a brace), a
  bar's `value` and `max` are numbers or names. At most 4096.
- **Input**: `click` and `point` find the top button shown (within any
  clipping parents); `move_focus` goes through the buttons shown in the
  order written, round the ends; `activate` says the focused one. A
  button is drawn in its `hover` colour while pointed at or focused. A
  shown widget with a background takes a click from the screens under it
  (a menu over the HUD).
- **Editor screens** (`GameUi`): named screens (at most 32, 16384 widgets
  in all), drawn by layer between the crosshair and the toasts, filled
  in from `GameUi::values`; images by name from textures game code gives
  (`set_image`; nothing in the editor gives any yet, so image widgets
  draw nothing over MCP). A screen loaded after the first frame is laid
  out at once, so it is clicked as it will be drawn. Clicks, presses and activations are queued as events
  `{screen, id}` (at most 64; the oldest go) for the game to take. Over
  MCP: `ui_load_screen`, `ui_remove_screen`, `ui_set_values`, `ui_click`,
  `ui_key`, `get_ui_events`, `get_ui_screen` (every widget's rect as laid
  out). In play, the window's mouse and keys reach screens through the
  menus (part 4); otherwise MCP and code click them.

- **HUD pieces** (part 3, `hud.h`): `ui::View` (the eye: position,
  orientation, fov) and `project` (world to screen pixels, doubles to the
  camera); headings and bearings in degrees clockwise from north (north
  -Z, east +X). `ui::Markers` (at most 256): a diamond, label and
  distance at a point in the world; off the screen or behind, clamped 48
  units inside the edge with an arrow the way (unless `clamp` is off);
  hidden past `max_distance`, fading over its last tenth. A `compass`
  widget draws the heading (its `value`), ticks every 15 degrees,
  letters at 45, and the markers by bearing (past its `span`: dimmed at
  its edge). Any widget may `fade` in and out over seconds as its
  `visible` changes (`Screen::advance` each frame; it fades in from its
  first frame; a button is clicked once half in; fading out it is drawn
  where it was last laid out, even if the screen resized, not clicked
  but still covering what is under it, its text still counted in the
  budget). Health and stamina bars and interact
  prompts are screens (bars, and text shown by a value). In the editor,
  markers draw under the screens, a whole-screen fade (`fade_to`) over
  them, and each frame (after the camera moved, so markers meet the scene
  drawn) sets `heading` (smooth: show it as `{heading:0}`), `playing`, and
  while playing `player.speed` (to 0.1), `player.on_ground`,
  `player.climbing`. A screen lays out again only when a value its layout
  read (its texts and `visible`s) changed: bars and compasses read theirs
  as they draw, so a compass turning lays nothing out. Widget fades and
  markers stop while the game UI is hidden; the whole-screen fade goes on
  (drawn when it shows again).

- **Menus** (part 4, editor `game_menus.h`): screens made as JSON in code
  (`menu.pause`, `menu.saves`, `menu.settings`, layer 900) and driven by
  values. In play, Esc no longer stops play (`PlayState::escape_stops`
  off): it opens the pause menu (resume, save, load, settings, quit to
  editor). While a menu is open over play, `GameMenus::game_seconds` is 0:
  the frame loop gives that to the player, physics (bullets and dust
  with it), walkers, animation, cloth, swings, weather, scripts and the
  play clock (audio and the UI's own fades keep real time), the mouse is
  released, and the window's input is the menu's: the pointer (points
  times pixels per point) points and clicks, Up/Down/W/S/Tab move the
  focus, Enter or Space presses, Esc goes back or resumes. Keys held when
  a menu opens are let go; a key repeating (`InputEvent::repeat`) does
  not press, toggle the pause menu or jump again; a menu opens with no
  focus. Menus take the window's input only over play and only while the
  game UI is shown (with it hidden, Esc stops play as before). The saves
  menu reads the slot list on a job when it opens ("Reading saves..."
  until it is in): slots newest first, at most 8 rows, the autosave left
  out when saving, a slot of an older world or one that cannot be read
  not loaded; "New save" takes the next `save-N`. A load or save that
  fails on its job, after the menu closed, is a toast. Screens named
  `menu.*` are the menus': MCP cannot load or remove them, clearing all
  screens keeps them, and `get_ui_events` leaves their presses. Settings (UI scale 0.5 to 2, vertical FOV 40 to 110, look
  speed 0.25 to 4, fullscreen, vsync) last while the editor runs (a
  game keeps them: below) and apply at once, but fullscreen and vsync,
  which are a game's: a game takes fullscreen at once and vsync when it
  next starts (the swap chain is made with it), the editor only shows
  them and leaves its own window and frames alone. Only
  the menus' own screens' events are taken; the game's stay queued for
  `get_ui_events`. MCP: `get_menu`, `open_menu`, `set_game_settings`, and
  `ui_click` on the menu screens.
- **Run as a game (N7 part 1).** `giant_editor --game <world folder>` is
  the same program with `EditorState::game_mode` set: the world is opened
  before the first frame (the program ends with an error if it cannot
  be, or if the game UI has no font to draw its menus with), no editor
  panel, camera or brush runs, the game's UI always shows, and the menus
  lead. Outside play the main menu stands (`Menu::main`, screen
  `menu.main`, titled with the world folder's name) and takes the input:
  Continue (the newest save a load would take: of this version of the
  world and of this folder's name, looked for among all the slots when
  the menu opens, on the job; shown when there is one), New game (the
  game closed, `GameSaves::close`, so the world is as authored, and play
  started from the view the program opened with: a world names no player
  start yet), Load, Settings, Quit (`quit_requested`). The pause menu's
  Quit reads "Quit to menu" and leads there, as does play stopped any
  other way; Back and Esc from the saves and the settings lead to the
  main menu (to the pause menu in play), and Esc does nothing on it. A
  game loaded from a menu is watched until it is in (`m_loading`), behind
  the loading screen (below): one saved in play goes on when its world
  is in, one saved outside play is started then,
  and one that fails on its job leads back to the main menu, which says
  why (`notice`, `menu.message`; in play the game goes on and the saves'
  toast says it). The editor has no main menu (asking for it there closes
  the menus). Nobody can be asked a question in a game (the editor's
  question windows are its panels'), so script files and mods' scripts are
  treated as with no window at all: not loaded unless allowed on the
  command line. Not yet: a player start in the world; the game's own
  screens show behind the main menu, and scripts run there too. Tested by
  `editor/tests/test_game_menus.cpp` and end to end by
  `tools/game_smoke.py` (ctest `editor_game`).
- **A game's settings last (N7 part 2).** A game keeps the settings
  menu's values between runs as text (`settings_text`: JSON, a key a
  line, numbers to three places: `ui_scale`, `fov`, `look_sensitivity`, `fullscreen`, `vsync`,
  `window` [width, height] in points), in `game_settings.json` in the
  person's settings folder (%APPDATA%\Giant, or the XDG config folder),
  or the file `--player-settings` names (tests: never the machine's
  own). `main` reads it before the window is made, since the window's
  mode and size and the swap chain's vsync are taken there, and hands
  it to the menus (`GameMenus::keep_in`). A key left out or of the
  wrong kind keeps its default (the wrong ones are logged), values are
  clamped, and a file that is no JSON gives the defaults: a broken file
  never stops the game. A file with anything wrong in it is copied to
  `game_settings.json.bad` before the game writes over it, so what a
  person wrote by hand is not lost; keys the game does not know are not
  written back. A setting changed is written on the menus' job
  (`write_text_file`: beside, then renamed over; no wait on the disk on
  the frame): one write at a time, each the newest text asked for
  (`GameMenus::Keeper`, since a thread waiting on the jobs runs them
  too). As the program ends what is not written yet is, a write that
  failed is tried again, and the window's size goes with it (noted each
  frame while it is a plain window, not fullscreen or maximised, never
  a reason to write by itself; that it was maximised is not kept).
  `--no-vsync` wins for its run without changing the setting:
  `get_menu`'s `vsync_now` and `fullscreen_now` say what the program
  has. Fullscreen is borderless over the
  window's screen at the desktop's resolution
  (`platform::Window::request_fullscreen`): no display mode is changed,
  so there is no resolution to choose. The editor itself keeps none of
  this (`kept_in` empty): its menus' settings last while it runs.
- **A game's loading screen (N7 part 4).** A game does not begin play
  into a world still arriving. `GameMenus::begin_play` (a new game) and
  `GameMenus::load` (a game loaded from a menu, from the moment it is
  asked for, so the world it replaces is never shown half gone) show the
  screen `menu.loading` on a layer of the game UI no screen a game
  loads can have (`GameUi::k_top_layer`; the world's name,
  "Loading...", a bar; opaque from its first frame), and play begins
  when nothing has been coming for `k_quiet_frames` (10) frames in
  which streaming looked at the world (`WorldStreaming::updates`: a
  frame that could not be drawn says nothing of what is to come). What is coming is counted each frame (`GameMenus::Loading`,
  `get_menu`'s `loading`): the saved `game` itself while it is read and
  put in; the `cells` streaming is bringing for where the camera is
  (`WorldStreaming::coming`: within the load radius, or touched by an
  edit, and not read yet, being read, or read and waiting for their
  frame's budget; not one whose file failed); the `textures` waiting to cook, cooking, waiting
  to upload or reading finer levels (`TextureLibrary::pending`); the
  `ground` tiles whose mesh is still to build (not those built and
  waiting for room in the renderer's geometry pool: those are the
  GPU's own, below; a loaded game's dented
  tiles are put in as their cells come and built then; those waiting
  for cells far off, which never come, are not counted); the cells
  whose `scatter` is waiting to be placed or being placed
  (`ScatterStats::building`); the `far_ground` chunks to be read, being read or
  built (the same); the `poses` being baked (`PoseBakes::baking`: those the
  far-off people are drawn in, asked for once someone of a look is
  near); and, once all of those are in and unless the game is playing
  already, the `bodies` the player would stand on or in (entities'
  bodies, ground, trunks: `PlayState::world_waits`), for at most
  `PlayState::k_most_placing_seconds` (5, as play itself waits for
  them), and the GPU's own as the last frame's `RenderStats` say it:
  `voxels` (`voxel_pending`: the far world) and `light` (`gi_waiting`;
  neither while the voxel world is off, whose counts stay as they
  were), `shadows` (`shadow_behind`: pages never drawn or long behind,
  not the backlog of what moves on them, which may never end) and
  `geometry` (cluster groups asked for and not loaded, `waiting`, and
  the ground's tiles waiting for room), for at most `k_most_gpu_seconds` (5, of the clock rather than of
  frames: on a software device a frame takes seconds; a pool too
  small may never let them all be done; play then begins with a line
  in the log saying what was left, and the bar still ends full). (`busy` saves count as `game` too: one being written while
  the screen stands.) So a game starts with ground to stand on
  and with its first cells and textures already in, not arriving over
  the first seconds of play (rule 9). A loaded game that was saved in
  play is playing the moment it is in: the screen stands over play as a
  menu does, so the game is held still (`paused`, and `main` gives the
  frame the load lands in no game time either) and goes on when its
  world is in. The bar is the share of all that was ever to come that
  has come (`progress`, never back: more found to come, as a cell in
  brings its textures, holds it where it is); the last tenth is the
  quiet frames after the first two. After `k_most_loading_seconds` (120) seconds of frames
  (each counted as at most a tenth of a second, so a window put away
  costs none, and not while the game itself is read: a read that never
  returns, as from a drive gone away, holds the screen as it would have
  held the load) play begins
  whatever is still to come, with a warning in the log saying what was
  (a world that cannot come in must not hold the game for ever). Esc
  does not leave the screen; another menu opened by a command, or play
  begun or ended by one, ends a new game's loading (a game being read
  keeps its screen through the play it stops and starts, and gets it
  again over a menu opened meanwhile once it is in: it is to be
  played). `loading.shown` counts the times the screen was put up; the
  other fields keep what they last said once it is gone. Not waited for: the nav tiles, people's
  entities, sounds. Toasts and `fade_to` still draw over
  the screen. No loading screen: the editor (play there begins at once,
  as before), the `play` command, and a game loaded by the `load_game`
  command or a script. Tested by `editor/tests/test_game_menus.cpp`
  (cells that stream in while the screen stands and the bar's way
  there, the longest wait, a cell that cannot be read, a game loaded in
  and outside play) and in `tools/game_smoke.py`.
- **The game gate (N7 part 5).** `tools/game_gate.py` builds the N3
  gate's valley in the editor, saves it and packages it (`package_game`),
  then runs the packaged program from another folder as anyone handed it
  would: New game from the main menu, the loading screen, the player
  running towards the town for 1200 frames (`set_player_input`), the
  game saved, left for the main menu and taken up again with Continue,
  and 1200 frames more, running back the way it came. The frames are counted from just before the
  loading screen went (`Editor.begin_frames`, `read_frames` and
  `end_frames` in `gate_common.py`, which `fly` is made of), so what the
  screen should have waited for shows as hitches at the start of play.
  It passes when the package is whole with nothing of the chat or a key
  in it, each run holds 60 fps without micro stutter (the gates' own
  limits), each loading stood once and ended with its bar full in under
  a minute with nothing it counts still to come, the player ran 50 m or
  more and ended on the ground, the game taken up again is where it was
  saved, and streaming never failed. (A world names no player start
  yet, so a new game begins under the view the program opens with, here
  the river's bed; `PlayState::start` stands a player whose view is
  under the terrain on the ground over it, not left to fall.) It prints how long each loading
  took and what was behind when play began (the voxel world, its light
  and the shadows are waited for by the loading screen for at most
  5 s). On CI
  (null device, optimised builds: ctest `editor_game_gate`) all but the
  speed; its own arithmetic by `tools/test_game_gate.py`.
- **A game as a folder (N7 part 3).** `package_game` writes the open
  world, as saved, as a game into a new folder (`editor/src/game_package.h`;
  never over anything: the folder must not be there, or be empty): the
  program copied under the game's name, the `D3D12` folder and
  `d3d10warp.dll` beside it where they are, `managed/` with the C# API
  alone, `world/` (the world's folder with its mods, without its saves),
  every file the world names at the path it names it by, `scripts/` (the
  scripts' assembly, its `.deps.json` and `.runtimeconfig.json`, the
  libraries beside it) and `game.json` (text: `giant_game` 1, `name`,
  `version`, `world`, `scripts`). The program started with a game's
  `game.json` beside it is that game (`take_packaged_game` in `main`):
  its working folder becomes its own, so the world's paths, which are
  from the working folder, hold wherever it is started from (paths on its
  command line are made whole first: they are from where it was started);
  it opens the world as `--game` does, loads the scripts as `--scripts`
  does, unasked, takes the game's name as its window's title and keeps
  its settings in `games/<name>/game_settings.json` of the person's
  settings folder. `--game` on its command line wins; a `game.json`
  without `"giant_game": 1` is some other program's and is let be; a
  game's with something wrong in it ends the program with an error. As
  it runs the game writes into its own folder: `world/saves/` and
  `giant_cache/` (it must be able to write there). The files a world
  uses are found in its text (`world_files`): every string in its JSON
  files that names a file under the working folder is one, read as the
  program reads a name (`MeshLibrary::normalize`), and a `.gltf` is read
  for the files it names in turn. That needs no list of the fields that
  hold paths, which would fall behind; the price is that a name that
  happens to be a file's path is copied for nothing. A file named by an
  absolute path or one outside the working folder cannot go in at its
  path: the command refuses and names them. A name on another machine
  (`//server/...`) is never looked for: text from a world or a mod must
  not make the editor call out. What scripts load by name as they run is
  not in the world's text: `include` names it. Everything that goes in
  is planned before anything is written, so every refusal leaves nothing
  made: two files for one place (two spellings of a path, on Windows two
  cases; one file twice is once), a file under what would be a file, a
  name that is the package's own (`world`, `managed`, `scripts`, `D3D12`,
  `giant_cache`, a Windows device), a world's JSON that cannot be read.
  Never in it, refused by name wherever found: the chat's library and
  what it brings, and any file named as the chat's key is (names ending
  in a dot or space, or with `~`, are refused too: Windows reads them as
  other files'). Not taken along: the saves, the textures' cache,
  symbols beside the program and the scripts, the editor's own files.
  `notes` says what one might miss: links not followed, a mod's scripts
  (they do not run in a game yet: no one is there to allow them), how
  many files came from the scripts' folder, that scripts need .NET. It
  waits on the disk for as long as the world is large, as `save_world`
  does: a command the person asked for, not while playing, with no
  progress shown yet. Not yet: the textures' cache (a game cooks its
  textures the first time it runs), .NET itself, what a `.glb` names
  outside itself, a `.gltf`'s names written with percent signs, an
  install folder that cannot be written to. Tested by
  `editor/tests/test_game_package.cpp` and end to end by
  `tools/game_smoke.py`, which runs the packaged folder, moved, from
  another working folder.

Known limits: one font, no scripts beyond Latin (no CJK, Arabic or
Cyrillic yet: the atlas would grow, or glyphs be made on demand on a
job); no colour emoji; text shaping is per character (no ligatures or
GPOS beyond Basic Latin's kerning).

## Window events (known limit)

`Window::poll_events` drains SDL's single, process-wide event queue and drops
events for other windows. That is fine while the editor has one window. Before
a second OS window exists (floating panels, a game preview), events move to one
central pump in `platform` that hands each window its own events.

## The shared voxel world

One sparse voxel copy of the world (brick map in camera-centred clipmaps) is
used for far geometry, global illumination, rain and snow occlusion, NPC sight
lines and far-ring destruction. It is owned by `world` (edits) and mirrored on
the GPU by `render` (drawing and lighting).

Done in N2 part 2:

- `engine/voxel` holds the plain-data pieces: bricks of 8x8x8 voxels (an
  occupancy bit per voxel, then colour and normal packed in one u32, then a
  stand-in bit per voxel (part 3); 2176 bytes, the GPU's layout too), a conservative triangle-box test to
  voxelize surfaces without holes, and the clipmap. Level 0 has 0.25 m
  voxels; each of the 11 levels doubles them, so level 10 (256 m voxels)
  reaches about 30 km out. Each level is a window of 32 bricks per axis,
  moved in whole bricks once the camera is more than a brick from its
  centre; a brick's slot in its level's table is its coordinate modulo 32,
  so moving only fills the slabs that came into view.
- `render` builds it (src/voxel_scene.h) from the frame's instances,
  materials and cluster meshes: per brick, each instance over it adds the
  clusters of a LOD cut at half a voxel of error (so a dense mesh voxelizes
  from few triangles at coarse levels), skipping clusters whose bounds miss
  the brick. It keeps its own copy of the instances (bounds from the
  meshes' cluster spheres, resolved colours) in a 32 m grid, and each frame
  compares the frame's instances with it. A moved, changed, added or
  removed instance (one contiguous run removed counts as removals) rebuilds
  the bricks under where it was and is; a recoloured material or an added
  or removed mesh counts as a change to the instances using it. Changes
  wait gathered by 64 m square (a box wider than two squares waits on its
  own): each square becomes one job per level over its boxes' bricks (the
  old boxes' built bricks, every brick under the changed instances; at a
  level whose bricks are 128 m or wider, where the box around the whole
  region is 8 bricks or fewer, each of those once, not once per instance), so thousands of scatter placements arriving are a
  few hundred jobs, not a rebuild. When the renderer's meshes change
  (streaming adds some most frames), only meshes it had found missing are
  looked up again: a removed one is reported by `forget_mesh`, and a mesh
  never changes once added. More than 256 changes in a frame (or an eighth of the
  instances), over 4,096 squares waiting, or any other removal rebuild
  every window, one rebuild at a time (another waits for it rather than
  restarting it, so constant changes still settle); `voxels.rebuilds`
  counts them. Big searches (coarse bricks, whole windows) look through
  the grid's 512 m coarse cells, not every 32 m cell.
- All the work is queues worked through under
  `RenderSettings::voxel_ms_per_frame` (1 ms, at least one step a frame):
  finding bricks in resumable steps (a time check every 256 marks), then
  building, bricks never built first (coarse levels first), then those
  built already whose contents changed, oldest first whatever their level
  (else a 2 km brick that scatter streaming in keeps changing goes back
  before everything nearer, again and again). Bricks are built on worker
  threads (a quarter of the cores, two to four), up to 16 at once: the
  main thread copies what is over a brick (its instances' places, colours,
  meshes and LOD cuts, held so a mesh removed meanwhile stays until the
  build is done; its water's triangles), a few hundred instances between
  looks at the clock, and copies the finished brick into the pool. A
  change over a brick under way builds it again once it is done (waiting
  its turn with the other rebuilds); builds not yet taken by a worker
  whose bricks leave the window are dropped. With a budget of 4 ms or
  more (a game's loading screen gives it 12 ms) the main thread builds
  bricks too (the smallest waiting, in the first half of the budget) and
  otherwise waits on the workers' (yielding) for the rest of the budget,
  which `cpu_ms` counts. Something far smaller than a voxel (its sphere at
  most a quarter of one: a grass tuft at 4 m voxels, a tree at 64 m) is
  added as one point where its middle is, as its triangles would have
  added it (`BrickBuilder::add_point`), not triangle by triangle: by the
  brick its middle is in, its area from its largest scale, its facing its
  triangles' summed (a canopy's shortened, not bent out per triangle).
  Until a brick is rebuilt the old one stays; a slab that just came into
  view reads "unbuilt" and the next coarser level stands in. Bricks live in
  a fixed GPU pool (`RendererDesc::voxel_bricks`, 32768 = 68 MiB) with one
  u32 slot table per level (only the changed range is copied, with no CPU
  wait); bricks that do not fit wait for room (`missing`). `probe_voxels`
  reads a CPU copy of the occupancy. The gate scene (town and 10,000
  trees: 20,000 instances) takes about 7,700 bricks and some 2.3 s of CPU
  time over about 2,100 frames (the coarsest bricks lie under every
  instance); in release builds its frames stay near the
  1 ms budget, at worst a few ms (a search over a whole window, or the
  search over a coarse brick and the water over it, is not split), even with every tree moving each
  frame, except the first (copying the instances and opening every
  window, about 7 ms; a teleport costs the same).
- Known costs left: comparing the instances is a pass over all of them each
  frame (about 0.2 ms for 20,000), and a full rebuild copies them all in one
  frame; world streaming (N3) will hand over changes instead.
- The voxels debug view marches each pixel's ray through the levels,
  skipping empty space with per-level bounds and empty occupancy words.
- Not yet: `world` owning edits (destruction carving it, N4); textures
  (colour is the material's base colour); GI and reflections that read it
  (part 5). (Building on worker threads came later, with big worlds.)

Done in N2 part 3, the far field:

- Past `RenderSettings::far_field_distance` (3 km by default) the voxel
  world stands in for small instances: the CPU leaves out every instance
  whose bounding sphere (radius at most `far_field_max_radius`, 50 m) lies
  wholly beyond the distance and that the voxel world already holds, so
  they cost no culling, streaming or raster work at all. Larger instances
  (terrain, mountains, big buildings) stay meshes at every distance,
  cluster LOD keeping them cheap. `RenderStats::far_culled` counts the
  ones left out.
- Voxels remember what they stand in for: voxels touched by an instance
  of at most `far_field_max_radius` (a stand-in) carry a stand-in bit, and
  take that instance's colour and facing over the big surfaces sharing the
  voxel (a far tree stays green over the ground it stands on). Changing
  the radius rebuilds the voxel world.
- The shading pass (`shaders/resolve.slang`) marches the voxels for every
  pixel whose mesh hit (if any) is past the distance, from the distance up
  to that hit, with the march the voxel view uses
  (`shaders/voxel_march.slang`), seeing only stand-in voxels. So meshes
  still drawn (terrain, mountains, buildings) are never covered or fattened
  by their own voxels, and small things left out show on top of them. The
  voxel's colour and normal are lit like a mesh (a rough dielectric). A
  stand-in voxel is up to a voxel larger than its object, so it can poke
  out in front of a mesh just before it. Where a stand-in is still drawn as
  a mesh (straddling the distance, moving, or not in the voxel world yet)
  the march ignores the voxels within two voxels of its bounding sphere,
  so its own voxels never cover it while others in front of it still
  show (other than ones that close to it: a far tree just before a moving
  car shows the car there).
- The march is kept cheap: levels flag whether they (or a coarser level
  standing in for their unbuilt slots) have stand-in bricks, so with none
  a ray skips the level, and with none anywhere there is no march at all;
  a brick without stand-in voxels is crossed in one step.
- Lit view only, and only while the voxel world is kept up to date (a
  frozen one would show stale far geometry). The levels are written once a
  frame for the far field and the voxel view.
- How sharp it is: each level window is 256 voxels across, so wherever a
  level takes over its voxels are about 7 to 14 pixels wide at 1080p
  (60 degree view). A far tree reads as a blob of a voxel or two; finer
  far detail needs more bricks per axis (`RendererDesc::voxel_clipmap`,
  64 halves the voxel width for some four times the bricks) or the
  proxies of part 5 and N3.
- An instance is left out only once the voxel world holds it as it is
  (`VoxelScene::holds`): every brick under it, on every level, built from
  it, none waiting to be found, rebuilt or for room in a full pool. So a
  scene still building, an instance just added or moved far out, a slab
  just come into view or a full pool keep meshes, never holes. Checking
  costs a few lookups per far instance and frame.
- Not yet: fading between mesh and voxels
  at the distance (the switch can show). A stand-in still drawn as a mesh
  also has its voxels around it (a halo up to a voxel wide against what
  is behind), and one that moves leaves them behind for a few frames until
  rebuilt; something far that changes every frame (a vehicle) is never
  left out and shows both. Stand-ins could drop their bit while they keep
  changing.

## Sun shadows

Done in N2 part 4: virtual shadow maps for the sun (`src/shadow_pages.h`,
`shaders/shadow_pages.slang`), drawn from the same clusters and culling as
the view, so every mesh casts at the detail its shadow texels need.

- The sun's view is a clipmap of 12 square levels in light space, centred
  on the camera: level 0 is 16 m across, each level twice the one before
  (level 11 is about 33 km). Each level is a window of 16 x 16 virtual
  pages of 128 x 128 texels, so texels run from 8 mm near the camera to
  16 m at the edge. A window moves once the camera is more than a page
  from its middle (so a camera bobbing on a page edge does not move it
  back and forth); a page keeps its slot (its coordinate modulo 16) while
  it stays in the window.
- Depth spans 40 km along the light, measured from a fixed anchor near
  the camera, not the camera itself, so a cached page stays right while
  the camera moves; the anchor moves (and every page is drawn again) only
  once the camera is 10 km from it along the light.
- Only pages something on screen needs are kept. Shading picks, per pixel,
  the finest level whose window holds the point with room for the filter
  and whose texels are no finer than the pixel; it asks for that page with
  a bit in a request buffer (and, while the page is not drawn yet, for
  each coarser page it falls back to, until one is drawn, and for the
  level above the one it uses, so a fallback is there when a new sun makes
  pages draw again, coarse levels first). The bits are
  read back two frames later, with the windows they were made in: a slot
  whose page has changed since is dropped. Each page asked for gets room
  in the pool (1024 pages, a 64 MiB atlas of u32 depths,
  `RendererDesc::shadow_pages`, at most 8192). When the pool is full the
  page asked for longest ago makes room, never one asked for this time;
  pages that leave their window are freed.
- A drawn page is cached until what casts onto it changes. Each frame the
  CPU compares the instances with its copy (position, rotation, scale,
  mesh, and whether it is drawn as a mesh this frame: one left to the far
  field casts nothing) and dirties the pages under the old and new bounds
  of each that changed, on every level (a shadow lies under its caster
  across the light). Dirtied boxes are gathered by 32 m square (a wider
  box keeps its own), and many squares at once (over max(256, an eighth
  of the instances)) dirty everything. The copy is kept by index, so removing an instance
  early in the list reads as all the rest moving (everything is redrawn).
- A new sun direction (taken up in steps of a quarter of a degree, so
  time of day redraws now and then, not every frame) or depth anchor
  starts a new view (lights part L1b): light space turns about the
  world's origin, so the old pages do not fit the new grid, but they are
  kept as the previous view (its axes, windows, depths and table, after
  the current's in the same buffers: `ResolveExtras::shadow_prev_*`).
  Shading that finds a level's page of the new view not drawn yet uses
  the previous view's page on that level before a coarser one, so a
  running clock's shadows hand over page by page with no gap or blur.
  The pages used lately are asked for again in the new view at once
  (about the camera's page as before; into free room only), drawn coarse
  levels first; the previous view's pages go back to the pool once none
  of the new view's waits to be drawn for the first time and shading's
  requests in it have come back (3 frames), up to the page budget a frame (coarse
  levels first, as the new view draws them) when shading asks for pages
  and the pool is dry, or after 600 frames. A new view that drew less
  than the previous one holds (two steps close together, a fast clock)
  leaves that one in place.
  Shadows follow the
  stepped sun, so they may lag the lighting's sun by up to a quarter of a
  degree (a 100 m tower's shadow by about 0.4 m).
- Drawing is budgeted: at most `shadow_pages_per_frame` pages a frame (64
  by default, at least one when any wait), drawn pages only if asked for
  in the last 30 frames: pages never drawn first (coarse levels first), then the
  pages dirty longest, so every level gets its turn however often things
  change (a giant walking through dirties pages on every level each
  frame). A dirty page keeps its old content until redrawn, so a moving
  caster's shadow may lag a few frames when many pages wait. Things that
  move all the time (cloth and swinging lanterns in a storm) dirty their
  pages again every frame, so pages always wait there; the stats count as
  `behind` only pages never drawn or dirty over 30 frames, which is what
  the gates wait on (four quiet samples in a row). With the default pool
  (1024 pages) and budget (64 a frame), pages drawn in turn are redrawn
  within 16 frames, so `behind` is in effect pages never drawn, or drawn
  pages starved by a stream of never-drawn ones (the pool thrashing). (The
  N3 gate's street, its banners and lanterns blowing in the storm, always
  had pages waiting on a real GPU, so it never counted as caught up.) Right after
  the camera jumps (or a new scene) pages are missing for the few frames
  their requests take, and shading falls back to coarser ones or lit.
- Per level with pages to draw: the pages are cleared, then culled and
  drawn like the view's first pass (no occlusion) with their own lists:
  the frustum is the pages' rectangle across the light plus a plane half
  the depth range towards the sun; LOD picks clusters by their error in
  shadow texels. The raster pass has no targets (the graphics layer's
  `RenderPassDesc::width`/`height`), is scissored to the rectangle and its
  pixel shader (`raster.slang` `ps_shadow`) keeps, with an atomic max, the
  depth nearest the sun in the page's place in the atlas, only for pages
  being drawn this frame. Both faces cast. Shadow culling marks the
  groups it uses (they stay loaded) but asks streaming for nothing: it
  draws what is loaded.
- Shading offsets the point along its normal by 1.5 texels, adds a depth
  margin of a texel (more where the sun grazes the surface) and filters 3
  x 3 texels with bilinear weights (16 taps; a tap on a page not drawn
  does not count). Points past the far end of the depth range are lit.
  Page requests are made once per wave where the whole wave wants one
  page. Far voxels are shadowed through the same lookup.
- Lit view with GPU culling only (the CPU path and the debug views are
  unshadowed). `RenderStats` (and `last_frame.shadows` over MCP) report
  the pages resident and drawn, those waiting, those asked for that found
  no room (as of the newest requests), and the raster passes. The work is
  timed as "shadows".
- Not yet: casters left to the far field (small things past 3 km) cast
  nothing; finer geometry streaming in does not redraw the pages it falls
  on; a shadow culling list that overflows leaves holes in its pages
  until they are next drawn; no contact-hardening (soft shadows by
  distance) or local lights.

## Global illumination

Light bouncing off the scene, and the sky's light shut out by it, from the
lit voxel world (N2 part 5; `src/voxel_light.h`, `shaders/voxel_light.slang`,
`shaders/voxel_gi.slang`). No ray tracing hardware, no noise to denoise and
nothing carried over from earlier frames, so the image is as sharp moving
as standing still.

- Every filled voxel has a light word in the light pool, a buffer beside
  the brick pool in the same pool indices (one u32 per voxel, 2 KiB a
  brick, 64 MiB for the default 32768 bricks, made on the first frame with
  GI): the bounce and sky light reaching it (RGB, 9-bit mantissas over a
  shared exponent, `voxel::pack_light`) and a bit for whether the sun
  reaches it. A word of 0 means "not lit yet".
- The GPU lights up to `gi_bricks_per_frame` bricks a frame (256 by
  default, at least one; one group per brick, its filled voxels shared out
  evenly). For each filled voxel: a march through the voxel world towards
  the sun (from its own level up) sets the sun bit, and six cones over the
  hemisphere around its normal (one straight out, five at 60 degrees,
  cosine weighted, 30 degree half angle) gather the light. A cone steps
  out through the clipmap, sampling the level whose voxels are about half
  its width (the coarser levels serve as its wider samples), each filled
  voxel taking 85% of what is left; voxels straddling the surface the
  cone leaves (centre less than half a voxel above its plane, taken a
  voxel above the lit voxel's centre: a surface on a voxel boundary fills
  a voxel on each side) are left out, as coarse voxels are fuller than what they hold (the sun march
  leaves them out the same way). What a voxel sends back is lit as
  shading lights a Lambert surface: its colour times the sun over pi
  (when its bit says the sun reaches it) plus its own light word, so
  light bounces once more each time a brick is lit again. What
  gets through comes from the sky (`ambient_light`: the physical sky's
  table, or the sky and ground colours by direction).
- `VoxelLight` picks the bricks on the CPU: a brick just put in a pool
  index is first cleared to "not lit" (what the index held was another
  brick's), then lit before anything else, as is a brick built again in
  place (it keeps its old light meanwhile); the rest are lit again in
  turn, round the pool, with a quarter of the budget always kept for the
  round (so it goes on while the camera flies and new bricks keep
  coming). So a moving sun or a change nearby reaches every brick within
  (bricks / budget) frames, about (4/3 x) that while new bricks keep
  coming (a few hundred frames for a full pool at the default). Turned off
  and on again (or after a debug view without it), the bricks put in the
  pool meanwhile are cleared (the voxel world numbers its placements);
  the rest keep their light until the round reaches them. If a frame's
  clears are lost (out of memory), the next frame clears them again.
- Shading looks the light up per pixel (`voxel_bounce`): on the finest
  level that holds the point, the 2 x 2 x 2 voxels around it (at the
  point itself, so a surface on a voxel boundary gets the same weight
  whichever way it faces), weighted by distance and by how far each faces
  the same way as the surface (a wall's other side, the floor under it, do
  not count). Where none there is lit, the next levels up; over the last
  brick before a window's edge it blends with the next level up, so the
  windows moving show no seams. The light replaces the flat sky and ground
  colours for diffuse, and dims the sky in reflections as much as the sky
  is hidden (for surfaces too rough for traced reflections). With nothing lit around (a
  brick just built, things the voxel world does not hold) it falls back to
  the flat colours. Far voxels use their own light word.
- Lit views (and the voxel view, which then shows voxels with their own
  light) with `voxel_world` on; the debug view `lighting` is the lit view
  in plain white-grey, the light alone. (The voxel view without GI keeps
  its simple, brighter sun term.) `RenderStats` (and
  `last_frame.gi` over MCP) report bricks lit and cleared this frame and
  those new or rebuilt still waiting. Timed as "gi".
- Known limits: the light is only as fine as the voxels (0.25 m near the
  camera, doubling outwards), so small gaps and contact shadows are left
  to later small-scale occlusion; walls thinner than a voxel leak light;
  coarse voxels are fuller than what they hold, so cones far out see a
  little too much blocked; emissive surfaces do not light the voxels yet;
  light lags changes by up to a round of the pool.
- Characters are not in the voxel world (their pose changes every frame,
  which would keep it rebuilding); their bodies shade the light round them
  instead (characters plan C2a, `src/occluders.h`,
  `shaders/occluders.slang`): `FrameInput::occluders` are capsules (a
  segment and a radius, with a colour), of which the nearest 1024 in view
  within 150 m are used. A compute pass after contact shadows (timed
  "character occlusion", only with some in view; the capsules go up in
  view space) lists per 8 x 8 pixel tile those whose reach (a sphere round
  the capsule and 4 radii round it) touches the tile's box of points, in
  their order, so with more than 64 a tile keeps the nearest 64 (the same
  every frame), then per pixel takes each listed capsule as a sphere on its
  axis (nearest the point lifted along its normal by the axis's distance,
  so a leg over the ground shades it from above) and the share of the
  cosine-weighted sky it hides, cos x (r / d)^2, faded over the last
  quarter of the reach, at most 0.9 of it (a point inside a capsule, where
  body parts overlap at a joint or a sunk capsule reaches under the ground,
  is shaded less the deeper it is: (depth from the axis / r)^2). Shading multiplies the ambient diffuse light (and
  the reflections' occlusion) by what is let through and adds half the
  hidden share back in the capsules' colour, as light bounced off them.
  Nothing is kept between frames, so it follows the pose exactly.
  `RenderSettings::character_occlusion`, `RenderStats::occluders`. The
  texture (full size, rgba8) is made the first frame with occluders and
  freed after 600 frames without. The editor (C2b, `character_occluders` in
  `editor/src/hit_targets.h`) hands over every visible skinned entity's
  bone capsules (those bullets meet, posed as drawn, frozen ones in the
  rest pose; walkers and other bodiless characters too), placed where it
  is drawn (moved by physics, walking, swinging, or where it stands, as
  the scene frame draws it), for those within 150 m of the camera, in the
  colour it is drawn in (its base colour times its base colour map's
  average, by its alpha cutoff, once that is ready), each frame the
  setting is on.

## Reflections

Glossy surfaces reflect along the mirror direction, traced every frame
from this frame's data alone (AGENTS.md rule 2: no history), so
reflections are as sharp moving as standing still.

- Voxel cones (`trace_reflection` in `voxel_gi.slang`, with `gi` on),
  in shading (`resolve.slang`): a cone through the lit voxel world along
  the mirror direction, as wide as the highlight (tan of the half angle
  from roughness squared, 0.03 to 0.577). Samples are at least a sixth of
  their distance wide and steps an eighth of it or more, so 48 samples
  reach a hundred metres and more. An empty brick is crossed in one
  step, and a cone rising past the top of every window's filled bricks
  stops there, so cones through open air (the water's, over the sea)
  take a handful of steps, not 48. What they meet shines with its light
  word (as GI's cones do); the rest of the cone takes the hemisphere
  colours, dimmed by GI where the sky is hidden (so no sky indoors). The
  surface's own voxels are left out (a plane half a voxel above it), and
  with them coarse voxels further out holding its plane. Full weight up to
  roughness 0.45, none past 0.6; rougher surfaces keep the dimmed
  hemisphere.
- Screen space (`reflect.slang`, `screen_space_reflections`): shading
  writes radiance before exposure (rgba16 float), and for glossy pixels
  the reflection's weight (Fresnel times the roughness fades, 10:10:10)
  with the normal (octahedral, rg32 uint) and the reflected light it used
  (rgba16 float). A full-screen pass then marches each glossy pixel's
  mirror ray against the depth buffer (32 steps, closer together near the
  surface, out to 200 m, then 6 halvings; surfaces taken to be
  0.3 m + 2% of their distance thick); where it hits a lit surface, that
  pixel's radiance replaces the reflected light shading used. It fades out
  at the screen's edges, for rays coming back at the camera and from
  roughness 0.25 to 0.4 (mirror sharp: no blur yet). It adds them to the
  light before exposure, into a target of its own (`post_draws` counts
  it), timed apart from shading as "screen reflections" (photoreal part
  4d); with screen reflections off, shading writes the light alone
  (`ps_direct`). Either way the post pass then exposes and tone maps.
- Targets: 24 bytes a pixel (about 88 MB at 1440p). `RenderStats::
  reflections` and `screen_reflections` (`last_frame.reflections.voxel`
  and `.screen` over MCP) say which are on.
- Known limits: reflections in mirrors show what shading gave the
  reflected point (a mirror in a mirror shows the voxel reflection, not a
  second sharp one); screen reflections are mirror sharp up to their
  roughness limit; voxel reflections are only as fine as the voxels they
  meet (coarse further out, and they lose the foot of distant things
  standing on the reflecting plane), miss emissive light and leak through
  thin walls; far voxels (the far field) do not reflect.

## Frame budget (1440p, 60 fps, reference GPU, High preset)

Frame pacing counts as much as the average (AGENTS.md rule 9). Performance
gates check the 99th percentile frame and count hitches (frames half again
over the target frame time). Where it stands:

- Done: no shader compiles at run time (shaders are built ahead) and every
  pipeline is created at load (`Renderer::init`, the device's UI pipeline).
  The C# GC runs in background, sustained low latency mode, and the script
  host does not allocate per frame.
- Done in N2 part 1: resources created with initial data inside a frame
  (the streamer growing its usage table, meshes added with
  `Renderer::add_mesh` mid-frame) are staged and copied on the GPU before
  the frame, with no CPU wait; terminal
  cluster groups load within the streamer's per-frame budget like
  everything else (a mesh draws once all of its are in, a few frames after
  many are added at once); the GPU times its passes, so budgets can be
  checked against measurements (`get_engine_info`, the Engine panel).
- Done in photoreal part 4d: the light's extras are timed pass by pass,
  "gi", "ambient occlusion", "contact shadows" and "screen reflections";
  the voxel cones and GI's light looked up in shading stay inside
  "shading" (a pass of their own would read the G-buffer again), and the
  N2 gate (`tools/n2_gate.py`) times shading with and without
  reflections (their share: the cones and writing the screen
  reflections' inputs), then without any of the light's extras (GI's
  light, AO and contact shadows looked up there), and holds the sum under
  4 ms at three stops, printing each part. The N3 gate's weather cost
  adds the screen reflections wet ground brings.
- Known gaps: the 32 MiB per frame streaming budget is set by hand, not yet
  tuned from those GPU timings; what callers create in one frame is not
  budgeted by the device, so a big batch of meshes added mid-frame uploads
  all their metadata (clusters, groups, LOD tree) in that frame (the
  world's cell streaming spreads it, N3); the streamer's zero-filled usage
  table, when it grows, is uploaded outside the streaming budget (rare: on
  doubling, and small); the editor's mesh import runs between frames (its
  buffers no longer wait for the GPU once frames run, see Uploads; a
  texture made there still does), and
  it reads and clusters files on the main thread (editor only today; a
  streamed world imports every mesh it lists when it opens, so cells never
  import while streaming; moving imports onto jobs is still to come).
  The editor's material textures (photoreal part 3a-3c) cook on workers
  under a per-frame upload budget and go up from their tails (part 3b-2),
  finer levels streamed rows at a time; a texture whose sides stop
  halving into multiples of 4 has a larger tail (up to all of it), which
  goes up in the frame it is taken, staged through a buffer made that
  frame if larger than the device's 8 MiB per-frame staging. Cooks are
  capped at 4096 a side for cook memory. Textures are
  asked for when a cell is built, so a cell streamed in draws plain values
  until its textures are ready.
  World streaming (N3 part 3): unloading cells is one `Scene::remove_many`,
  a pass over the scene's entities (a few milliseconds at tens of
  thousands of entities, once every `unload_every` frames that unloads
  anything; entities should stay in the tens of thousands per loaded
  area, with scatter for the rest), and a
  cell joining or leaving relays out the instance list (see "World
  cells"). `save_world` and `open_world` are commands, not frames: they
  wait for reads in flight and read or write files on the main thread.
  Terrain (N3 part 4): `create_terrain` makes its heights between frames
  (spread over the terrain's workers; about half a second for 4096
  hilly tiles of 64 samples, the most a command may make), and a mesh
  that arrives for a cell with nothing else in it re-sorts the cell order
  (every frame of a first build). Terrain streaming (N3 part 4b): each
  tile set or freed costs about 6 to 20 us (64 to 128 samples) within
  the budgets above, and the two mesh workers build about 115 tiles of 64
  samples a second: on opening a world at the default radius the ground
  is all drawn after some 16 s, and flying faster than about 50 m/s at
  that radius outruns them (late full tiles, not stutter: the far ground
  draws until they are in). Far ground (N3 part 4c): about 800 level-1
  and 200 level-2 chunks at the default radii (4 KB of heights each),
  read on a worker and meshed on each level's two workers; what draws is
  worked out for changed chunks only (a level-2 chunk looks at its 64
  children, a level-1 chunk at its 64 cells: hash lookups).
  Physics (N4 part 1): at most four fixed steps a frame on physics' own
  workers (the main thread waits for them). A frame's sync looks at no
  more than 1024 entities, a body counting each of its parts (1024 cubes
  or 25 kit placements of 40 pieces, about a millisecond, made in one
  batch; an eighth kept for dynamic entities, which have a queue of their
  own, so an edit to one never waits behind a stream of fixed ones), and copies out no more than 16
  terrain tiles, whose height fields (0.2 to 0.8 ms each at 64 to 128
  samples) are made on a worker of their own (one thread, kept) and
  swapped in when ready, the old tile holding the ground meanwhile; the
  rest waits for the next frame, so opening a big world builds its bodies
  over a few seconds, not in one frame. A new dynamic entity's body waits
  until no fixed one is waiting and the ground round it is in, so nothing
  falls through. A new scene (new, load, open, and their undo) starts a
  fresh physics world (about 4 ms on the main thread, sized for 262,144
  bodies) and lets the old one's bodies and the follower's tables go on
  threads of their own (tens of milliseconds for 60,000 bodies, off the
  frame; at most two old worlds at once); with the follower's own
  clear-up and first scan that frame takes 10 to 20 ms in all for a
  50,000-entity scene on 4 cores (a scene swap: the renderer rebuilds
  everything that frame too), and the scene's entities are then looked
  at a share a frame. Every body
  that moved rebuilds its cell's instances that frame (as any edit does;
  cost grows with what else is in the cell). Kits are compared whole each
  frame (as the renderer does). `raycast` and `step_physics` finish
  whatever waits first (commands, not frames; `step_physics` of a minute
  runs 3600 steps, about half a second among 50,000 fixed bodies), and
  `get_physics` counts every body (a few milliseconds in a big world).

| GPU work | Budget |
| --- | --- |
| Geometry: culling, clusters, visibility buffer | 3.0 ms |
| Materials, decals, terrain | 3.0 ms |
| Shadows | 2.0 ms |
| GI and reflections | 3.5 ms |
| Weather, clouds, particles | 1.5 ms |
| Water, where it is seen (its pass, its share of shading and the voxel world; from the headroom) | 1.5 ms |
| Hair | 1.0 ms |
| Post and UI | 1.0 ms |
| Headroom for destruction spikes | 0.1 ms (1.6 ms where no water is seen) |
