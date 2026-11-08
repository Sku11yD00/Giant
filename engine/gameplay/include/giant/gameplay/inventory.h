// Inventories: a backpack, a body, a crate, a stash, a trader's stock. A
// list of stacks of catalog items, each with a count and a condition.
// Weight is not a hard limit (as in GAMMA): carrying more only slows the
// player down (`burden`).
#pragma once

#include "giant/gameplay/items.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace giant::gameplay {

class Inventory;

// An inventory as text: {"format": "giant-inventory", "version": 1,
// "next_id": 5, "stacks": [{"id": 3, "item": "makarov", "count": 1,
// "condition": 0.42}]}, items by their catalog id. "count" (1) and
// "condition" (1) may be left out.
std::string save_inventory(const Inventory& inventory, const ItemCatalog& catalog);
// Replaces `out` only on success; strict: an unknown item or field, a stack
// over its item's max_stack, a worn count above 1, a repeated id are errors.
[[nodiscard]] std::optional<std::string> load_inventory(std::string_view text,
                                                        const ItemCatalog& catalog, Inventory& out);

// Saved stack ids (and "next_id") stay at or below this, so a loaded
// inventory never runs out of ids.
inline constexpr u32 k_max_stack_id = 1u << 30;
// The most one call to Inventory::add takes.
inline constexpr u32 k_max_add_count = 1u << 20;

struct ItemStack {
    // Unique in its inventory and never reused there (also across saves),
    // so the UI and other rules can point at one stack.
    u32 id = 0;
    ItemIndex item = 0;
    u32 count = 1;
    // 0 (broken) to 1 (new); 1 for items that are not worn.
    float condition = 1.0f;

    bool operator==(const ItemStack&) const = default;
};

class Inventory {
public:
    // Puts `count` of `item` in: a stackable item fills the stacks of it
    // already here up to its max_stack, then starts new ones; a worn item
    // gets a stack of one each, at `condition` (clamped to 0..1; items that
    // are not worn ignore it). Returns the id of the last stack it touched
    // (0 when `count` is 0). `count` is at most k_max_add_count.
    u32 add(const ItemCatalog& catalog, ItemIndex item, u32 count, float condition = 1.0f);

    // Takes up to `count` of `item` out, the worst condition first, then the
    // newest stack; returns how many it took.
    u32 remove(ItemIndex item, u32 count);
    // Takes up to `count` out of one stack; returns what it took (all zero
    // when there is no such stack or `count` is 0). An emptied stack goes.
    ItemStack take(u32 stack_id, u32 count);

    // Moves up to `count` from the stack `stack_id` of `from` into `to`,
    // keeping its condition; returns how many moved (0 when `from` and `to`
    // are the same inventory).
    static u32 move(const ItemCatalog& catalog, Inventory& from, u32 stack_id, u32 count,
                    Inventory& to);

    // Sets a worn item's condition (repairs and wear), clamped to 0..1;
    // false when there is no such stack or its item is not worn.
    bool set_condition(const ItemCatalog& catalog, u32 stack_id, float condition);

    const ItemStack* find(u32 stack_id) const;
    // How many of `item` there are, in all stacks.
    u64 count(ItemIndex item) const;
    std::span<const ItemStack> stacks() const { return stacks_; }
    bool empty() const { return stacks_.empty(); }
    // Empties it; ids already used stay used.
    void clear();

    // The weight of everything in it.
    float weight_kg(const ItemCatalog& catalog) const;

    bool operator==(const Inventory&) const = default;

private:
    friend std::string save_inventory(const Inventory&, const ItemCatalog&);
    friend std::optional<std::string> load_inventory(std::string_view, const ItemCatalog&,
                                                     Inventory&);

    u32 new_stack(ItemIndex item, u32 count, float condition);

    std::vector<ItemStack> stacks_; // in the order they were made
    u32 next_id_ = 1;
};

// How much a character can carry before it slows down, and at most.
struct CarryLimits {
    float comfortable_kg = 30.0f; // up to here: no penalty
    float max_kg = 50.0f;         // past here: cannot sprint, walks slowly
};
enum class Burden : u8 { light, heavy, overloaded };
// Overloaded for a weight that is not a number.
Burden burden(float weight_kg, const CarryLimits& limits);

} // namespace giant::gameplay
