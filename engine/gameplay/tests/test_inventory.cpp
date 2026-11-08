// Inventories: stacking, taking, moving, weight, burden and saving.
#include "giant/gameplay/inventory.h"

#include "giant_test.h"

#include <cmath>
#include <string>

using namespace giant;
using namespace giant::gameplay;

namespace {

struct Items {
    ItemCatalog catalog;
    ItemIndex pistol = 0, ammo = 0, bandage = 0, note = 0;

    Items() {
        GIANT_CHECK(!catalog.add({"pm_pistol", "PM pistol", ItemKind::weapon, 0.75f, 900, 1, true}));
        GIANT_CHECK(!catalog.add({"ammo_9x18", "9x18 rounds", ItemKind::ammo, 0.01f, 6, 120, false}));
        GIANT_CHECK(!catalog.add({"bandage", "Bandage", ItemKind::consumable, 0.05f, 60, 10, false}));
        GIANT_CHECK(!catalog.add({"stash_note", "Note", ItemKind::misc, 0.0f, 0, 1, false}));
        pistol = *catalog.find("pm_pistol");
        ammo = *catalog.find("ammo_9x18");
        bandage = *catalog.find("bandage");
        note = *catalog.find("stash_note");
    }
};

} // namespace

GIANT_TEST("inventory: stackable items fill stacks up to their limit, then start new ones") {
    Items it;
    Inventory inv;
    inv.add(it.catalog, it.ammo, 100);
    inv.add(it.catalog, it.ammo, 50); // 120 + 30
    GIANT_CHECK(inv.stacks().size() == 2);
    GIANT_CHECK(inv.stacks()[0].count == 120 && inv.stacks()[1].count == 30);
    GIANT_CHECK(inv.count(it.ammo) == 150);
    inv.add(it.catalog, it.ammo, 300); // fills 30 -> 120, then 120, 90
    GIANT_CHECK(inv.count(it.ammo) == 450);
    GIANT_CHECK(inv.stacks().size() == 4);
    GIANT_CHECK(inv.add(it.catalog, it.ammo, 0) == 0);
    // Stackable items ignore condition.
    inv.add(it.catalog, it.bandage, 3, 0.2f);
    GIANT_CHECK(inv.stacks().back().condition == 1.0f);
}

GIANT_TEST("inventory: worn items get a stack each, their condition clamped") {
    Items it;
    Inventory inv;
    inv.add(it.catalog, it.pistol, 2, 0.4f);
    inv.add(it.catalog, it.pistol, 1, 7.0f);
    inv.add(it.catalog, it.pistol, 1, -1.0f);
    GIANT_CHECK(inv.stacks().size() == 4);
    GIANT_CHECK(inv.stacks()[0].condition == 0.4f && inv.stacks()[1].condition == 0.4f);
    GIANT_CHECK(inv.stacks()[2].condition == 1.0f);
    GIANT_CHECK(inv.stacks()[3].condition == 0.0f);
    // NaN is no condition: it becomes 0, never a NaN in a save; -0 is 0.
    inv.add(it.catalog, it.pistol, 1, std::nanf(""));
    GIANT_CHECK(inv.stacks().back().condition == 0.0f);
    inv.add(it.catalog, it.pistol, 1, -0.0f);
    GIANT_CHECK(!std::signbit(inv.stacks().back().condition));
    // Items that are neither worn nor stackable: one each, like new.
    inv.add(it.catalog, it.note, 2, 0.5f);
    GIANT_CHECK(inv.count(it.note) == 2 && inv.stacks().back().condition == 1.0f);
}

GIANT_TEST("inventory: stack ids are unique and survive other stacks going") {
    Items it;
    Inventory inv;
    const u32 a = inv.add(it.catalog, it.pistol, 1, 0.5f);
    const u32 b = inv.add(it.catalog, it.pistol, 1, 0.9f);
    GIANT_CHECK(a != 0 && b != 0 && a != b);
    GIANT_CHECK(inv.take(a, 1).count == 1);
    GIANT_CHECK(inv.find(a) == nullptr);
    GIANT_CHECK(inv.find(b) != nullptr && inv.find(b)->condition == 0.9f);
    const u32 c = inv.add(it.catalog, it.pistol, 1, 0.1f);
    GIANT_CHECK(c != a && c != b); // ids are never reused
}

