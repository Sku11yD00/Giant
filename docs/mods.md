# Mods

A mod changes or adds to a world without touching the world's own files:
you can turn it off and the world is as it was. This page is for people
who make mods and people who install them.

So far a mod can change a world's places, the looks of its people, their
routines and their behaviours. People of a mod's own, new assets and
scripts are parts still being built; this page will grow with them.

## Where mods go

Each mod is a folder inside the world's `mods` folder, named by the mod's
id:

```
my_world/
  world.json
  mods/
    enabled.json          which mods are on (the editor writes this)
    winter/
      mod.json            the mod's manifest
      world.json          what it changes of the world (optional)
    better_roads/
      mod.json
```

To install a mod, copy its folder into `mods`. To remove it, turn it off
and delete the folder. A folder whose name begins with `.` or `_` is
passed over, so you can set a mod aside by renaming it `_winter`. A mod
must be a real folder, not a link to one.

## The manifest: mod.json

```json
{
  "format": "giant-mod",
  "format_version": 1,
  "id": "winter",
  "name": "Deep Winter",
  "version": "1.2.0",
  "author": "Someone",
  "description": "Snow on everything, and frozen lakes.",
  "needs": [{"id": "better_roads", "at_least": "2.0.0"}],
  "after": ["lanterns"]
}
```

| Key | Needed | What it is |
|---|---|---|
| `format`, `format_version` | yes | Always `"giant-mod"` and `1` |
| `id` | yes | The mod's own name among mods, and its folder's name: 1 to 64 of `a-z`, `0-9` and `_`, beginning with a letter. Not a name Windows keeps for devices (`con`, `prn`, `aux`, `nul`, `com0` to `com9`, `lpt0` to `lpt9`) |
| `name` | yes | The name people see: one line, at most 128 bytes |
| `version` | yes | Three numbers with dots, such as `"1.2.0"`, each 0 to 999999 and written without a 0 in front (`1.2.0`, not `01.02.0`) |
| `author`, `description` | no | For people: plain text, the author one line of at most 128 bytes |
| `needs` | no | Mods this one cannot work without. Each has an `id` and may have `at_least`, the oldest version that will do. They load before this mod |
| `after` | no | Mods this one loads after when they are on. It works without them |

A key the engine does not know is an error, so a typing mistake is told
to you instead of being ignored.

## What a mod changes: world.json

A mod's `world.json` holds places, person looks, routines and behaviours,
written exactly as a world's own `world.json` writes them (the easiest way
to make one is to build the things in the editor, save the world, and copy
them out of its `world.json`):

```json
{
  "format": "giant-mod-world",
  "version": 1,
  "places": [
    {"name": "tavern", "position": [30, 0, -30], "radius": 4, "tags": ["rest"]}
  ],
  "routines": [],
  "person_looks": [],
  "behaviours": []
}
```

Things go by name:

- A thing whose name the world does not have is **added**.
- A thing whose name the world already has **takes its place**, whole. So
  a mod can move the smithy, give the villagers a new look, or rewrite
  what the guards do when they hear a shot.

The world's own files are never changed by a mod. The changes are laid
over the world each time it is opened, in the load order, so a later mod's
thing wins over an earlier mod's of the same name. Every such clash is
reported: `list_mods` shows under `world` what each mod added and
replaced, with the clashes first.

A mod's changes go in whole or not at all. If they would leave the world
broken (more looks than a world keeps, say), that mod's changes are left
out and `list_mods` says why under `world.problems`. The same goes for a
`world.json` with a mistake in it.

After turning mods on or off, open the world again to see the change
(`list_mods` says `reopen` when the world on screen is not what the mods
now give).

### Working on a world while mods are on

You can keep building a world with mods on. When you save:

- What the mods brought and you left alone is not written into the
  world's files.
- What you changed or made is the world's own and is saved, including a
  mod's thing that you changed (it becomes the world's).
- The world must still open without the mods. If one of the world's
  people uses a look, home, routine or behaviour that only a mod brings,
  the save is refused and says which.

## Turning mods on and off

In the editor (or over MCP, or by asking Claude in the Chat panel):

- `list_mods` shows every mod beside the open world: whether it is on,
  whether it is loaded, and its place in the load order. It also lists
  `broken` folders (below), `missing` mods (on, but their folder is gone),
  and says so when `enabled.json` or the `mods` folder cannot be read.
- `enable_mod {"id": "winter"}` turns one on; `{"id": "winter",
  "enabled": false}` turns it off.

Which mods are on is kept in `mods/enabled.json`, so it travels with the
world.

## Load order

Mods load one after another, and a later mod's thing wins over an earlier
one's of the same name (the clash is reported). The order is worked out
from the manifests:

1. A mod loads after every mod it `needs`.
2. A mod loads after every mod in its `after` list that is loaded.
3. Otherwise by id, compared letter by letter (so `m10` comes before `m9`,
   and `a_b` before `aa`).

So the same set of mods always loads in the same order, on every machine.

## When a mod does not load

A mod that is on but cannot load is left out, and `list_mods` says why
under `problem`. Nothing is left out silently. The reasons:

- A mod it needs is not there, is not on, or is older than `at_least`.
- A mod it needs was itself left out.
- Mods wait on each other in a circle, through `needs` or `after` (A needs
  B, B needs A; or A after B, B after A). The message names the mods of
  the circle. A mod that only asked to come `after` a mod that was left
  out loads without it.

Folders in `mods` that are not mods are listed under `broken` with the
reason: no `mod.json`, a `mod.json` with a mistake (the message says
where), or a folder whose name is not the mod's id (capital letters count:
the folder `Winter` is not the mod `winter`).

## Limits

At most 256 mods beside one world: only the first 256 folders in `mods`
(by name, broken ones included) are looked at, and `list_mods` says
`too_many` when there are more. A `mod.json` and `enabled.json` are at most
64 KB each, a name or author at most 128 bytes, a description at most
2,048 bytes, and a mod names at most 32 `needs` and 32 `after`. A mod's
`world.json` is at most 64 MB, and the world with its mods keeps to a
world's own limits (65,536 places, 1,024 looks, 1,024 routines, 256
behaviours).
