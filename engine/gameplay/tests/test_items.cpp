// Item catalogs: checks, lookups, item files, and the game's own files.
#include "giant/gameplay/items.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::gameplay;

namespace {

ItemDef pistol() {
    return {"pm_pistol", "PM pistol", ItemKind::weapon, 0.73f, 900, 1, true};
}
ItemDef ammo() {
    return {"ammo_9x18", "9x18 rounds", ItemKind::ammo, 0.01f, 6, 120, false};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

GIANT_TEST("items: a catalog finds items by id and refuses bad or repeated ones") {
    ItemCatalog c;
    GIANT_CHECK(!c.add(pistol()).has_value());
    GIANT_CHECK(!c.add(ammo()).has_value());
    GIANT_CHECK(c.size() == 2);
    GIANT_CHECK(c.find("ammo_9x18") == ItemIndex{1});
    GIANT_CHECK(c.get(*c.find("pm_pistol")) == pistol());
    GIANT_CHECK(!c.find("ak").has_value());

    GIANT_CHECK(c.add(pistol()).has_value()); // twice
    ItemDef bad = ammo();
    bad.id = "Ammo 9x18";
    GIANT_CHECK(c.add(bad).has_value());
    bad.id = "";
    GIANT_CHECK(c.add(bad).has_value());
    bad = ammo();
    bad.id = "x";
    bad.name = "";
    GIANT_CHECK(c.add(bad).has_value());
    bad = ammo();
    bad.id = "x";
    bad.weight_kg = -1.0f;
    GIANT_CHECK(c.add(bad).has_value());
    bad.weight_kg = 0.1f;
    bad.max_stack = 0;
    GIANT_CHECK(c.add(bad).has_value());
    bad.max_stack = 10001;
    GIANT_CHECK(c.add(bad).has_value());
    bad.max_stack = 5;
    bad.worn = true; // worn items cannot stack
    GIANT_CHECK(c.add(bad).has_value());
    bad = ammo();
    bad.id = "x";
    bad.weight_kg = 1001.0f;
    GIANT_CHECK(c.add(bad).has_value());
    bad.weight_kg = std::nanf("");
    GIANT_CHECK(c.add(bad).has_value());
    bad = ammo();
    bad.id = "x";
    bad.name = "\xff\xfe"; // not UTF-8: saving it would fail
    GIANT_CHECK(c.add(bad).has_value());
    bad.name = "\xc0\xaf"; // overlong
    GIANT_CHECK(c.add(bad).has_value());
    bad.name = "\xe2\x82"; // cut short
    GIANT_CHECK(c.add(bad).has_value());
    bad.name = "Ma\xc3\xb1" "ana \xe2\x82\xac \xf0\x9f\x94\xab"; // fine
    GIANT_CHECK(!c.add(bad).has_value());
    GIANT_CHECK(c.size() == 3);
    ItemCatalog back;
    GIANT_CHECK(!load_items(save_items(c), back).has_value());
    GIANT_CHECK(back.get(2).name == bad.name);
}

GIANT_TEST("items: kinds have names both ways") {
    for (const char* name : {"weapon", "part", "ammo", "armour", "tool", "consumable", "relic", "misc"}) {
        const std::optional<ItemKind> k = item_kind(name);
        GIANT_CHECK(k.has_value() && item_kind_name(*k) == name);
    }
    GIANT_CHECK(!item_kind("armor").has_value());
}

GIANT_TEST("items: files save and load back the same") {
    ItemCatalog c;
    GIANT_CHECK(!c.add(pistol()).has_value());
    GIANT_CHECK(!c.add(ammo()).has_value());
    ItemCatalog back;
    GIANT_CHECK(!load_items(save_items(c), back).has_value());
    GIANT_CHECK(back.size() == 2);
    GIANT_CHECK(back.get(0) == c.get(0));
    GIANT_CHECK(back.get(1) == c.get(1));
    GIANT_CHECK(save_items(back) == save_items(c));
    // Weights are written as people would write them.
    GIANT_CHECK(save_items(c).find("\"weight_kg\": 0.73,") != std::string::npos);
    GIANT_CHECK(save_items(c).find("\"weight_kg\": 0.01,") != std::string::npos);
}

GIANT_TEST("items: a bad file says where and changes nothing") {
    ItemCatalog c;
    GIANT_CHECK(!c.add(pistol()).has_value());
    const auto fails = [&](const char* text, const char* where) {
        const std::optional<std::string> error = load_items(text, c);
        GIANT_CHECK(error.has_value() && error->find(where) != std::string::npos);
        GIANT_CHECK(c.size() == 1);
    };
    fails("not json", "not valid JSON");
    fails(R"({"format": "giant-scene", "version": 1, "items": []})", "format");
    fails(R"({"format": "giant-items", "version": 2, "items": []})", "version");
    fails(R"({"format": "giant-items", "version": 1})", "items");
    // The first item is fine; the second is wrong, so neither is added.
    fails(R"({"format": "giant-items", "version": 1, "items": [
        {"id": "a", "name": "A", "kind": "misc", "weight_kg": 1, "value": 1},
        {"id": "b", "name": "B", "kind": "gizmo", "weight_kg": 1, "value": 1}]})",
          "items[1].kind");
    fails(R"({"format": "giant-items", "version": 1, "items": [
        {"id": "a", "name": "A", "kind": "misc", "weight_kg": "heavy", "value": 1}]})",
          "items[0].weight_kg");
    fails(R"({"format": "giant-items", "version": 1, "items": [
        {"id": "a", "name": "A", "kind": "misc", "weight_kg": 1, "value": -5}]})",
          "items[0].value");
    fails(R"({"format": "giant-items", "version": 1, "items": [
        {"id": "a", "name": "A", "kind": "misc", "weight_kg": 1, "value": 1, "wieght": 2}]})",
          "items[0].wieght");
    fails(R"({"format": "giant-items", "version": 1, "items": [
        {"id": "pm_pistol", "name": "A", "kind": "misc", "weight_kg": 1, "value": 1}]})",
          "defined twice");
    fails(R"({"format": "giant-items", "version": 1, "items": [], "itemz": []})", "itemz");
    fails(R"({"format": "giant-items", "version": 1, "items": [
        {"id": "a", "id": "b", "name": "A", "kind": "misc", "weight_kg": 1, "value": 1}]})",
          "\"id\" is given twice");
    // A number too large for a double is an error, not a crash.
    fails(R"({"format": "giant-items", "version": 1, "items": [
        {"id": "a", "name": "A", "kind": "misc", "weight_kg": 1e999, "value": 1}]})",
          "not valid JSON");
}

GIANT_TEST("items: the game's item files load into one catalog") {
    ItemCatalog c;
    const std::filesystem::path dir = GIANT_GAME_DATA_DIR;
    GIANT_CHECK(std::filesystem::is_directory(dir));
    if (!std::filesystem::is_directory(dir)) {
        return;
    }
    // In name order, as the game loads them (item indices follow it).
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    usize files = 0;
    for (const std::filesystem::path& path : paths) {
        const std::string name = path.filename().string();
        if (name.size() > 11 && name.ends_with(".items.json")) {
            const std::optional<std::string> error = load_items(read_file(path), c);
            if (error) {
                std::printf("%s: %s\n", name.c_str(), error->c_str());
            }
            GIANT_CHECK(!error.has_value());
            ++files;
        }
    }
    GIANT_CHECK(files > 0);
    GIANT_CHECK(c.find("pm_pistol").has_value());
}
