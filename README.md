# Giant Engine

A custom game engine for one kind of game: a huge, photoreal open world with a sharp image
(no TAA smear, no motion blur) and an AI that can do every job in the editor.

The full design is in the plan doc (v2 tab, "Custom engine plan"). The short
version:

| Part | Choice |
| --- | --- |
| Engine core | C++20, strict house subset (see `docs/coding-rules.md`) |
| Game scripts | C# on .NET 10 (from milestone N0 part 3) |
| Shaders | Slang |
| Graphics | Direct3D 12 first, behind a thin layer so Vulkan can follow |
| First platform | Windows; Linux builds the core and runs all tests |

## Status

Milestone **N0 Foundation**:

- Part 1 (done): layout, build system, rule book, core library (logging,
  asserts, double-precision world math, job system), architecture checker, CI.
- Part 2 (done): SDL3 window, the graphics layer with a Direct3D 12 backend,
  and the editor app that opens a window and renders every frame.
- Part 3 (done): the AI command layer and MCP server (Claude Code can
  drive the editor and take screenshots, see `docs/ai-control.md`), editor
  panels (Dear ImGui, docked), and C# scripting on .NET 10 with reload
  (see `docs/scripting.md`).

Milestone **N1 Virtual geometry** (in progress):

- Part 1 (done): the real graphics layer (bindless, shader model 6.6,
  frames in flight, Slang shaders, Agility SDK), the scene (entities,
  lighting, save and load), a lit forward renderer and a 3D viewport with a
  fly camera (hold the right mouse button, then W A S D). Claude can build,
  light and photograph a scene over MCP alone, which closes N0's gate.
- Part 2 (done): mesh import (glTF 2.0 and GLB) and cluster LOD, the
  Nanite method: every mesh is cut into clusters of up to 128 triangles,
  simplified level by level into a hierarchy, and drawn each frame at the
  detail that keeps the error under a pixel. Try it with the Scene panel's
  "+ Mesh" field, and the Engine panel's View menu (LOD levels) to see it work.
- Part 3 (done): the detail choice moved to the GPU, with frustum and
  two-pass occlusion culling (hidden geometry is skipped), mesh shaders
  (with a vertex shader fallback) and a visibility buffer that shades each
  pixel once. The Engine panel has switches for each and shows what was hidden.
- Part 4 (done): cluster streaming. Geometry lives in a fixed pool of GPU
  memory that holds only the detail the view needs: closer detail loads over
  a few frames as the camera moves (coarser clusters stand in until then),
  and detail nobody has looked at for a while makes room. The Engine panel
  shows how full the pool is.
