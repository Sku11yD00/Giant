// Weapons made of parts: definitions, found guns, condition, taking guns
// apart, swapping parts, and saving guns with their parts.
#include "giant/gameplay/weapons.h"

#include "giant_test.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace giant;
using namespace giant::gameplay;

namespace {

struct Armoury {
    ItemCatalog items;
    WeaponCatalog weapons;
    ItemIndex pistol = 0, barrel = 0, slide = 0, trigger = 0, rifle = 0, ammo = 0;

    Armoury() {
        GIANT_CHECK(!items.add({"pm_pistol", "PM pistol", ItemKind::weapon, 0.3f, 900, 1, true}));
        GIANT_CHECK(!items.add({"pistol_barrel", "Pistol barrel", ItemKind::part, 0.12f, 300, 1, true}));
        GIANT_CHECK(!items.add({"pistol_slide", "Pistol slide", ItemKind::part, 0.18f, 260, 1, true}));
        GIANT_CHECK(!items.add({"trigger_group", "Trigger group", ItemKind::part, 0.15f, 350, 1, true}));
        GIANT_CHECK(!items.add({"hunting_rifle", "Rifle", ItemKind::weapon, 2.0f, 4000, 1, true}));
        GIANT_CHECK(!items.add({"ammo_9x18", "9x18", ItemKind::ammo, 0.01f, 6, 120, false}));
        pistol = *items.find("pm_pistol");
        barrel = *items.find("pistol_barrel");
        slide = *items.find("pistol_slide");
        trigger = *items.find("trigger_group");
        rifle = *items.find("hunting_rifle");
        ammo = *items.find("ammo_9x18");
        GIANT_CHECK(!weapons.add(items, {pistol, {{"barrel", {barrel}}, {"slide", {slide}}, {"trigger", {trigger}}}}));
    }
    const WeaponDef& pm() const { return *weapons.find(pistol); }
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

GIANT_TEST("weapons: definitions are checked") {
    Armoury a;
    GIANT_CHECK(a.weapons.find(a.pistol) != nullptr);
    GIANT_CHECK(a.weapons.find(a.rifle) == nullptr);
    WeaponCatalog w;
    GIANT_CHECK(w.add(a.items, {a.ammo, {{"barrel", {a.barrel}}}}).has_value());           // not a weapon
    GIANT_CHECK(w.add(a.items, {a.rifle, {}}).has_value());                                // no slots
    GIANT_CHECK(w.add(a.items, {a.rifle, {{"barrel", {}}}}).has_value());                  // empty slot
    GIANT_CHECK(w.add(a.items, {a.rifle, {{"barrel", {a.ammo}}}}).has_value());            // not a part
    GIANT_CHECK(w.add(a.items, {a.rifle, {{"Barrel", {a.barrel}}}}).has_value());          // bad name
    GIANT_CHECK(w.add(a.items, {a.rifle, {{"a", {a.barrel}}, {"a", {a.slide}}}}).has_value()); // name twice
    GIANT_CHECK(w.add(a.items, {a.rifle, {{"a", {a.barrel}}, {"b", {a.barrel}}}}).has_value()); // part twice
    GIANT_CHECK(w.add(a.items, {a.rifle, {{"a", {99}}}}).has_value());                     // no such item
    GIANT_CHECK(w.weapons().empty());
    GIANT_CHECK(!w.add(a.items, {a.rifle, {{"barrel", {a.barrel}}}}).has_value());
    GIANT_CHECK(w.add(a.items, {a.rifle, {{"barrel", {a.barrel}}}}).has_value()); // twice
    GIANT_CHECK(slot_of(a.pm(), a.slide) == usize{1});
    GIANT_CHECK(!slot_of(a.pm(), a.ammo).has_value());
}

GIANT_TEST("weapons: files save and load back the same, and bad ones say where") {
    Armoury a;
    WeaponCatalog back;
    GIANT_CHECK(!load_weapons(save_weapons(a.weapons, a.items), a.items, back).has_value());
    GIANT_CHECK(back.weapons().size() == 1 && back.weapons()[0] == a.pm());
    const auto fails = [&](const char* text, const char* where) {
        WeaponCatalog w;
        const std::optional<std::string> error = load_weapons(text, a.items, w);
        GIANT_CHECK(error.has_value() && error->find(where) != std::string::npos);
        GIANT_CHECK(w.weapons().empty());
    };
    fails(R"({"format": "giant-weapons", "version": 1, "weapons": [{"item": "ak", "slots": []}]})",
          "weapons[0].item");
    fails(R"({"format": "giant-weapons", "version": 1, "weapons": {}})", "weapons must be an array");
    fails(R"({"format": "giant-weapons", "version": 1, "weapons": [
        {"item": "hunting_rifle", "slots": [{"name": 3, "parts": ["pistol_barrel"]}]}]})",
          "weapons[0].slots[0].name");
    fails(R"({"format": "giant-weapons", "version": 1, "weapons": [
        {"item": "hunting_rifle", "slots": [{"name": "barrel", "parts": ["nope"]}]}]})",
          "weapons[0].slots[0].parts[0]");
    fails(R"({"format": "giant-weapons", "version": 1, "weapons": [
        {"item": "hunting_rifle", "slots": [{"name": "barrel", "parts": "pistol_barrel"}]}]})",
          "weapons[0].slots[0].parts");
    fails(R"({"format": "giant-weapons", "version": 1, "weapons": [
        {"item": "hunting_rifle", "slots": [{"name": "barrel", "parts": ["pistol_barrel"], "x": 1}]}]})",
          "weapons[0].slots[0].x");
    fails(R"({"format": "giant-weapons", "version": 1, "weapons": [
        {"item": "hunting_rifle", "slots": [{"name": "barrel", "parts": ["ammo_9x18"]}]}]})",
          "weapons[0]: ");
}

