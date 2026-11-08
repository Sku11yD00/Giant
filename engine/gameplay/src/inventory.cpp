#include "giant/gameplay/inventory.h"

#include "giant/core/assert.h"
#include "text_file.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace giant::gameplay {

namespace {

float clamp_condition(float condition) {
    // NaN becomes 0: a broken value must not spread into saves. Adding 0
    // turns -0 into 0.
    return std::isfinite(condition) ? std::clamp(condition, 0.0f, 1.0f) + 0.0f : 0.0f;
}

} // namespace

u32 Inventory::new_stack(ItemIndex item, u32 count, float condition) {
    // Saved inventories start below k_max_stack_id, leaving three billion
    // more ids: running out is a bug (something adding stacks in a loop).
    GIANT_ASSERT(next_id_ != std::numeric_limits<u32>::max(), "inventory ran out of stack ids");
    const u32 id = next_id_++;
    stacks_.push_back({id, item, count, condition});
    return id;
}

u32 Inventory::add(const ItemCatalog& catalog, ItemIndex item, u32 count, float condition) {
    GIANT_ASSERT(count <= k_max_add_count, "too many items added at once");
    const ItemDef& def = catalog.get(item);
    const float c = def.worn ? clamp_condition(condition) : 1.0f;
    u32 last = 0;
    if (def.max_stack > 1) {
        for (ItemStack& s : stacks_) {
            if (count == 0) {
                break;
            }
            if (s.item == item && s.count < def.max_stack) {
                const u32 n = std::min(count, def.max_stack - s.count);
                s.count += n;
                count -= n;
                last = s.id;
            }
        }
    }
    while (count > 0) {
        const u32 n = std::min(count, def.max_stack);
        last = new_stack(item, n, c);
        count -= n;
    }
    return last;
}

u32 Inventory::remove(ItemIndex item, u32 count) {
    u32 taken = 0;
    while (taken < count) {
        // The worst condition first, then the newest stack (the last made).
        auto worst = stacks_.end();
        for (auto it = stacks_.begin(); it != stacks_.end(); ++it) {
            if (it->item == item && (worst == stacks_.end() || it->condition <= worst->condition)) {
                worst = it;
            }
        }
        if (worst == stacks_.end()) {
            break;
        }
        const u32 n = std::min(count - taken, worst->count);
        worst->count -= n;
        taken += n;
        if (worst->count == 0) {
            stacks_.erase(worst);
        }
    }
    return taken;
}

ItemStack Inventory::take(u32 stack_id, u32 count) {
    const auto it = std::find_if(stacks_.begin(), stacks_.end(),
                                 [&](const ItemStack& s) { return s.id == stack_id; });
    if (it == stacks_.end() || count == 0) {
        return {0, 0, 0, 1.0f};
    }
    ItemStack out = *it;
    out.count = std::min(count, it->count);
    it->count -= out.count;
    if (it->count == 0) {
        stacks_.erase(it);
    }
    return out;
}

u32 Inventory::move(const ItemCatalog& catalog, Inventory& from, u32 stack_id, u32 count,
                    Inventory& to) {
    if (&from == &to) {
        return 0;
    }
    const ItemStack taken = from.take(stack_id, count);
    if (taken.count > 0) {
        to.add(catalog, taken.item, taken.count, taken.condition);
    }
    return taken.count;
}

bool Inventory::set_condition(const ItemCatalog& catalog, u32 stack_id, float condition) {
    for (ItemStack& s : stacks_) {
        if (s.id == stack_id) {
            if (!catalog.get(s.item).worn) {
                return false;
            }
            s.condition = clamp_condition(condition);
            return true;
        }
    }
    return false;
}

const ItemStack* Inventory::find(u32 stack_id) const {
    const auto it = std::find_if(stacks_.begin(), stacks_.end(),
                                 [&](const ItemStack& s) { return s.id == stack_id; });
    return it == stacks_.end() ? nullptr : &*it;
}

u64 Inventory::count(ItemIndex item) const {
    u64 n = 0;
    for (const ItemStack& s : stacks_) {
        if (s.item == item) {
            n += s.count;
        }
    }
    return n;
}

void Inventory::clear() {
    stacks_.clear();
}

float Inventory::weight_kg(const ItemCatalog& catalog) const {
    double kg = 0.0;
    for (const ItemStack& s : stacks_) {
        kg += static_cast<double>(catalog.get(s.item).weight_kg) * s.count;
    }
    return static_cast<float>(kg);
}

Burden burden(float weight_kg, const CarryLimits& limits) {
    if (!(weight_kg <= limits.max_kg)) {
        return Burden::overloaded;
    }
    return weight_kg > limits.comfortable_kg ? Burden::heavy : Burden::light;
}

std::string save_inventory(const Inventory& inventory, const ItemCatalog& catalog) {
    Json stacks = Json::array();
    for (const ItemStack& s : inventory.stacks()) {
        Json j = {{"id", s.id}, {"item", catalog.get(s.item).id}};
        if (s.count != 1) {
            j["count"] = s.count;
        }
        if (s.condition != 1.0f) {
            j["condition"] = short_float(s.condition);
        }
        stacks.push_back(std::move(j));
    }
    return text_file_header("giant-inventory",
                            {{"next_id", inventory.next_id_}, {"stacks", std::move(stacks)}})
               .dump(2) +
           "\n";
}

std::optional<std::string> load_inventory(std::string_view text, const ItemCatalog& catalog,
                                          Inventory& out) {
    Json root;
    if (std::optional<std::string> error = parse_text_file(text, "giant-inventory", {"next_id", "stacks"}, root)) {
        return error;
    }
    const auto stacks = root.find("stacks");
    if (stacks == root.end() || !stacks->is_array()) {
        return std::string("stacks must be an array");
    }
    Inventory result;
    if (const auto next = root.find("next_id"); next != root.end()) {
        if (!next->is_number_integer() || next->get<i64>() < 1 || next->get<i64>() > i64{k_max_stack_id}) {
            return "next_id must be 1 to " + std::to_string(k_max_stack_id);
        }
        result.next_id_ = static_cast<u32>(next->get<i64>());
    }
    std::unordered_set<u32> ids;
    for (usize i = 0; i < stacks->size(); ++i) {
        const std::string where = "stacks[" + std::to_string(i) + "]";
        FieldReader r((*stacks)[i], where);
        ItemStack s;
        std::string item;
        r.whole("id", s.id, true);
        r.string("item", item, true);
        r.whole("count", s.count, false);
        r.number("condition", s.condition, false);
        r.finish();
        if (r.error()) {
            return r.error();
        }
        const std::optional<ItemIndex> index = catalog.find(item);
        if (!index) {
            return where + ".item \"" + item + "\" is not in the item catalog";
        }
        const ItemDef& def = catalog.get(*index);
        s.item = *index;
        if (s.id == 0 || s.id >= k_max_stack_id || !ids.insert(s.id).second) {
            return where + ".id must be 1 to " + std::to_string(k_max_stack_id - 1) + " and used once";
        }
        if (s.count < 1 || s.count > def.max_stack) {
            return where + ".count must be 1 to " + std::to_string(def.max_stack) + " for \"" + item + "\"";
        }
        if (s.condition < 0.0f || s.condition > 1.0f || (!def.worn && s.condition != 1.0f)) {
            return where + ".condition must be 0 to 1 (and 1 for an item that is not worn)";
        }
        s.condition += 0.0f; // -0 is 0
        result.stacks_.push_back(s);
        result.next_id_ = std::max(result.next_id_, s.id + 1);
    }
    out = std::move(result);
    return std::nullopt;
}

} // namespace giant::gameplay