- Part 5 (done): kits and decals. A kit is a reusable group of pieces (a
  modular wall, a house, a tree with its rocks) placed any number of times;
  editing the kit updates every placement, and every placement shares its
  meshes and materials, so a town of a thousand houses is drawn in the same
  few passes as one. Decals stamp colour and roughness onto whatever is
  under them (mud, scorch marks, cracked ground from a giant's footfall),
  with five shapes for now. Try "+ Decal" and "Selected to kit" in the
  Scene panel.
- Part 6 (done, N1 complete): sharp anti-aliasing and the N1 gate. Jagged
  edges are smoothed from the finished image alone (SMAA), so nothing smears
  or ghosts when the camera moves, and shiny highlights are widened where a
  surface curves faster than a pixel can show, so they do not sparkle. Both
  have switches in the Engine panel. The gate scene, a kit-built town and
  10,000 trees, still draws in the same few passes; `tools/n1_gate.py` flies
  through it and reports whether your GPU holds 60 fps:

      python tools/n1_gate.py giant_editor.exe

Milestone **N2, voxel world and GI** (done in code; gate: a 20 km view that stays sharp while moving, GI and reflections under 4 ms, no hitches):

- Part 1 (done): stutter-free uploads and GPU timing. Buffers and textures
  made inside a frame (streaming's own tables, meshes added mid-frame) no
  longer stop the CPU to wait for the GPU: their data is copied on the GPU
  ahead of the frame that uses them. (The editor's mesh import still runs
  between frames and waits; it moves into the frame with N3.) Streaming
  spreads even the coarsest levels of many newly added meshes over frames
  within its per-frame budget, so adding a forest at once never makes one
  frame upload all of it. The GPU now times its own passes (streaming,
  culling, raster, depth pyramid, shading, anti-aliasing, UI); the Engine panel and
  `get_engine_info` show the times.
- Part 2 (done): the voxel world. The scene is kept as a voxel copy around
  the camera: 0.25 m voxels close up, doubling with each of 11 levels out to
  256 m voxels about 30 km away, built from the meshes and kits (from coarse
  versions of dense meshes far out) and moved with the camera a slab at a
  time. Editing the scene rebuilds only what changed, and the work is
  spread over frames at about 1 ms of CPU each (a few ms on the frame the
  scene loads or the camera teleports); a slab still building shows the
  coarser level meanwhile. `probe_voxels` tells the AI what the voxel
  world holds at a point. The Engine panel's
  "Voxels" view (or `set_render_settings` `debug_view` `voxels`) shows it.
  It is what the far view, lighting and weather of the next parts read.
- Part 3 (done): the far field. Past 3 km (`far_field_distance`), small
  objects such as trees, rocks and props are no longer drawn as meshes at
  all; the voxel world shows them instead, lit the same way, out to about
  30 km. Big objects (terrain, mountains, large buildings) stay meshes at
  every distance. This keeps forests of millions of trees affordable far
  out. Far voxels are coarse (a far tree is a blob of a voxel or two);
  sharper far detail comes with later parts. `last_frame.far_field.culled`
  counts the objects left to the voxels.
- Part 4 (done): sun shadows. Every mesh casts a sharp sun shadow, from
  8 mm detail at your feet to the horizon, drawn from the same detail
  system as the meshes. Shadows are cached: only the patches something on
  screen needs are drawn, and only again when something that casts onto
  them moves, a few dozen patches a frame at most, so a static scene costs
  next to nothing. Far voxels are shadowed too. "Sun shadows" in the Engine
  panel (or `set_render_settings` `shadows`) turns them off;
  `last_frame.shadows` shows the work.
- Part 5a (done): global illumination. Sunlight bounces off the scene and
  the sky's light is shut out where it is hidden (under trees, behind
  walls, in alleys), from the lit voxel world: each voxel works out the
  light reaching it a few hundred bricks a frame, and each pixel looks it
  up, so there is no noise and nothing smeared over frames. The "Lighting
  only" view shows the light alone. "Global illumination" in the Engine
  panel (or `set_render_settings` `gi`) turns it off; `last_frame.gi` shows
  the work.
- Part 5b (done): reflections. Glossy surfaces (wet streets, metal, glass,
  polished floors) reflect what is around them: sharply, from the image
  itself, where what they reflect is on screen, and from the lit voxel
  world everywhere else (blurrier the rougher the surface), with the sky
  past it. Every frame works them out afresh, so nothing smears or lags. "Reflections" and
  "Screen-space reflections" in the Engine panel (or `set_render_settings`
  `reflections`, `screen_space_reflections`) turn them off;
  `last_frame.reflections` shows which were traced.
- Part 6: the N2 gate. The N1 town and forest in a 40 km landscape with wet
  streets, a still lake, hills out to 19 km and villages and woods 4 to 18
  km off. With everything on (voxel world, far field, sun shadows, global
  illumination, reflections) it flies down a street, out over the lake and
  up to 1.5 km, turning to look round the horizon. It passes when the
  flight holds 60 fps without stutter, GI and reflections take under 4 ms
  of GPU time, and the voxel world reaches 20 km:

      python tools/n2_gate.py giant_editor.exe

Milestone **N3, weather, landscape and scatter** (done in code; gate: a storm rolls over an edited valley, and the fly-through has no hitches):

- Part 1: world cells. The world is cut into 64 m squares, and the editor
  and renderer keep their work by square: moving, recolouring or deleting
  something redoes only its square, not the whole world, which is what
  lets a world grow to millions of objects. (`last_frame.cells` shows how
  many squares there are and how many changed.)
- Part 2: objects stay on the GPU. The GPU keeps every object from frame to
  frame, and a frame sends only the squares that changed, plus a few bytes
  a square, instead of every object every frame. Positions are kept
  relative to their square, so they stay exact to well under a millimetre
  anywhere in a huge world.
- Part 3: worlds load from disk around the camera. A world is a folder
  with one small text file per 64 m square (`save_world` makes one from a
  scene, `open_world` opens it). Squares near the camera are read on a
  background thread and join a few thousand objects a frame, and far ones
  are let go, so a world can be far bigger than memory. A square with
  edits you have not saved never unloads, however far you fly, so nothing
  is lost; `save_world` writes only the squares that changed.
- Part 3b: squares keep their place. The list of objects handed to the
  renderer keeps each square where it was as others come and go (a square
  that leaves is a gap the next one of its size fills), so squares
  streaming in and out cost only themselves, not the whole voxel world
  and GPU copy.
- Part 4: terrain. The ground is a height field with one tile per 64 m
  square (`create_terrain` makes flat ground or rolling hills,
  `set_terrain_heights` sets a tile by hand, `get_terrain` samples heights
  so things can stand on the ground). Tiles are built into the same
  detail-on-demand meshes as everything else on a background thread, meet
  their neighbours without cracks at any detail, cast shadows and light
  the scene, and stay real meshes all the way to the horizon. Terrain is
  saved with the scene as text.
- Part 4b: terrain in streamed worlds. Each square's file holds its piece
  of ground, which loads and unloads with the square; a piece you have
  changed stays until saved, like everything else.
- Part 4c: far ground. Past the loaded squares, the ground is drawn from
  coarse copies saved with the world (512 m pieces out to about 8 km,
  4 km pieces out to about 32 km), so it reaches the horizon for little
  memory. Near the camera the full ground takes over piece by piece, with
  no cracks where the two meet.
- Part 5: terrain editor. Sculpt the ground with a brush (raise, lower,
  smooth, flatten): turn on "Sculpt" in the Scene panel's Terrain brush
  section and hold the left mouse button over the viewport (a whole drag
  is one undo). `sculpt_terrain` does the same over MCP, and
  `import_height_map` makes ground from a 16-bit RAW (.r16) or PGM height
  map from tools like World Machine or Gaea.
- Part 6: scatter. Strew trees, rocks, bushes or grass over the ground by
  rules, like Unreal's PCG: `add_scatter_layer` says what to place (shapes,
  meshes or kits, picked by weight, scaled and turned at random, upright or
  along the slope) and where (spacing, density, clumps, an area, the
  ground's height and slope). The placements are worked out from the rules
  and the ground on a worker thread, cell by cell, and follow every sculpt
  and every change to the layer; the Scene panel's Scatter section has the
  main settings.
- Part 7: weather and clouds. `set_weather` gives a scene a sky the sun
  shines through (blue overhead, pale at the horizon, red low in the sky or
  in haze), a layer of volumetric clouds that drift on the wind and shade
  the ground, and height fog. Presets (clear, fair, cloudy, overcast, storm,
  fog) or any mix of cover, density, base, thickness, size, wind, fog and
  haze; `over_seconds` blends from one weather to the next. The Scene
  panel's Weather section has the settings.
- Part 8: rain, snow and wet ground. The weather's `precipitation` falls as
  rain or snow by its `temperature` (sleet between), drawn falling and
  blown by the wind. Rain wets the ground (darker where it is porous,
  glossier, puddles in the hollows of flat ground) and it dries again;
  snow covers what faces up and melts when it is warm. Under a roof, a
  bridge or a tree it stays dry: a map from above made from the voxel world
  says what is sheltered. Presets `rain`, `snow` and `blizzard`;
  `wetness` and `snow_cover` set how the ground starts.
- Part 9: the N3 gate. A 4 km valley edited with the terrain brushes (a
  river cut down it, ridges raised either side, a flat site with a town),
  a forest and stones strewn by scatter rules, saved as a world and
  streamed back from disk. It flies low up the river, down the town's
  street and up 1 km into the clouds while a storm rolls in over 20
  seconds. It passes when the flight holds 60 fps without stutter, the
  weather takes under 1.5 ms of GPU time in the full storm (its share of
  the frame budget), streaming and
  scatter keep up, and the storm arrives with rain and wet ground:

      python tools/n3_gate.py giant_editor.exe

Milestone **N4, first playable slice** (in progress; gate: a growing giantess wrecks a town and a forest at 60 fps):

- Part 1: physics (Jolt). Entities collide as they are drawn (cubes,
  spheres, planes, kits, the ground's tiles); an entity's body is fixed,
  dynamic or none. Simulate (the menu bar, or `simulate` over MCP) lets
  dynamic entities fall and come to rest, follows edits, and puts them
  back when stopped (the scene, its undo and its saves keep where things
  were put); `raycast` finds what a ray meets. Bodies are built a share
  each frame, the ground's in the background, so big worlds open without
  a stall.
- Part 2: play mode. Play (the menu bar, or `play` over MCP) puts a
  player on the ground under the camera and looks through its eyes: the
  mouse looks, W A S D walk, Shift runs, Space jumps, Esc pauses (its menu's Quit stops). It walks
  up steps, stops at walls, slides off slopes too steep, rides what it
  stands on and pushes crates; stopping gives the camera back. Over MCP,
  `set_player_input`, `step_player` and `get_player` drive and read it;
  `travel_player` is fast travel (in a game, behind the loading screen).
- Part 4: breakable buildings. A kit marked breakable (`set_kit_breakable`)
  breaks where it is hit hard enough: the piece struck comes loose, and so
  does everything that stood only on it, so a wall knocked out drops the
  roof it held. A heavy ball, a falling crate or `impact` over MCP (a blow
  at a point, as a giant's foot will strike) does it; stopping simulating
  puts every building back whole, and the scene is never changed.
- Part 5: trees topple. Scatter items marked as trees stand as trunks near
  the camera and fall when struck hard enough, knocking down what they land on.
- Part 6: destruction near and far. Far from the camera, pieces come down at
  once as rubble and trees are laid down, with no cost to the simulation;
  collapses throw up dust clouds that drift with the wind.
- Part 7: craters and footprints. `dent_ground` presses a crater or a
  footprint into the ground while simulating; everything on the ground follows.
- Part 8: damage kept. `keep_damage` makes what simulating broke part of the
  world: broken buildings, felled trees and craters stay, are saved with each
  square's file and come back when the world is opened again. `clear_damage`
  (or undo) takes it away.
- Part 9: climbing and being carried. Walking into a wall, a cliff or a
  building's side, the player climbs it (W up, S down, A and D along it), pulls
  itself up onto the top and pushes off with Space. Anything with a body can
  carry the player (`carry_player`), ready for a giant's hand to pick them up.

Milestone **N5, photoreal characters** (in progress; gate: close-ups hold up beside Unreal reference shots, with no smear):

- Part 1a: skinned characters, the data. A glTF/GLB file with a skin
  imports its skeleton, each vertex's joints and weights and every
  animation clip (a walk cycle); the new `anim` module samples clips,
  cross-fades and blends them, and works out the skinning.
- Part 1b: skinned characters drawn animated. The GPU poses each
  character's vertices when its pose changes and refits its clusters'
  culling spheres, so culling, shadows and shading see the pose as they
  see everything else.
- Part 1c: playing them in the editor. A skinned mesh entity plays a clip
  saved with the scene (`set_animation`: clip, speed, loop), and the AI
  can play, cross-fade and blend clips live, pause and step time
  (`play_animation`, `blend_animations`, `step_animation`,
  `get_animation`, `list_animations`). Up to 64 characters animate at
  once.

Combat for open-world games (general systems any game on the engine can use):

- W1a: ballistics. Bullets fly with drop and drag, go through what their
  energy takes them through (each material's surface takes so many joules a
  metre: wood, glass and flesh let rounds through, concrete and metal stop
  them), glance off hard surfaces at shallow angles, push what they hit and
  wear through the pieces of breakable buildings until they are shot out (`fire`, `get_ballistics`,
  `set_ballistics`; `surface` on materials).
- W1b: hit zones. Every skinned character gets a capsule round each bone,
  fitted to its mesh at import and posed with its animation; bullets meet
  them as flesh and report the bone and its zone (head, neck, torso, arm,
  leg by default, from the bones' names; `get_hit_zones`, `set_hit_zones`).

Open-world systems:

- E1a: wind. The weather's wind blows near the ground at half its speed,
  in gusts that roll over the land in waves, gustier the harder it blows.
  Materials given `sway` bend from their foot along it (trees, bushes,
  grass: further, and rocking faster, in a storm) and `flutter` shakes
  their tips (leaves). It is worked out in the shader as they are drawn, no
  state per thing, so whole forests move; it fades out by
  `wind_distance` (150 m), and the shadow pages under what sways are
  drawn again in turn so the shadows move too (`get_weather` shows the
  wind).
- E1b: cloth. A `cloth` entity hangs a sheet (a flag, banner, curtain,
  washing on a line) from its top edge, its top corners or its left edge;
  the same gusty wind blows it about, pushing on its face with the speed
  squared, so a storm throws it far harder and faster than a breeze, and
  ripples run away from what holds it. The nearest 32 within 60 m move
  (spread over worker threads); farther ones hold still, and calm ones
  sleep. Each is drawn as its own skinned mesh whose joints are its
  points, with its shadow (`get_cloth`, `push_cloth`).
- E1c: swinging things. Any shape or kit with `swing` on (a lantern, a
  sign on two hooks, a bell) hangs from a pivot above it and swings as a
  pendulum in the same gusty wind, pushed on its sides with the speed
  squared: a storm leans it far over and throws it about, a breeze barely
  stirs it. Shots knock it and blasts (`impact`) throw it and the cloth
  near. The nearest 512 within 100 m move and still ones sleep; each is
  moved in place in its cell (never rebuilt) and kept out of the voxel
  world, so a town full of them costs next to nothing (`get_swing`,
  `push_swing`).
- E2a: navigation (`engine/nav`). Where people can walk, as a grid of
  0.5 m samples per 64 m cell built from the terrain, the fixed bodies
  and the scatter trees' trunks (slopes past 40 degrees, walls and low
  ceilings block; kerbs and steps up to 0.4 m are walked up; floors with
  no terrain under them count), built on two workers near what asks and
  rebuilt when what it was built from changes. Paths are searched in
  bounded slices a frame (A*, pulled straight), so a long one never
  stutters a frame (`find_path`, `get_nav`).
- E2b: walkers. An entity sent walking (`walk_to`, while simulating)
  becomes a physics character that follows the way: corners in turn,
  facing where it goes, slowing to stop at the end, stepping to its right
  of anyone coming at it and away from anyone too close, and looking for
  the way again when stuck (`get_walker`, `stop_walker`).
- E3a: sound (`engine/audio`). A sound is heard late by its distance
  over the speed of sound, quieter and duller the farther it is (far
  shots rumble), muffled through what stands between, and echoed by
  what is round it: rays from it meet bodies, kits and the terrain, and
  a cliff 500 m off answers about 3 s later from its side, rock and
  concrete louder than soil and leaves. A reverb tail follows from how
  open, big and hard the place is. The mix runs on its own thread with
  fixed voice and tap budgets (`play_sound`, `record_audio` to a WAV,
  `load_sound`, `get_audio`).
- E3b: shots heard. A shot's report leaves the muzzle like any sound; a
  supersonic round's crack reaches you first, from where its shock cone
  swept over you (upstream of where it passed), and hits knock where
  they strike. Sound bends over walls and ridges (quieter and duller the
  longer the detour), and rain, snow and fog take its highs sooner while
  snow on the ground deadens echoes.
- E4a: game save slots. A save keeps what play made of the world apart
  from the world itself, cell by cell (only the cells play reached, and
  only those that changed since), committed so a crash never loses the
  last save.
- E4b: save_game and load_game. Broken buildings, felled trees, the
  camera, play and the weather are saved beside the open world and shown
  again on loading, without ever touching the world's own files.
- E4 part 2a: craters and footprints are saved with the game and pressed
  back into the ground on loading, even for tiles that stream in later.
- E4 part 2b: bodies knocked about while simulating are saved where they
  lie and put back on loading, and the game autosaves while playing.
- E4 part 2c: the player is saved as they were: where they stood, hung on
  a wall or fell, how fast, and what carried them.
- E4 part 2d: walkers are saved mid-walk and walk on from where they were
  on loading.
- E5 part 1: the game's own UI: sharp text at any size from distance-field
  fonts, a crosshair and toasts over play.
- E5 part 2: game screens from data: panels, text, bars, images and
  buttons written as JSON, laid out at any window size, filled in from
  values the game sets, and clicked or moved through with keys (over MCP
  until the menus part routes the window's mouse and keys).
- E5 part 3: HUD pieces: a compass, markers in the world that point the
  way when off screen, prompts and bars that fade in and out, and fades
  to black.
- E5 part 4: menus over play: Esc pauses the game and opens a pause menu
  (resume, save, load, settings, quit to editor), a saves menu listing
  the world's saves with their date and play time, and settings for the
  UI scale, field of view and look speed, all by mouse, keys or MCP.
- P1 part 1: a physical sky: light scattered many times in the air (a
  deeper sky, the glow after sunset), far land fading blue through the
  clear air out to 32 km, and ambient light taken from the sky itself.
- P1 part 2: auto exposure (the image follows the scene's brightness as
  eyes adapt, without smearing), bloom round bright light, and a choice of
  tone mapper (PBR Neutral or AgX).
- LW1 part 1: the people of a living world as world data: thousands of
  people, the looks they share and the places they go to, kept by the world
  whichever part of it is loaded.
- LW1 part 2: the people near the camera are given real characters made
  from their looks, a few a frame, and lose them again far off.
- LW2 part 1: roads and paths across the whole world as a graph the world
  keeps, laid by command, and the cheapest way found along them.
- LW2 part 2: people sent somewhere travel there along the roads by the
  simulation's clock, thousands at once at no cost a frame, their ways
  found a budget a frame.
- LW2 part 3: those near the camera walk their way over the ground as
  characters, and go on by the clock from where they walked to once out
  of sight.
- LW3 part 1: routines as world data: what the people who have one do at
  each hour of the day and where (their home, a named place, a kind of
  place), made and given to people by command.
- LW3 part 2: people follow their routines while the world runs: each
  sets off at its own time for where its routine says, a village's way
  found once and taken again by the rest.
- LW3 part 3: a person look names the clip its people play for each
  activity (working, eating, sleeping) once they stand where their
  routine has them.
- LW4 part 1: game saves keep the people: where each had got to, where
  it was going and what it had done of its routine, so a loaded game
  goes on with nobody sent twice.
- LW4 part 2: nobody appears or goes close by in view: those looked
  towards get their characters far off, those near only while out of
  sight, and one met part way along its walk is already walking.
- LW5a: people hear: shots, strikes, blasts and splashes spread at the
  speed of sound and alarm whoever they are loud enough for.
- LW5b: people see: the player while playing, and whatever
  `show_to_people` shows, within a distance and a cone about the way
  each faces, with nothing between (a few rays a frame).
- LW5c: the alarmed react: a routine says what its people do over
  each alarm (watch, flee home, hide at a place), and they go back to
  their day when calm.
- LW6a: the People panel: list the people, filter them (text, only
  the alarmed, only those travelling), read what one does and why,
  and have the camera follow one (`follow_person`).
- LW6 part 2: behaviours as data: small state machines (states,
  transitions on alarm, what was sensed, arriving, time) saved with
  the world and given to people (`set_behaviour`).
- LW6 part 3: people act on their behaviours while the world runs:
  each is in a state, moves on when a transition's conditions hold,
  and does what the state says in place of its routine.
- LW6 part 4: scripts hear what people do: C# `OnPersonEvent` when
  one begins a behaviour or changes state, and `get_person_events`
  for the AI.
- LW6 part 5: game saves keep the state of its behaviour each person
  is in, and how long it has been in it.
- LW6b part 1: a chat with Claude inside the editor: say what you want
  in plain words and it does it with the editor's own commands, on the
  undo history like your own work (`chat_send`, `get_chat`). Needs
  your Claude key: `docs/ai-control.md`.
- LW6b part 2: the Chat panel: the conversation, a box to write in,
  Stop and Clear.
- LW6b part 3: the editor asks you before Claude changes anything
  ("Ask me before changes" in the Chat panel: Allow, Allow everything
  for this message, or Don't allow), and before any command loads a
  script file. Only the buttons in the editor's window answer.
- LW6b part 4: Claude in the chat can look at the editor: a
  screenshot it takes comes back to it as a picture.
- LW6b part 5: Claude's answers in the chat appear as they are written,
  word by word, instead of all at once at the end.
- LW7 part 1: mods as folders beside a world, each with a manifest (id,
  version, the mods it needs), a load order that is the same every time,
  and every problem said (`list_mods`, `enable_mod`; `docs/mods.md`).
- LW7 part 2: a mod's `world.json` adds or replaces places, person looks,
  routines and behaviours when the world is opened; a later mod wins and
  every clash is reported; the world's own files stay its own when saved.
- LW7 part 3a: a mod brings people of its own, under ids that stay the
  same whatever other mods are on, so saved games find them again.
- LW7 part 3b: a mod keeps its own art in its folder (`mod:art/guard.glb`),
  so a mod is one folder that can be copied whole.
- LW7 part 3c: a mod brings kits: one of a name the world has gives every
  placement of it the mod's pieces, any other is added.
- LW7 part 3d: a mod brings C# scripts, run beside the world's own while
  it is open, and only once allowed on the machine (a script is a
  program; `docs/mods.md`).
- LW8: the living-world gate (`tools/living_gate.py`): 5,000 people
  living a day in ten minutes in the valley (sleep, work, a midday meal),
  flown through, a shot in the street, a game saved and loaded.
  `simulate` with `clock` lets the day pass while simulating; the frame's
  phases time the people's share (`travel`, `senses`, `routines`,
  `people`); `set_people_settings` counts anyone appearing or going
  close by in view.
- N7 part 1 (standalone game build): `giant_editor --game <world folder>`
  runs a world as a game: a main menu (Continue, New game, Load,
  Settings, Quit) instead of the editor's panels, play from it, and the
  pause menu's Quit leads back to it.
- N7 part 2: a game's settings last. UI scale, field of view, mouse
  speed, fullscreen, vsync and the window's size are kept as text
  (`game_settings.json` in the person's settings folder) and are there
  the next time the game runs.
- N7 part 3: a game as a folder. `package_game` writes the open world as
  a game into a new folder: the program under the game's name, the world,
  the files it uses, the scripts and `game.json`. That folder runs on its
  own, from anywhere, as the game: no editor panels, and no chat or key
  in it.
- N7 part 4: a game's loading screen. A new game, or one loaded from a
  menu, begins only once the world round the player is in (cells,
  textures, ground), behind a loading screen with a bar, so the first
  cells do not arrive during the first seconds of play.
- N7 part 5: the game gate (`tools/game_gate.py`): the valley packaged
  as a game, that game begun behind its loading screen, run through,
  saved and taken up again, timed from the first frame of play.
- Fast travel: `travel_player` puts the player far off, past what is
  loaded too, and in a game the loading screen stands until the world
  there is in; a camera that
  jumps over 64 m in the editor brings the world round it in with a
  loading screen's shares of each frame, not play's.
- The loading screen also waits for scatter, the far ground and the
  far-off people's poses, and (for at most 5 s, once the rest is in)
  for the GPU's own: the voxel world, sun shadows and geometry.
- Crowds part 1: baked poses. A character held in one pose becomes a
  small plain mesh (`bake_pose`, `get_baked_poses`), cut down from the
  file and baked on a worker: what distant people will be drawn with.
- Crowds part 2: figures. People with no entity (those farther off than
  the ones near the camera) are drawn as instances of their look's baked
  pose, out to 400 m, with no skeleton (`set_figure_settings`; on by
  default since the crowd gate measured them).
- Crowds part 3: figures walk. One travelling is put along its way
  every frame, facing along it, stepping through eight poses of its
  look's walk baked in place (which one by how far it has gone, so its
  feet keep to the ground at any speed); one at what its routine has it
  do is held in its look's pose for that, and one asleep is not drawn.
- Crowds part 4: nobody is missing between far and near. A person whose
  entity is taken away is a figure from that same frame, where it
  stood, and one given an entity shows what it does (sitting, working)
  from its first frame.
- Crowds part 5: variety. A look can come in up to 16 colours
  (`set_person_look`'s `tints`); each person has one of them by its
  seed, the same as a figure far off and as an entity near.
- Crowds part 6: the crowd gate (`tools/crowd_gate.py`): 3,000 people
  in a market square by the valley's town, 2,000 standing and 1,000
  walking its lanes, flown to, through and round; it measures what the
  figures add on the GPU (their shadows told apart) and what the
  people's own work takes of the main thread. It passed on a real GPU
  (0.30 ms for 3,000), so figures are on by default.
- The far-off voxel world keeps up in big worlds: its bricks are built on
  worker threads, small things far off are added as single points rather
  than triangle by triangle, bricks never built go before ones that only
  changed, and behind a loading screen it gets 12 ms a frame.
- The voxel world catches up while plants stream in: it no longer
  re-checks every mesh it knows each time one is added, and marks a
  far-off brick once per batch of changes rather than once per plant.

The Windows editor download is attached to each CI run (`giant-editor-windows`).
It is a console app for now so its log is visible; run `giant_editor.exe` and
close the window to exit. Keep the `D3D12` folder next to the exe. The
performance gates come with it: in that folder, run
`python n1_gate.py giant_editor.exe`, `python n2_gate.py giant_editor.exe`,
`python n3_gate.py giant_editor.exe`, `python night_gate.py giant_editor.exe`
(the valley at night, lit by over a thousand lamps) and `python water_gate.py giant_editor.exe`
(the valley with a lake, a river past the town and a coast, flown over and under the water) and `python effects_gate.py giant_editor.exe`
(that valley in a storm, with chimney smoke, fires, a boathouse knocked into the river and shots fired) and `python characters_gate.py giant_editor.exe`
(a crowd walking the valley's town, some struck down by a blast and shots and getting up again, and faces seen close) and `python living_gate.py giant_editor.exe`
(5,000 people living their day in the valley's town and villages, flown through as they wake, a shot in the street, a game saved and loaded; about ten minutes) and `python crowd_gate.py giant_editor.exe`
(3,000 people in a market square by the town, drawn as figures far off and as characters near, flown to, through and round; about five minutes) and `python game_gate.py giant_editor.exe`
(the valley packaged as a game, and that game run on its own: a new game begun behind its loading screen, the player running down the valley, the game saved and taken up again, and before that the editor's camera flown down the valley at 30 and 90 m/s to measure how the voxel world keeps up; about four minutes) (Python 3; keep the folder's `.py` files together: the gates use one another). A gate with hitches prints which part of the frame each one spent its time on; set `GIANT_GATE_FRAMES` to a file name to keep the details. C# scripts need the .NET 10 runtime
(https://dotnet.microsoft.com/download/dotnet/10.0); the rest works without it.

The earlier Godot-based work is archived on the branches
`giant/m0-foundation` and `giant/m1-giant-prototype`.

## Build

Needs CMake 3.25+, Ninja, Git, a C++20 compiler (MSVC 2022, Clang 16+ or GCC 13+),
and the .NET 10 SDK for C# scripting (optional; without it scripting is skipped).
On Linux, SDL3 also needs the X11 headers (`sudo apt install libx11-dev libxext-dev`).

```sh
cmake --preset dev          # debug build with sanitizers (Linux)
cmake --build --preset dev
ctest --preset dev
```

On Windows, open a "x64 Native Tools" prompt and use the `msvc` preset, or open
the folder in Visual Studio.

`python3 tools/check_architecture.py` checks that every module only includes
the modules it is allowed to (CI runs it on every push).

## Layout

| Folder | Holds |
| --- | --- |
| `engine/core` | Memory, threads, jobs, math (double precision), logging, reflection, file formats |
| `engine/platform` | Windows, input and files, on SDL3 |
| `engine/rhi` | Thin render layer, D3D12 backend first |
| `engine/voxel` | Voxel bricks, voxelizing triangles, the camera-centred clipmap (plain data) |
| `engine/render` | Virtual geometry, voxel world, GI, shadows, materials, hair, sky and clouds |
| `engine/world` | Cells, streaming, landscape, scatter, weather simulation |
| `engine/sim` | Physics glue (Jolt), destruction, giants, animation, NPC simulation |
| `engine/nav` | Navigation: walkable tiles per cell, path searches in bounded slices |
| `engine/script` | .NET host, C# game framework |
| `engine/ai` | Command layer, MCP server, rule checks |
| `editor` | Editor app |
| `game` | The game project (C# scripts, content) |
| `shaders` | Slang shader library and material types |
| `third_party` | Pinned open-source libraries |
| `tests` | Shared test harness; unit tests live next to each module |
| `docs` | Architecture and rules |

AI agents: read `AGENTS.md` first.