GIANT_TEST("weapons: found guns get every part near their condition, the same for a seed") {
    Armoury a;
    Inventory inv;
    const u32 gun = add_weapon(inv, a.items, a.pm(), 0.4f, 7);
    const ItemStack* s = inv.find(gun);
    GIANT_CHECK(s != nullptr && s->item == a.pistol && s->parts.size() == 3);
    GIANT_CHECK(complete(a.pm(), s->parts));
    GIANT_CHECK(s->condition >= 0.25f && s->condition <= 0.55f);
    for (const FittedPart& p : s->parts) {
        GIANT_CHECK(p.condition >= 0.25f && p.condition <= 0.55f);
    }
    Inventory again;
    const u32 same = add_weapon(again, a.items, a.pm(), 0.4f, 7);
    GIANT_CHECK(again.find(same)->parts == s->parts && again.find(same)->condition == s->condition);
    const u32 other = add_weapon(again, a.items, a.pm(), 0.4f, 8);
    GIANT_CHECK(again.find(other)->parts != s->parts);
    // Conditions stay in 0..1 at the ends.
    const u32 wreck = add_weapon(inv, a.items, a.pm(), 0.0f, 3);
    for (const FittedPart& p : inv.find(wreck)->parts) {
        GIANT_CHECK(p.condition >= 0.0f && p.condition <= 0.15f);
    }
}

GIANT_TEST("weapons: condition is the mean of frame and parts, a missing part counting 0") {
    Armoury a;
    const std::vector<FittedPart> all{{a.barrel, 0.8f}, {a.slide, 0.4f}, {a.trigger, 0.6f}};
    GIANT_CHECK_NEAR(weapon_condition(a.pm(), 0.2f, all), 0.5, 1e-6);
    const std::vector<FittedPart> two{{a.barrel, 0.8f}, {a.trigger, 0.6f}};
    GIANT_CHECK_NEAR(weapon_condition(a.pm(), 0.2f, two), 0.4, 1e-6);
    GIANT_CHECK(complete(a.pm(), all));
    GIANT_CHECK(!complete(a.pm(), two));
}

