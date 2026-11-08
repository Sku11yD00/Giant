# Game plan: FOLD (working title)

The second game on Giant, and the one being built now (the giantess game is
on hold since 2026-11-08). A first-person, single-player open-world survival
shooter with the gameplay loop of the Stalker GAMMA mod pack, in a new
setting, with real destruction. The design doc with the reasoning lives
outside the repo (link in the PR that added this file); this file is the
part engineers and agents need.

## The loop

1. **Scavenge** bodies, containers and stashes (stash locations come from
   intel found on bodies and from missions).
2. **Disassemble** guns and armour into parts. Each part keeps its own
   condition. Found guns are worn (20 to 60 %), and traders pay little for
   them, so parts are where the value is.
3. **Repair, craft, upgrade** at benches with kits and tools. The tool tier
   (basic, advanced, expert) caps what can be repaired or fitted. Tools are
   scarce and found deeper in.
4. **Trade and take jobs**: sell parts and relics, buy tools and ammo, take
   repeatable missions from faction bosses, follow the main story.
5. **Push into the next ring**: the map is split into rings by distance from
   the start. Each ring raises enemy gear, creatures, hazard strength,
   weather and loot tier together.

## Setting (default until the owner picks)

"The Fold": a sealed northern valley, about 16 x 16 km. Ring 1 farms and a
checkpoint town; ring 2 boreal forest, logging camps, a rail yard; ring 3 a
mining town, the dam, a military base; ring 4 the collapsed research site.
Blizzards and "Fold storms" near the centre.

## Who builds what

| Area | Builder | Where |
| --- | --- | --- |
| Items, inventories, weapon parts, condition, disassembly, repair, crafting, loot tables | game thread | `engine/gameplay` |
| Health, bleeding, armour, survival needs | game thread | `engine/gameplay` |
| Factions, A-life squads, traders, missions | game thread | `engine/gameplay` + C# scripts |
| Weapon handling: viewmodel, firing, ballistics, penetration, hit events | engine thread | `render`, `sim` |
| Game UI: fonts, HUD, inventory/trade screens, PDA | engine thread | new module |
| Audio | engine thread | new module |
| NPC navigation, perception, locomotion | engine thread | `sim`, `anim` |
| Save games | engine thread (framework), game thread (game data) | |

Rule of thumb: the engine decides how things move, render, sound and get
hit; `gameplay` decides what the numbers are and what they mean.

`engine/gameplay` is pure rules: it depends on `core`, `world` and `sim`,
holds no GPU state, and is fully testable in ctest. Every rule set is data
(items, parts, recipes, loot tables, factions, missions are text files with
stable ids, AGENTS.md rule 8), and every action is a command the editor, the
AI and C# scripts share (rule 7).

## Roadmap

| Phase | Game thread | Engine thread |
| --- | --- | --- |
| G1 Gear rules | item catalog and inventories; weapon parts, condition, disassembly, repair; crafting and tool tiers; loot tables and containers; commands | weapon handling and hits |
| G2 First fight | health, bleeding, armour, survival; part condition drives jams, spread, recoil | game UI, audio |
| G3 Living ring 1 | factions, A-life squads, traders, stashes, repeatable missions, save data | NPC navigation and perception, save framework |
| G4 Slice gate | rings 1 and 2 playable end to end | gate tooling, performance |
| G5 Full Fold | rings 3 and 4, hazards and relics, creatures, main story | photoreal characters (N5), storm and hazard effects |

The G4 gate is like the N gates: 60 fps at 1440p on the reference GPU,
judged by p99 frame time and hitch count (AGENTS.md rule 9), during
firefights that break cover.
