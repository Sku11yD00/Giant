# Mods

A mod changes or adds to a world without touching the world's own files:
you can turn it off and the world is as it was. This page is for people
who make mods and people who install them.

So far the engine knows what mods there are, which are on, and what order
they load in. What a mod holds (changes to the world, new assets, scripts)
is loaded by parts still being built; this page will grow with them.

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
    better_roads/
      mod.json
```

To install a mod, copy its folder into `mods`. To remove it, turn it off
and delete the folder.

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
| `id` | yes | The mod's own name among mods, and its folder's name: 1 to 64 of `a-z`, `0-9` and `_`, beginning with a letter |
| `name` | yes | The name people see |
| `version` | yes | Three numbers with dots, such as `"1.2.0"` |
| `author`, `description` | no | For people |
| `needs` | no | Mods this one cannot work without. Each has an `id` and may have `at_least`, the oldest version that will do. They load before this mod |
| `after` | no | Mods this one loads after when they are on. It works without them |

A key the engine does not know is an error, so a typing mistake is told
to you instead of being ignored.

## Turning mods on and off

In the editor (or over MCP, or by asking Claude in the Chat panel):

- `list_mods` shows every mod beside the open world: whether it is on,
  whether it is loaded, and its place in the load order.
- `enable_mod {"id": "winter"}` turns one on; `{"id": "winter",
  "enabled": false}` turns it off.

Which mods are on is kept in `mods/enabled.json`, so it travels with the
world.

## Load order

Mods load one after another, and a later mod's changes win over an earlier
one's. The order is worked out from the manifests:

1. A mod loads after every mod it `needs`.
2. A mod loads after every mod in its `after` list that is on.
3. Otherwise, by id, in alphabetical order.

So the same set of mods always loads in the same order, on every machine.

## When a mod does not load

A mod that is on but cannot load is left out, and `list_mods` says why
under `problem`. Nothing is left out silently. The reasons:

- A mod it needs is not there, is not on, or is older than `at_least`.
- A mod it needs was itself left out.
- Mods wait on each other in a circle (A needs B, B needs A).

Folders in `mods` that are not mods are listed under `broken` with the
reason: no `mod.json`, a `mod.json` with a mistake (the message says
where), or a folder whose name is not the mod's id.

## Limits

At most 256 mods beside one world. A `mod.json` is at most 64 KB, a
description at most 2,048 bytes, and a mod names at most 32 `needs` and 32
`after`.
