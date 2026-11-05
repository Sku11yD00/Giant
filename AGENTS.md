# Rules for AI agents working on Giant Engine

Read this file, `docs/architecture.md` and `docs/coding-rules.md` before you
change anything. These rules are enforced by CI where possible; where they are
not, reviewers enforce them.

## What this engine is for

One game genre: a huge open world, a giantess who grows to landscape scale and
destroys cities and forests, photoreal and sharp with no temporal smear.
Performance is a pillar: every system has a frame budget. When a choice trades
generality for speed in that genre, take speed.

## Hard rules

1. **Modules only talk downward.** The allowed dependencies are listed in
   `docs/architecture.md` and checked by `tools/check_architecture.py`. Never
   include another module's `src/` headers; only its `include/giant/<module>/`.
2. **No screen-space temporal accumulation.** No TAA, no history buffers that
   reproject old frames, no motion blur. Anything that must accumulate over
   time does it in world space (voxels, probes). An opt-in temporal AA mode
   may exist for players, never on by default and never required by an effect.
3. **Double precision for world positions.** Store positions as `DVec3`
   (`giant/core/math.h`). Convert to float only relative to the camera, at the
   last moment, with `to_camera_relative`.
4. **Tests with every change.** New behaviour gets a test in the module's
   `tests/` folder. Fix a bug by first writing the test that fails.
5. **Warnings are errors.** Do not silence a warning; fix the cause.
6. **No new third-party code without an entry** in `third_party/README.md`
   with its licence (permissive only: MIT, BSD, zlib, Apache 2, Boost).
7. **Everything the editor can do is a command** (from N0 part 3): a named,
   typed, undoable action that people and the AI both use. Never add an editor
   feature that only works by clicking.
8. **Data is text.** Scenes, cells, materials, scatter graphs and settings are
   saved as readable, diffable text with stable IDs. Only cooked binary data
   (meshes, textures) is binary.
9. **No micro stutter.** Frame-time consistency is part of the performance
   pillar, judged by the 99th percentile and hitch count, not the average.
   Never compile shaders or create pipelines while the game runs: shaders are
   compiled at build time and pipelines made at load (any variant created
   later is a bug). Streaming and uploads work within a per-frame budget and
   never wait on disk or the GPU on the main thread. C# scripts must not
   allocate every frame; the GC runs in background mode. Performance gates
   (tools/n1_gate.py and later ones) fail on hitches as well as on fps.

## Multi-agent review (required)

The owner's standing rule: when a piece of the engine is finished, it is not
done until independent agents have checked it.

1. The author runs the build, tests and architecture check.
2. At least two reviewer agents, who did not write the code, review it in
   parallel from different angles (for example correctness and concurrency;
   performance and frame budget; rules, tests and regressions). They get the
   code and the rule book, not the author's conclusions.
3. Every finding is verified by an agent other than the one that raised it
   (reproduce it, or show why it is wrong) before it is fixed or dismissed.
4. The author fixes confirmed findings, adds a test for each real bug, and
   reruns everything. The PR description lists what the review found and
   what changed.

## Style of work

- Small, focused changes. One feature per pull request where possible.
- Prefer deleting code to adding it. Do not copy-paste; extract a function.
- Name things for what they mean in the game ("giant", "cell", "rubble"),
  not for how they are implemented ("manager", "helper", "util").
- If you are unsure where code goes, put the question in the PR description
  instead of guessing.

## Before you push

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan   # when touching threads
python3 tools/check_architecture.py
```
