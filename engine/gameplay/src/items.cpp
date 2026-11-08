#include "giant/gameplay/items.h"

#include "giant/core/assert.h"
#include "text_file.h"

#include <array>
#include <cmath>
#include <utility>

namespace giant::gameplay {

namespace {

constexpr std::array<std::pair<ItemKind, std::string_view>, 8> k_kind_names{{
    {ItemKind::weapon, "weapon"},
    {ItemKind::part, "part"},
    {ItemKind::ammo, "ammo"},
    {ItemKind::armour, "armour"},
    {ItemKind::tool, "tool"},
    {ItemKind::consumable, "consumable"},
    {ItemKind::relic, "relic"},
    {ItemKind::misc, "misc"},
}};

constexpr usize k_max_id_length = 64;
constexpr float k_max_weight_kg = 1000.0f;
constexpr u32 k_max_stack = 10000;

// Well-formed UTF-8 (JSON text must be; a bad name would fail at saving).
bool valid_utf8(std::string_view text) {
    usize i = 0;
    while (i < text.size()) {
        const auto c = static_cast<u8>(text[i]);
        usize more = 0;
        u32 code = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            more = 1;
            code = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            more = 2;
            code = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            more = 3;
            code = c & 0x07u;
        } else {
            return false;
        }
        if (more > text.size() - i - 1) {
            return false;
        }
        for (usize k = 1; k <= more; ++k) {
            const auto next = static_cast<u8>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3Fu);
        }
        // Overlong forms, surrogates and code points past U+10FFFF.
        const u32 min_code = more == 1 ? 0x80u : more == 2 ? 0x800u : 0x10000u;
        if (code < min_code || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) {
            return false;
        }
        i += more + 1;
    }
    return true;
}

bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > k_max_id_length) {
        return false;
    }
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<ItemKind> item_kind(std::string_view name) {
    for (const auto& [kind, kind_name] : k_kind_names) {
        if (kind_name == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string_view item_kind_name(ItemKind kind) {
    for (const auto& [k, name] : k_kind_names) {
        if (k == kind) {
            return name;
        }
    }
    GIANT_ASSERT(false, "unknown item kind");
    return "misc";
}

std::optional<std::string> check_item(const ItemDef& item) {
    if (!valid_id(item.id)) {
        return "id \"" + item.id + "\" must be 1 to 64 lowercase letters, digits or '_'";
    }
    if (item.name.empty() || !valid_utf8(item.name)) {
        return "item \"" + item.id + "\" needs a name (UTF-8 text)";
    }
    if (!std::isfinite(item.weight_kg) || item.weight_kg < 0.0f || item.weight_kg > k_max_weight_kg) {
        return "item \"" + item.id + "\": weight_kg must be 0 to 1000";
    }
    if (item.max_stack < 1 || item.max_stack > k_max_stack) {
        return "item \"" + item.id + "\": max_stack must be 1 to 10000";
    }
    if (item.worn && item.max_stack != 1) {
        return "item \"" + item.id + "\": a worn item cannot stack (max_stack must be 1)";
    }
    return std::nullopt;
}

std::optional<std::string> ItemCatalog::add(ItemDef item) {
    if (std::optional<std::string> error = check_item(item)) {
        return error;
    }
    if (by_id_.contains(item.id)) {
        return "item \"" + item.id + "\" is defined twice";
    }
    const auto index = static_cast<ItemIndex>(items_.size());
    by_id_.emplace(item.id, index);
    items_.push_back(std::move(item));
    return std::nullopt;
}

std::optional<ItemIndex> ItemCatalog::find(std::string_view id) const {
    // unordered_map<std::string> has no heterogeneous lookup without a
    // transparent hash; ids are short, so the copy is cheap.
    const auto it = by_id_.find(std::string(id));
    if (it == by_id_.end()) {
        return std::nullopt;
    }
    return it->second;
}

const ItemDef& ItemCatalog::get(ItemIndex index) const {
    GIANT_ASSERT(index < items_.size(), "item index out of range");
    return items_[index];
}

std::string save_items(const ItemCatalog& catalog) {
    Json items = Json::array();
    for (const ItemDef& item : catalog.items()) {
        Json j = {
            {"id", item.id},
            {"name", item.name},
            {"kind", std::string(item_kind_name(item.kind))},
            {"weight_kg", short_float(item.weight_kg)},
            {"value", item.value},
        };
        if (item.max_stack != 1) {
            j["max_stack"] = item.max_stack;
        }
        if (item.worn) {
            j["worn"] = true;
        }
        items.push_back(std::move(j));
    }
    return text_file_header("giant-items", {{"items", std::move(items)}}).dump(2) + "\n";
}

std::optional<std::string> load_items(std::string_view text, ItemCatalog& catalog) {
    Json root;
    if (std::optional<std::string> error = parse_text_file(text, "giant-items", {"items"}, root)) {
        return error;
    }
    const Json* items = root.contains("items") ? &root["items"] : nullptr;
    if (items == nullptr || !items->is_array()) {
        return std::string("items must be an array");
    }
    // Into a copy, so a bad file leaves the catalog as it was.
    ItemCatalog result = catalog;
    for (usize i = 0; i < items->size(); ++i) {
        const std::string where = "items[" + std::to_string(i) + "]";
        FieldReader r((*items)[i], where);
        ItemDef item;
        std::string kind;
        r.string("id", item.id, true);
        r.string("name", item.name, true);
        r.string("kind", kind, true);
        r.number("weight_kg", item.weight_kg, true);
        r.whole("value", item.value, true);
        r.whole("max_stack", item.max_stack, false);
        r.boolean("worn", item.worn, false);
        r.finish();
        if (r.error()) {
            return r.error();
        }
        const std::optional<ItemKind> k = item_kind(kind);
        if (!k) {
            return where + ".kind \"" + kind + "\" is not an item kind";
        }
        item.kind = *k;
        if (std::optional<std::string> error = result.add(std::move(item))) {
            return where + ": " + *error;
        }
    }
    catalog = std::move(result);
    return std::nullopt;
}

} // namespace giant::gameplay
