#include "giant/gameplay/weapons.h"

#include "giant/core/assert.h"
#include "random.h"
#include "text_file.h"

#include <algorithm>
#include <cmath>

namespace giant::gameplay {

namespace {

constexpr usize k_max_slots = 16;

bool valid_slot_name(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

float clamp01(float v) {
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) + 0.0f : 0.0f;
}

// The part fitted in `slot`, or null.
const FittedPart* part_in(const WeaponDef& weapon, std::span<const FittedPart> parts, usize slot) {
    for (const FittedPart& p : parts) {
        if (slot_of(weapon, p.item) == slot) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

std::optional<std::string> WeaponCatalog::add(const ItemCatalog& items, WeaponDef weapon) {
    if (weapon.weapon >= items.size()) {
        return std::string("the weapon is not in the item catalog");
    }
    const ItemDef& w = items.get(weapon.weapon);
    if (w.kind != ItemKind::weapon || !w.worn) {
        return "\"" + w.id + "\" must be a worn item of kind weapon";
    }
    if (find(weapon.weapon) != nullptr) {
        return "\"" + w.id + "\" is defined twice";
    }
    if (weapon.slots.empty() || weapon.slots.size() > k_max_slots) {
        return "\"" + w.id + "\" needs 1 to 16 slots";
    }
    std::vector<ItemIndex> seen;
    for (usize i = 0; i < weapon.slots.size(); ++i) {
        const WeaponSlot& slot = weapon.slots[i];
        if (!valid_slot_name(slot.name)) {
            return "\"" + w.id + "\": slot name \"" + slot.name + "\" must be 1 to 32 lowercase letters, digits or '_'";
        }
        for (usize k = 0; k < i; ++k) {
            if (weapon.slots[k].name == slot.name) {
                return "\"" + w.id + "\": slot \"" + slot.name + "\" is named twice";
            }
        }
        if (slot.parts.empty()) {
            return "\"" + w.id + "\": slot \"" + slot.name + "\" needs at least one part";
        }
        for (const ItemIndex part : slot.parts) {
            if (part >= items.size()) {
                return "\"" + w.id + "\": a part is not in the item catalog";
            }
            const ItemDef& p = items.get(part);
            if (p.kind != ItemKind::part || !p.worn) {
                return "\"" + w.id + "\": \"" + p.id + "\" must be a worn item of kind part";
            }
            if (std::find(seen.begin(), seen.end(), part) != seen.end()) {
                return "\"" + w.id + "\": \"" + p.id + "\" fits more than one slot";
            }
            seen.push_back(part);
        }
    }
    weapons_.push_back(std::move(weapon));
    return std::nullopt;
}

const WeaponDef* WeaponCatalog::find(ItemIndex weapon) const {
    for (const WeaponDef& w : weapons_) {
        if (w.weapon == weapon) {
            return &w;
        }
    }
    return nullptr;
}

std::string save_weapons(const WeaponCatalog& weapons, const ItemCatalog& items) {
    Json list = Json::array();
    for (const WeaponDef& w : weapons.weapons()) {
        Json slots = Json::array();
        for (const WeaponSlot& slot : w.slots) {
            Json parts = Json::array();
            for (const ItemIndex p : slot.parts) {
                parts.push_back(items.get(p).id);
            }
            slots.push_back({{"name", slot.name}, {"parts", std::move(parts)}});
        }
        list.push_back({{"item", items.get(w.weapon).id}, {"slots", std::move(slots)}});
    }
    return text_file_header("giant-weapons", {{"weapons", std::move(list)}}).dump(2) + "\n";
}

std::optional<std::string> load_weapons(std::string_view text, const ItemCatalog& items,
                                        WeaponCatalog& weapons) {
    Json root;
    if (std::optional<std::string> error = parse_text_file(text, "giant-weapons", {"weapons"}, root)) {
        return error;
    }
    const auto list = root.find("weapons");
    if (list == root.end() || !list->is_array()) {
        return std::string("weapons must be an array");
    }
    WeaponCatalog result = weapons;
    for (usize i = 0; i < list->size(); ++i) {
        const std::string where = "weapons[" + std::to_string(i) + "]";
        FieldReader r((*list)[i], where);
        std::string item;
        r.string("item", item, true);
        const Json* slots = r.array("slots", true);
        r.finish();
        if (r.error()) {
            return r.error();
        }
        WeaponDef def;
        const std::optional<ItemIndex> index = items.find(item);
        if (!index) {
            return where + ".item \"" + item + "\" is not in the item catalog";
        }
        def.weapon = *index;
        for (usize k = 0; k < slots->size(); ++k) {
            const std::string slot_where = where + ".slots[" + std::to_string(k) + "]";
            FieldReader sr((*slots)[k], slot_where);
            WeaponSlot slot;
            sr.string("name", slot.name, true);
            const Json* parts = sr.array("parts", true);
            sr.finish();
            if (sr.error()) {
                return sr.error();
            }
            for (usize n = 0; n < parts->size(); ++n) {
                const Json& part = (*parts)[n];
                const std::optional<ItemIndex> p =
                    part.is_string() ? items.find(part.get<std::string>()) : std::nullopt;
                if (!p) {
                    return slot_where + ".parts[" + std::to_string(n) + "] must be an item id in the item catalog";
                }
                slot.parts.push_back(*p);
            }
            def.slots.push_back(std::move(slot));
        }
        if (std::optional<std::string> error = result.add(items, std::move(def))) {
            return where + ": " + *error;
        }
    }
    weapons = std::move(result);
    return std::nullopt;
}

std::optional<usize> slot_of(const WeaponDef& weapon, ItemIndex part) {
    for (usize i = 0; i < weapon.slots.size(); ++i) {
        const std::vector<ItemIndex>& parts = weapon.slots[i].parts;
        if (std::find(parts.begin(), parts.end(), part) != parts.end()) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::string> check_weapon_parts(const WeaponDef& weapon, const ItemCatalog& items,
                                              std::span<const FittedPart> parts) {
    std::vector<bool> filled(weapon.slots.size(), false);
    for (const FittedPart& p : parts) {
        const std::optional<usize> slot = slot_of(weapon, p.item);
        const std::string name = p.item < items.size() ? items.get(p.item).id : "?";
        if (!slot) {
            return "\"" + name + "\" does not fit \"" + items.get(weapon.weapon).id + "\"";
        }
        if (filled[*slot]) {
            return "two parts in slot \"" + weapon.slots[*slot].name + "\" of \"" +
                   items.get(weapon.weapon).id + "\"";
        }
        filled[*slot] = true;
    }
    return std::nullopt;
}

std::optional<std::string> check_inventory_weapons(const Inventory& inventory, const ItemCatalog& items,
                                                   const WeaponCatalog& weapons) {
    for (const ItemStack& s : inventory.stacks()) {
        if (s.parts.empty()) {
            continue;
        }
        const WeaponDef* def = weapons.find(s.item);
        const std::string where = "stack " + std::to_string(s.id) + ": ";
        if (def == nullptr) {
            return where + "\"" + items.get(s.item).id + "\" has parts but is not a known weapon";
        }
        if (std::optional<std::string> error = check_weapon_parts(*def, items, s.parts)) {
            return where + *error;
        }
    }
    return std::nullopt;
}

float weapon_condition(const WeaponDef& weapon, float frame_condition, std::span<const FittedPart> parts) {
    double sum = clamp01(frame_condition);
    for (usize slot = 0; slot < weapon.slots.size(); ++slot) {
        if (const FittedPart* p = part_in(weapon, parts, slot)) {
            sum += clamp01(p->condition);
        }
    }
    return static_cast<float>(sum / static_cast<double>(weapon.slots.size() + 1));
}

bool complete(const WeaponDef& weapon, std::span<const FittedPart> parts) {
    for (usize slot = 0; slot < weapon.slots.size(); ++slot) {
        if (part_in(weapon, parts, slot) == nullptr) {
            return false;
        }
    }
    return true;
}

u32 add_weapon(Inventory& inventory, const ItemCatalog& items, const WeaponDef& weapon,
               float condition, u64 seed) {
    Random random(seed);
    const float base = clamp01(condition);
    const auto around = [&] { return clamp01(base + 0.15f * random.signed_unit()); };
    const u32 id = inventory.add(items, weapon.weapon, 1, around());
    std::vector<FittedPart> parts;
    for (const WeaponSlot& slot : weapon.slots) {
        GIANT_ASSERT(!slot.parts.empty(), "the weapon comes from a WeaponCatalog");
        parts.push_back({slot.parts.front(), around()});
    }
    const bool ok = inventory.set_parts(items, id, std::move(parts));
    GIANT_ASSERT(ok, "the weapon comes from a WeaponCatalog");
    return id;
}

std::optional<Disassembly> disassemble(Inventory& inventory, const ItemCatalog& items,
                                       const WeaponCatalog& weapons, u32 weapon_stack,
                                       float tool_quality, u64 seed) {
    const ItemStack* stack = inventory.find(weapon_stack);
    if (stack == nullptr || weapons.find(stack->item) == nullptr) {
        return std::nullopt;
    }
    // The frame keeps its stack (and id): only its parts come out.
    const std::vector<FittedPart> parts = stack->parts;
    inventory.set_parts(items, weapon_stack, {});

    Random random(seed);
    const float wear = 0.1f * (1.0f - clamp01(tool_quality));
    Disassembly result;
    for (const FittedPart& p : parts) {
        const float condition = clamp01(p.condition - wear * random.unit());
        if (condition < k_scrap_condition) {
            ++result.parts_scrapped;
        } else {
            inventory.add(items, p.item, 1, condition);
            ++result.parts_kept;
        }
    }
    return result;
}

bool fit_part(Inventory& inventory, const ItemCatalog& items, const WeaponCatalog& weapons,
              u32 weapon_stack, u32 part_stack) {
    const ItemStack* weapon = inventory.find(weapon_stack);
    const ItemStack* part = inventory.find(part_stack);
    if (weapon == nullptr || part == nullptr || weapon_stack == part_stack) {
        return false;
    }
    const WeaponDef* def = weapons.find(weapon->item);
    if (def == nullptr) {
        return false;
    }
    const std::optional<usize> slot = slot_of(*def, part->item);
    if (!slot) {
        return false;
    }
    std::vector<FittedPart> parts = weapon->parts;
    const ItemStack taken = inventory.take(part_stack, 1);
    const auto old = std::find_if(parts.begin(), parts.end(),
                                  [&](const FittedPart& p) { return slot_of(*def, p.item) == slot; });
    if (old != parts.end()) {
        const FittedPart out = *old;
        parts.erase(old);
        inventory.add(items, out.item, 1, out.condition);
    }
    parts.push_back({taken.item, taken.condition});
    const bool ok = inventory.set_parts(items, weapon_stack, std::move(parts));
    GIANT_ASSERT(ok, "the weapon stack holds a worn item");
    return ok;
}

bool strip_part(Inventory& inventory, const ItemCatalog& items, const WeaponCatalog& weapons,
                u32 weapon_stack, usize slot) {
    const ItemStack* weapon = inventory.find(weapon_stack);
    if (weapon == nullptr) {
        return false;
    }
    const WeaponDef* def = weapons.find(weapon->item);
    if (def == nullptr || slot >= def->slots.size()) {
        return false;
    }
    std::vector<FittedPart> parts = weapon->parts;
    const auto it = std::find_if(parts.begin(), parts.end(),
                                 [&](const FittedPart& p) { return slot_of(*def, p.item) == slot; });
    if (it == parts.end()) {
        return false;
    }
    const FittedPart out = *it;
    parts.erase(it);
    // Set the weapon's parts first: adding may move the stack list.
    inventory.set_parts(items, weapon_stack, std::move(parts));
    inventory.add(items, out.item, 1, out.condition);
    return true;
}

} // namespace giant::gameplay