GIANT_TEST("weapons: taking a gun apart gives the frame and parts back, scrapping wrecks") {
    Armoury a;
    Inventory inv;
    const u32 gun = inv.add(a.items, a.pistol, 1, 0.5f);
    inv.set_parts(a.items, gun, {{a.barrel, 0.9f}, {a.slide, 0.05f}, {a.trigger, 0.6f}});
    const std::optional<Disassembly> d = disassemble(inv, a.items, a.weapons, gun, 1.0f, 1);
    GIANT_CHECK(d.has_value() && d->parts_kept == 2 && d->parts_scrapped == 1);
    // The frame keeps its stack, now bare.
    GIANT_CHECK(inv.find(gun) != nullptr && inv.find(gun)->parts.empty());
    GIANT_CHECK(inv.count(a.pistol) == 1 && inv.count(a.barrel) == 1 && inv.count(a.trigger) == 1);
    GIANT_CHECK(inv.count(a.slide) == 0);
    for (const ItemStack& s : inv.stacks()) {
        if (s.item == a.pistol) {
            GIANT_CHECK(s.parts.empty() && s.condition == 0.5f);
        }
        if (s.item == a.barrel) {
            GIANT_CHECK(s.condition == 0.9f); // expert tools: no wear
        }
    }
    // Bare hands wear the parts, by at most 0.1.
    Inventory rough;
    const u32 g2 = rough.add(a.items, a.pistol, 1, 0.5f);
    rough.set_parts(a.items, g2, {{a.barrel, 0.9f}, {a.slide, 0.9f}, {a.trigger, 0.9f}});
    GIANT_CHECK(disassemble(rough, a.items, a.weapons, g2, 0.0f, 5)->parts_kept == 3);
    float lost = 0.0f;
    for (const ItemStack& s : rough.stacks()) {
        if (s.item != a.pistol) {
            GIANT_CHECK(s.condition <= 0.9f && s.condition >= 0.8f);
            lost += 0.9f - s.condition;
        }
    }
    GIANT_CHECK(lost > 0.0f);
    // Half-good tools wear half as much at most.
    Inventory half;
    const u32 g3 = half.add(a.items, a.pistol, 1, 0.5f);
    half.set_parts(a.items, g3, {{a.barrel, 0.9f}, {a.slide, 0.9f}, {a.trigger, 0.9f}});
    GIANT_CHECK(disassemble(half, a.items, a.weapons, g3, 0.5f, 5)->parts_kept == 3);
    for (const ItemStack& s : half.stacks()) {
        if (s.item != a.pistol) {
            GIANT_CHECK(s.condition <= 0.9f && s.condition >= 0.85f);
        }
    }
    // Not a weapon, or no such stack.
    const u32 rounds = inv.add(a.items, a.ammo, 10);
    GIANT_CHECK(!disassemble(inv, a.items, a.weapons, rounds, 1.0f, 1).has_value());
    GIANT_CHECK(!disassemble(inv, a.items, a.weapons, 999, 1.0f, 1).has_value());
}

GIANT_TEST("weapons: fitting a better part swaps the old one out") {
    Armoury a;
    Inventory inv;
    const u32 gun = inv.add(a.items, a.pistol, 1, 0.5f);
    inv.set_parts(a.items, gun, {{a.barrel, 0.2f}, {a.slide, 0.5f}, {a.trigger, 0.5f}});
    const u32 good = inv.add(a.items, a.barrel, 1, 0.95f);
    const float before = weapon_condition(a.pm(), 0.5f, inv.find(gun)->parts);
    GIANT_CHECK(fit_part(inv, a.items, a.weapons, gun, good));
    GIANT_CHECK(inv.find(good) == nullptr);
    GIANT_CHECK(weapon_condition(a.pm(), 0.5f, inv.find(gun)->parts) > before);
    GIANT_CHECK(inv.find(gun)->parts.size() == 3);
    // The worn barrel came out as an item.
    bool old_out = false;
    for (const ItemStack& s : inv.stacks()) {
        old_out = old_out || (s.item == a.barrel && s.condition == 0.2f);
    }
    GIANT_CHECK(old_out);
    // A part that does not fit, a weapon that is not one, or the same stack.
    const u32 rounds = inv.add(a.items, a.ammo, 5);
    GIANT_CHECK(!fit_part(inv, a.items, a.weapons, gun, rounds));
    GIANT_CHECK(!fit_part(inv, a.items, a.weapons, rounds, gun));
    GIANT_CHECK(!fit_part(inv, a.items, a.weapons, gun, gun));
}

