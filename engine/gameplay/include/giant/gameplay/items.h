// Items: what can be picked up, carried, sold, taken apart. Each kind of
// item is defined once in an ItemCatalog, loaded from text files
// ("giant-items", see docs/architecture.md, "Gameplay rules"); inventories
// refer to it by index.
#pragma once

#include "giant/core/types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace giant::gameplay {

enum class ItemKind : u8 {
    weapon,     // a whole gun; taken apart into parts (G1 part 2)
    part,       // a gun or armour part: barrel, bolt, trigger group, plate
    ammo,
    armour,     // suits, helmets
    tool,       // repair kits and tool sets (their tier caps repairs)
    consumable, // food, drink, meds
    relic,      // found in the Fold's warped zones
    misc,       // junk, intel, quest items
};

// The kind named in item files ("weapon", "part", ...), or none.
std::optional<ItemKind> item_kind(std::string_view name);
std::string_view item_kind_name(ItemKind kind);

struct ItemDef {
    std::string id;   // stable, lowercase letters, digits and '_' (what files and saves use)
    std::string name; // shown to the player
    ItemKind kind = ItemKind::misc;
    float weight_kg = 0.0f; // one of it
    u32 value = 0;          // a trader's base price for one, in full condition
    // How many lie in one inventory stack. Worn items never stack.
    u32 max_stack = 1;
    // Has a condition, 0 (broken) to 1 (new): guns, parts, armour, tools.
    bool worn = false;

    bool operator==(const ItemDef&) const = default;
};

// An item's place in its catalog: the order items were added, so load a
// game's item files in a fixed order (sorted by name) for indices to stay
// the same from run to run. Files and saves use ids, never indices.
using ItemIndex = u32;

class ItemCatalog {
public:
    // Adds an item kind; why it cannot (an id that is malformed or used
    // already, no name, a weight or max_stack out of range, a worn item
    // that stacks), and then nothing changes.
    [[nodiscard]] std::optional<std::string> add(ItemDef item);

    // The item with that id, or none.
    std::optional<ItemIndex> find(std::string_view id) const;
    const ItemDef& get(ItemIndex index) const;
    usize size() const { return items_.size(); }
    std::span<const ItemDef> items() const { return items_; }

private:
    std::vector<ItemDef> items_;
    std::unordered_map<std::string, ItemIndex> by_id_;
};

// Why `item` cannot be in a catalog, or none.
std::optional<std::string> check_item(const ItemDef& item);

// An item file: {"format": "giant-items", "version": 1, "items": [{"id":
// "makarov", "name": "Makarov PM", "kind": "weapon", "weight_kg": 0.73,
// "value": 900, "worn": true}, ...]}. "max_stack" (1) and "worn" (false)
// may be left out. A game loads several files into one catalog.
std::string save_items(const ItemCatalog& catalog);
// Adds every item in `text` to `catalog`, or none of them: on an error
// (not JSON, a missing or wrong value, an id already in the catalog) it
// says where and `catalog` is unchanged.
[[nodiscard]] std::optional<std::string> load_items(std::string_view text, ItemCatalog& catalog);

} // namespace giant::gameplay
