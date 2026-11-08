// Weapons made of parts, as in GAMMA: a gun is its frame (the weapon item)
// with a part in each slot (barrel, slide, bolt, trigger group...), each
// part with its own condition. Found guns are worn; taking one apart gives
// its parts back as items, the good ones worth keeping, and fitting the
// best parts builds a better gun. Weapon kinds are defined in text files
// ("giant-weapons"), beside the item files.
#pragma once

#include "giant/gameplay/inventory.h"
#include "giant/gameplay/items.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace giant::gameplay {

struct WeaponSlot {
    std::string name;             // "barrel", "trigger" (lowercase, like ids)
    std::vector<ItemIndex> parts; // the part items that fit; the first is the stock part

    bool operator==(const WeaponSlot&) const = default;
};

struct WeaponDef {
    ItemIndex weapon = 0;          // a worn item of kind weapon
    std::vector<WeaponSlot> slots; // 1 to 16; a part item fits one slot at most

    bool operator==(const WeaponDef&) const = default;
};

class WeaponCatalog {
public:
    // Why `weapon` cannot be added (see WeaponDef; parts must be worn items
    // of kind part, slot names unique, the weapon not defined yet), or none.
    [[nodiscard]] std::optional<std::string> add(const ItemCatalog& items, WeaponDef weapon);
    // The definition of a weapon item, or null when it is not a weapon here.
    const WeaponDef* find(ItemIndex weapon) const;
    std::span<const WeaponDef> weapons() const { return weapons_; }

private:
    std::vector<WeaponDef> weapons_;
};

// {"format": "giant-weapons", "version": 1, "weapons": [{"item":
// "pm_pistol", "slots": [{"name": "barrel", "parts": ["pistol_barrel"]},
// ...]}]}. Items by id, from the item catalog. All or nothing, like
// load_items.
std::string save_weapons(const WeaponCatalog& weapons, const ItemCatalog& items);
[[nodiscard]] std::optional<std::string> load_weapons(std::string_view text, const ItemCatalog& items,
                                                      WeaponCatalog& weapons);

// The slot `part` fits in, or none.
std::optional<usize> slot_of(const WeaponDef& weapon, ItemIndex part);

// Why `parts` cannot be fitted in `weapon` (a part that does not fit it,
// two parts for one slot), or none.
std::optional<std::string> check_weapon_parts(const WeaponDef& weapon, const ItemCatalog& items,
                                              std::span<const FittedPart> parts);
// The same for every weapon in an inventory (a gun whose item has no
// definition must have no parts). Run after load_inventory.
[[nodiscard]] std::optional<std::string> check_inventory_weapons(const Inventory& inventory,
                                                                 const ItemCatalog& items,
                                                                 const WeaponCatalog& weapons);

// A weapon's condition: the mean of its frame (the weapon stack's own
// condition) and its slots' parts, an empty slot counting as 0. A gun
// missing a part does not fire (`complete`).
float weapon_condition(const WeaponDef& weapon, float frame_condition,
                       std::span<const FittedPart> parts);
bool complete(const WeaponDef& weapon, std::span<const FittedPart> parts);

// Puts a found weapon in `inventory`: the stock part in every slot, the
// frame and each part near `condition` (within 0.15 either way, from `seed`, so the same seed
// gives the same gun). `weapon` comes from a WeaponCatalog. Returns its
// stack id.
u32 add_weapon(Inventory& inventory, const ItemCatalog& items, const WeaponDef& weapon,
               float condition, u64 seed);

// Parts below this condition are scrap: taking a gun apart loses them.
inline constexpr float k_scrap_condition = 0.1f;

struct Disassembly {
    u32 parts_kept = 0;
    u32 parts_scrapped = 0;
};
// Takes the weapon in stack `weapon_stack` apart: the frame stays in its
// stack (same id, its own condition, no parts) and each part goes into
// the inventory as an item. Working without good tools wears the parts:
// each loses up to (1 - tool_quality) x 0.1 (from `seed`); tool_quality 0
// (bare hands) to 1 (an expert tool set). Parts that end below
// k_scrap_condition are lost. None when the stack is not a weapon.
std::optional<Disassembly> disassemble(Inventory& inventory, const ItemCatalog& items,
                                       const WeaponCatalog& weapons, u32 weapon_stack,
                                       float tool_quality, u64 seed);

// Fits the part in stack `part_stack` into the weapon in stack
// `weapon_stack`, in the slot it fits; the part that was there comes out
// as an item. False (and nothing changes) when either stack is missing,
// the first is not a weapon or the part does not fit it.
bool fit_part(Inventory& inventory, const ItemCatalog& items, const WeaponCatalog& weapons,
              u32 weapon_stack, u32 part_stack);
// Takes the part out of `slot`, as an item; false when there is none.
bool strip_part(Inventory& inventory, const ItemCatalog& items, const WeaponCatalog& weapons,
                u32 weapon_stack, usize slot);

} // namespace giant::gameplay