GIANT_TEST("weapons: stripping a part leaves the gun incomplete, fitting it back completes it") {
    Armoury a;
    Inventory inv;
    const u32 gun = add_weapon(inv, a.items, a.pm(), 0.7f, 11);
    GIANT_CHECK(strip_part(inv, a.items, a.weapons, gun, 1));
    GIANT_CHECK(!complete(a.pm(), inv.find(gun)->parts));
    GIANT_CHECK(!strip_part(inv, a.items, a.weapons, gun, 1)); // already empty
    GIANT_CHECK(!strip_part(inv, a.items, a.weapons, gun, 9)); // no such slot
    const u32 rounds = inv.add(a.items, a.ammo, 3);
    GIANT_CHECK(!strip_part(inv, a.items, a.weapons, rounds, 0)); // not a weapon
    GIANT_CHECK(!strip_part(inv, a.items, a.weapons, 999, 0));    // no such stack
    inv.take(rounds, 3);
    u32 slide = 0;
    for (const ItemStack& s : inv.stacks()) {
        slide = s.item == a.slide ? s.id : slide;
    }
    GIANT_CHECK(slide != 0);
    GIANT_CHECK(fit_part(inv, a.items, a.weapons, gun, slide));
    GIANT_CHECK(complete(a.pm(), inv.find(gun)->parts));
    GIANT_CHECK(inv.stacks().size() == 1);
}

GIANT_TEST("weapons: guns keep their parts when moved, weighed and saved") {
    Armoury a;
    Inventory body;
    Inventory pack;
    const u32 gun = add_weapon(body, a.items, a.pm(), 0.5f, 2);
    const std::vector<FittedPart> parts = body.find(gun)->parts;
    GIANT_CHECK_NEAR(body.weight_kg(a.items), 0.3 + 0.12 + 0.18 + 0.15, 1e-5);
    GIANT_CHECK(Inventory::move(a.items, body, gun, 1, pack) == 1);
    GIANT_CHECK(pack.stacks().size() == 1 && pack.stacks()[0].parts == parts);

    Inventory back;
    GIANT_CHECK(!load_inventory(save_inventory(pack, a.items), a.items, back).has_value());
    GIANT_CHECK(back == pack);

    const auto fails = [&](const char* text, const char* where) {
        Inventory inv;
        const std::optional<std::string> error = load_inventory(text, a.items, inv);
        GIANT_CHECK(error.has_value() && error->find(where) != std::string::npos);
    };
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 1, "item": "ammo_9x18", "parts": [{"item": "pistol_barrel"}]}]})",
          "stacks[0].parts");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 1, "item": "pm_pistol", "parts": [{"item": "ammo_9x18"}]}]})",
          "stacks[0].parts[0].item");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 1, "item": "pm_pistol", "parts": [{"item": "pistol_barrel", "condition": 2}]}]})",
          "stacks[0].parts[0].condition");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 1, "item": "pm_pistol", "parts": {"item": "pistol_barrel"}}]})",
          "stacks[0].parts");
    // Parts inside a part would be lost when it is fitted: only weapons hold parts.
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 1, "item": "pistol_slide", "parts": [{"item": "pistol_barrel"}]}]})",
          "only a weapon");
}