GIANT_TEST("inventory: remove takes the worst condition first, then the newest") {
    Items it;
    Inventory inv;
    const u32 good = inv.add(it.catalog, it.pistol, 1, 0.9f);
    const u32 bad = inv.add(it.catalog, it.pistol, 1, 0.2f);
    const u32 bad_newer = inv.add(it.catalog, it.pistol, 1, 0.2f);
    GIANT_CHECK(inv.remove(it.pistol, 1) == 1);
    GIANT_CHECK(inv.find(bad_newer) == nullptr && inv.find(bad) != nullptr);
    GIANT_CHECK(inv.remove(it.pistol, 5) == 2);
    GIANT_CHECK(inv.find(good) == nullptr && inv.empty());

    inv.add(it.catalog, it.ammo, 130);
    GIANT_CHECK(inv.remove(it.ammo, 20) == 20);
    GIANT_CHECK(inv.count(it.ammo) == 110);
    GIANT_CHECK(inv.remove(it.bandage, 1) == 0);
}

GIANT_TEST("inventory: take splits a stack, and a missing stack gives nothing") {
    Items it;
    Inventory inv;
    const u32 s = inv.add(it.catalog, it.ammo, 50);
    const ItemStack part = inv.take(s, 20);
    GIANT_CHECK(part.item == it.ammo && part.count == 20 && part.id == s);
    GIANT_CHECK(inv.find(s)->count == 30);
    GIANT_CHECK(inv.take(s, 99).count == 30);
    GIANT_CHECK(inv.empty());
    GIANT_CHECK(inv.take(s, 1).count == 0);
    const u32 t = inv.add(it.catalog, it.ammo, 5);
    const ItemStack none = inv.take(t, 0);
    GIANT_CHECK(none.count == 0 && none.id == 0);
    GIANT_CHECK(inv.find(t)->count == 5);
}

GIANT_TEST("inventory: repairs and wear set a worn item's condition") {
    Items it;
    Inventory inv;
    const u32 gun = inv.add(it.catalog, it.pistol, 1, 0.3f);
    const u32 ammo = inv.add(it.catalog, it.ammo, 10);
    GIANT_CHECK(inv.set_condition(it.catalog, gun, 0.8f));
    GIANT_CHECK(inv.find(gun)->condition == 0.8f);
    GIANT_CHECK(inv.set_condition(it.catalog, gun, 3.0f));
    GIANT_CHECK(inv.find(gun)->condition == 1.0f);
    GIANT_CHECK(!inv.set_condition(it.catalog, ammo, 0.5f)); // not worn
    GIANT_CHECK(inv.find(ammo)->condition == 1.0f);
    GIANT_CHECK(!inv.set_condition(it.catalog, 999, 0.5f));
}

GIANT_TEST("inventory: moving keeps condition and merges stacks in the target") {
    Items it;
    Inventory body;
    Inventory pack;
    const u32 gun = body.add(it.catalog, it.pistol, 1, 0.35f);
    const u32 rounds = body.add(it.catalog, it.ammo, 40);
    pack.add(it.catalog, it.ammo, 100);

    GIANT_CHECK(Inventory::move(it.catalog, body, gun, 1, pack) == 1);
    GIANT_CHECK(Inventory::move(it.catalog, body, rounds, 40, pack) == 40);
    GIANT_CHECK(body.empty());
    GIANT_CHECK(pack.count(it.ammo) == 140);
    GIANT_CHECK(pack.stacks().size() == 3); // 120 + 20 rounds, one pistol
    bool found = false;
    for (const ItemStack& s : pack.stacks()) {
        found = found || (s.item == it.pistol && s.condition == 0.35f);
    }
    GIANT_CHECK(found);
    GIANT_CHECK(Inventory::move(it.catalog, body, gun, 1, pack) == 0);
    // Moving into itself does nothing.
    const u32 any = pack.stacks()[0].id;
    GIANT_CHECK(Inventory::move(it.catalog, pack, any, 1, pack) == 0);
    GIANT_CHECK(pack.count(it.ammo) == 140);
}

GIANT_TEST("inventory: weight adds up and sets the burden") {
    Items it;
    Inventory inv;
    inv.add(it.catalog, it.pistol, 2, 1.0f);
    inv.add(it.catalog, it.ammo, 100);
    GIANT_CHECK_NEAR(inv.weight_kg(it.catalog), 2.5, 1e-4);
    const CarryLimits limits{30.0f, 50.0f};
    GIANT_CHECK(burden(30.0f, limits) == Burden::light);
    GIANT_CHECK(burden(30.5f, limits) == Burden::heavy);
    GIANT_CHECK(burden(50.0f, limits) == Burden::heavy);
    GIANT_CHECK(burden(50.5f, limits) == Burden::overloaded);
    GIANT_CHECK(burden(std::nanf(""), limits) == Burden::overloaded);
}

