# Mods

A mod changes or adds to a world without touching the world's own files:
you can turn it off and the world is as it was. This page is for people
who make mods and people who install them.

So far a mod can change a world's places, the looks of its people, their
routines and their behaviours, and bring people of its own, with its own
art files. Kits and scripts are parts still being built; this page will
grow with them.

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

A mod's `world.json` holds places, person looks, routines, behaviours and
people. All but the people (see below) are written exactly as a world's own `world.json` writes them (the easiest way
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
replaced, with the clashes first (at most 256 changes are listed; the
counts are of all).

A routine or behaviour may send people to a place by name. Nothing checks
that the place is there, in a mod or in a world: a place that is missing
is one nobody can be sent to, so name places your mod brings or needs.

### A mod's own files

A look needs a mesh, and may name image maps for its materials and a file
for its hair. Keep those files in the mod's folder and name them with
`mod:` and their path inside it:

```json
"person_looks": [
  {"name": "guard", "mesh": "mod:art/guard.glb", "height": 1.2}
]
```

With the mod in `<world>/mods/guards/`, that is the file
`<world>/mods/guards/art/guard.glb`. So a mod is one folder: copy it into
another world's `mods` folder and its art comes with it.

- This works for a look's `mesh`, its `hair.file`, the maps of its
  `material` and of each of its `parts`, and a skin's `detail_normal`.
- Use forward slashes. The path may not leave the mod's folder: no `..`,
  no `.`, no empty part, no backslash, no colon. Nor may it hold what
  Windows reads as something else: a part that ends in a dot or a space
  or begins with a space, a device's name (`con`, `prn`, `aux`, `nul`,
  `conin$`, `conout$`, `com0` to `com9`, `lpt0` to `lpt9`, whatever
  follows a dot), any of `< > " | ? *`, or a control character. A path that breaks this is an error, and the mod's changes
  are left out.
- Put the files themselves in the mod's folder, not links to them. A mod
  that names files of its own and has a link (or, on Windows, a
  junction) anywhere in its folder is left out, and `list_mods` says
  which link.
- A path without `mod:` is a path from the folder the game runs in, for
  files every install has (`people/villager.glb`). The same rules hold
  for it, so a mod can never name a file by a drive, a root, a server or
  a folder above. (A world's own files may: the world is yours, a mod
  may be anyone's.)
- `mod:` means something only in a mod's `world.json`. Do not write it
  in a world's own files or in a command: there it is taken as part of a
  file's name, and no such file will be found.
- The whole path (the world's folder, `mods/<id>/`, and the path in the
  mod) can be at most 1024 bytes.

### People

A mod can bring people of its own. They are written as a world's people
are, one row each, under `people.list`:

```json
"people": {
  "list": [
    [1, "Captain", "guard", "barracks", 70, 0, -70, 0, 5],
    [2, "Recruit", "guard", "barracks", 72, 0, -70, 90, 6, "guard's day"]
  ]
}
```

A row is `[number, name, look, home, x, y, z, yaw, seed]`, then the
person's routine if it has one, then its behaviour if it has one (with
`""` for the routine when it has a behaviour and no routine). The look,
home, routine and behaviour may be the world's, an earlier mod's, or this
mod's own; if one is not there, the mod's changes are left out and
`list_mods` says which.

The first value is the person's **number in the mod**, from 1 to 65535,
in rising order. It is not the person's id in the world. Each mod is
given a **slot** the first time it is turned on (`list_mods` shows it,
and it is kept in `mods/enabled.json` even while the mod is off), and
person number *n* of the mod in slot *s* has the id
1073741824 + 65536 × *s* + *n*. So:

- A mod's people never take the id of one of the world's people or of
  another mod's.
- They keep their ids whatever other mods are turned on or off, and in
  whatever order the mods load. A saved game finds them again.
- Keep a person's number for good once people play with your mod. If you
  remove a person, leave its number unused.

People are only ever added. A mod cannot replace or remove one of the
world's people.

While the mod is on you can move or change its people in the editor, but
they are the mod's: `save_world` does not write them into the world, and
names the ones you changed or removed as `person <id>` under
`mods_things_not_saved`. To change them for good, change the mod's
`world.json`.

A mod that is on and has no slot (the list was written by hand, or
before mods had slots) is left out if it brings people, until you run
`enable_mod` on it again, which gives it one. There are 512 slots; a mod
keeps its slot until you take its line out of `slots` in that file.
Do not take a line out, or delete the file, for a mod people have played
with: the slot goes to the next mod turned on, the mod's people come back
under other ids, and saved games would take them for someone else.

A mod's changes go in whole or not at all. If they would leave the world
broken (more looks than a world keeps, say), that mod's changes are left
out and `list_mods` says why under `world.problems`. The same goes for a
`world.json` with a mistake in it.

After turning mods on or off, or changing a mod's `world.json`, open the
world again to see the change. `list_mods` says `reopen` when other mods
load now than when the world was opened, a mod's slot has changed, or a
mod's `world.json` has appeared, gone, or changed in size or time of
writing since. (A file
rewritten at the same size within the same tick of the disk's clock, or
copied over with its old time kept, is not noticed: open the world again
anyway after changing one.)

### Working on a world while mods are on

You can keep building a world with mods on. When you save:

- The world's own things are saved as you changed, made and removed them.
- What a mod brought is the mod's, and is never written into the world's
  files (nor is a mod's look's mesh put in the world's list of meshes).
  That holds even if you changed it in the editor: the change shows until
  the world is closed, is not saved, and `save_world` names such things
  under `mods_things_not_saved` (names only, without their kind, at most
  32 of them; `mods_things_not_saved_count` is how many there are). Where
  a mod replaced one of the world's own things, the world's own stays in
  its file as it was.
- A name a mod set stays the mod's for as long as the world is open. If
  you remove a mod's thing and make your own under the same name, yours
  is not saved either (and is named as above): give it another name.
  To change what a mod brings, change the mod's `world.json`; to change
  the world's own, turn the mod off first.
- The world must still open without the mods. If one of the world's
  people uses a look, home, routine or behaviour that only a mod brings,
  the save is refused and says which. Give the world one of its own under
  another name.

If you close a world (`close_world`) the scene keeps what the mods
brought, as plain things: saved as a new world, they would be that
world's own.

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