GIANT_TEST("weapons: parts that do not fit their gun are caught after loading") {
    Armoury a;
    const auto check = [&](const char* text) {
        Inventory inv;
        GIANT_CHECK(!load_inventory(text, a.items, inv).has_value());
        return check_inventory_weapons(inv, a.items, a.weapons);
    };
    GIANT_CHECK(!check(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 1, "item": "pm_pistol", "parts": [{"item": "pistol_barrel"}, {"item": "trigger_group"}]}]})"));
    const std::optional<std::string> twice = check(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 1, "item": "pm_pistol", "parts": [{"item": "pistol_barrel"}, {"item": "pistol_barrel"}]}]})");
    GIANT_CHECK(twice.has_value() && twice->find("two parts in slot \"barrel\"") != std::string::npos);
    // The rifle has no definition here, so it cannot hold parts.
    const std::optional<std::string> unknown = check(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 1, "item": "hunting_rifle", "parts": [{"item": "pistol_barrel"}]}]})");
    GIANT_CHECK(unknown.has_value() && unknown->find("not a known weapon") != std::string::npos);
    WeaponCatalog more = a.weapons;
    GIANT_CHECK(!more.add(a.items, {a.rifle, {{"bolt", {a.slide}}}}));
    Inventory inv;
    GIANT_CHECK(!load_inventory(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 1, "item": "hunting_rifle", "parts": [{"item": "pistol_barrel"}]}]})",
                                a.items, inv));
    const std::optional<std::string> misfit = check_inventory_weapons(inv, a.items, more);
    GIANT_CHECK(misfit.has_value() && misfit->find("does not fit") != std::string::npos);
}

GIANT_TEST("weapons: only weapons take parts, and put brings a gun back whole") {
    Armoury a;
    Inventory inv;
    const u32 slide = inv.add(a.items, a.slide, 1, 0.5f);
    GIANT_CHECK(!inv.set_parts(a.items, slide, {{a.barrel, 1.0f}}));
    GIANT_CHECK(!inv.set_parts(a.items, 999, {}));
    const u32 gun = inv.add(a.items, a.pistol, 1, 0.5f);
    GIANT_CHECK(inv.set_parts(a.items, gun, {{a.barrel, 4.0f}, {a.slide, -1.0f}}));
    GIANT_CHECK(inv.find(gun)->parts[0].condition == 1.0f && inv.find(gun)->parts[1].condition == 0.0f);
    const ItemStack taken = inv.take(gun, 1);
    Inventory other;
    const u32 back = other.put(a.items, taken);
    GIANT_CHECK(other.find(back)->parts == taken.parts && other.find(back)->condition == 0.5f);
}

GIANT_TEST("weapons: rolls are pinned, so every platform finds the same guns") {
    // splitmix64 from seed 0; any change to the generator shows here.
    Inventory inv;
    Armoury a;
    const u32 gun = add_weapon(inv, a.items, a.pm(), 0.5f, 0);
    const ItemStack* s = inv.find(gun);
    // Worked out separately (numpy float32, no fused multiply-add).
    GIANT_CHECK(s->condition == 0.614993215f);
    GIANT_CHECK(s->parts[0].condition == 0.479458392f);
    GIANT_CHECK(s->parts[1].condition == 0.357930124f);
    GIANT_CHECK(s->parts[2].condition == 0.641264558f);
}

GIANT_TEST("weapons: the game's weapon files load against its items") {
    ItemCatalog items;
    WeaponCatalog weapons;
    const std::filesystem::path dir = GIANT_GAME_DATA_DIR;
    GIANT_CHECK(!load_items(read_file(dir / "ring1.items.json"), items).has_value());
    const std::optional<std::string> error = load_weapons(read_file(dir / "ring1.weapons.json"), items, weapons);
    if (error) {
        std::printf("%s\n", error->c_str());
    }
    GIANT_CHECK(!error.has_value());
    // Every weapon item in the game has a definition.
    for (usize i = 0; i < items.size(); ++i) {
        if (items.get(static_cast<ItemIndex>(i)).kind == ItemKind::weapon) {
            GIANT_CHECK(weapons.find(static_cast<ItemIndex>(i)) != nullptr);
        }
    }
}