GIANT_TEST("inventory: saves and loads back the same, ids included") {
    Items it;
    Inventory inv;
    inv.add(it.catalog, it.pistol, 1, 0.42f);
    inv.add(it.catalog, it.ammo, 130);
    const u32 gone = inv.add(it.catalog, it.bandage, 2);
    inv.take(gone, 2);
    Inventory back;
    const std::optional<std::string> error = load_inventory(save_inventory(inv, it.catalog), it.catalog, back);
    GIANT_CHECK(!error.has_value());
    GIANT_CHECK(back.stacks().size() == inv.stacks().size());
    for (usize i = 0; i < inv.stacks().size(); ++i) {
        GIANT_CHECK(back.stacks()[i] == inv.stacks()[i]);
    }
    GIANT_CHECK(back == inv);
    // New stacks after loading reuse no id, not even the id of a stack
    // that was gone before saving.
    const u32 next = back.add(it.catalog, it.note, 1);
    GIANT_CHECK(next > gone);
    // Conditions are written as people would write them.
    GIANT_CHECK(save_inventory(inv, it.catalog).find("\"condition\": 0.42") != std::string::npos);
    // Counts and conditions may be left out.
    Inventory short_form;
    GIANT_CHECK(!load_inventory(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 7, "item": "pm_pistol"}]})",
                                it.catalog, short_form));
    GIANT_CHECK(short_form.stacks().size() == 1 && short_form.stacks()[0].count == 1 &&
                short_form.stacks()[0].condition == 1.0f);
    GIANT_CHECK(short_form.add(it.catalog, it.note, 1) == 8);
}

GIANT_TEST("inventory: a saved inventory at the id limit can still add stacks") {
    Items it;
    Inventory inv;
    const std::string text = R"({"format": "giant-inventory", "version": 1, "next_id": )" +
                             std::to_string(k_max_stack_id) + R"(, "stacks": [{"id": )" +
                             std::to_string(k_max_stack_id - 1) + R"(, "item": "bandage"}]})";
    GIANT_CHECK(!load_inventory(text, it.catalog, inv).has_value());
    GIANT_CHECK(inv.add(it.catalog, it.pistol, 3, 0.5f) == k_max_stack_id + 2);
}

GIANT_TEST("inventory: a bad save says where and leaves the inventory alone") {
    Items it;
    Inventory inv;
    inv.add(it.catalog, it.pistol, 1, 0.5f);
    const Inventory before = inv;
    const auto fails = [&](const char* text, const char* where) {
        const std::optional<std::string> error = load_inventory(text, it.catalog, inv);
        GIANT_CHECK(error.has_value() && error->find(where) != std::string::npos);
        GIANT_CHECK(inv == before);
    };
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [{"id": 1, "item": "ak"}]})",
          "stacks[0].item");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [
        {"id": 1, "item": "bandage"}, {"id": 1, "item": "bandage"}]})",
          "stacks[1].id");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [{"id": 0, "item": "bandage"}]})",
          "stacks[0].id");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [{"id": 4294967294, "item": "bandage"}]})",
          "stacks[0].id");
    fails(R"({"format": "giant-inventory", "version": 1, "next_id": 4294967295, "stacks": []})", "next_id");
    fails(R"({"format": "giant-inventory", "version": 1, "next_id": 0, "stacks": []})", "next_id");
    fails(R"({"format": "giant-inventory", "version": 1})", "stacks");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [], "extra": 5})", "extra");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [], "x": 1e999})", "not valid JSON");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [{"id": 1, "id": 2, "item": "bandage"}]})",
          "given twice");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [{"id": 1, "item": "bandage", "count": 11}]})",
          "stacks[0].count");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [{"id": 1, "item": "pm_pistol", "count": 2}]})",
          "stacks[0].count");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [{"id": 1, "item": "pm_pistol", "condition": 1.5}]})",
          "stacks[0].condition");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [{"id": 1, "item": "bandage", "condition": 0.5}]})",
          "stacks[0].condition");
    fails(R"({"format": "giant-inventory", "version": 1, "stacks": [{"id": 1, "item": "bandage", "colour": 1}]})",
          "stacks[0].colour");
}
