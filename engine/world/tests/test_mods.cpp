// Mods (world/mods.h): the manifest and the list of those on as text, and
// the order mods load in.
#include "giant/world/mods.h"

#include "giant_test.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace giant;
using namespace giant::world;

namespace {

Mod mod(std::string id, ModVersion version = {1, 0, 0}, std::vector<ModNeed> needs = {}, std::vector<std::string> after = {}) {
    Mod m;
    m.name = "The " + id;
    m.id = std::move(id);
    m.version = version;
    m.needs = std::move(needs);
    m.after = std::move(after);
    return m;
}

ModVersion v(u32 major, u32 minor, u32 patch) { return {major, minor, patch}; }

std::string load_error(const std::string& text) {
    Mod out;
    const auto error = load_mod(text, out);
    return error ? error->message : std::string();
}

// The ids in the order loaded, with spaces between.
std::string loaded(const std::vector<Mod>& mods, const ModOrder& order) {
    std::string out;
    for (const usize i : order.order) {
        out += (out.empty() ? "" : " ") + mods[i].id;
    }
    return out;
}

ModOrder all_on(const std::vector<Mod>& mods) { return order_mods(mods, std::vector<u8>(mods.size(), 1)); }

std::string problem_of(const std::vector<Mod>& mods, const ModOrder& order, std::string_view id) {
    for (const ModProblem& p : order.problems) {
        if (mods[p.mod].id == id) {
            return p.message;
        }
    }
    return {};
}

} // namespace

GIANT_TEST("mods: versions are three numbers with dots, and compare as numbers") {
    GIANT_CHECK(mod_version_from_text("1.4.2") == v(1, 4, 2) && mod_version_from_text("0.0.0") == v(0, 0, 0));
    GIANT_CHECK(mod_version_from_text("999999.0.10") == v(999999, 0, 10));
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "1.2.", ".1.2", "1..2", "a.b.c", "1.2.x", "-1.0.0", "+1.0.0", " 1.0.0", "1.0.0 ",
                            "1000000.0.0", "1.0.99999999999999999999", "1,0,0", "01.2.3", "1.02.3", "1.2.00", "1.2.3\n"}) {
        GIANT_CHECK(!mod_version_from_text(bad));
    }
    GIANT_CHECK(mod_version_from_text("10.0.100") == v(10, 0, 100) && !mod_version_from_text(std::string_view()));
    GIANT_CHECK(to_string(v(1, 20, 3)) == "1.20.3");
    GIANT_CHECK(v(1, 10, 0) > v(1, 9, 9) && v(2, 0, 0) > v(1, 99, 99) &&
                v(1, 0, 1) > v(1, 0, 0));
}

GIANT_TEST("mods: a manifest is checked, and saved and loaded as it was") {
    GIANT_CHECK(mod_id_ok("winter_2") && !mod_id_ok("") && !mod_id_ok("2winter") && !mod_id_ok("Winter") && !mod_id_ok("a-b") &&
                !mod_id_ok("a b") && !mod_id_ok("../x") && !mod_id_ok(std::string(65, 'a')) && mod_id_ok(std::string(64, 'a')));
    // Not a name a folder cannot have on Windows.
    for (const char* device : {"con", "prn", "aux", "nul", "com1", "com0", "lpt9"}) {
        GIANT_CHECK(!mod_id_ok(device));
    }
    GIANT_CHECK(mod_id_ok("console") && mod_id_ok("com") && mod_id_ok("com10") && mod_id_ok("coma") && mod_id_ok("lpt"));
    Mod m = mod("winter", {1, 2, 3}, {{"base", {2, 0, 0}}, {"snow", {}}}, {"roads"});
    m.author = "Someone";
    m.description = "Snow on \"everything\".\nAnd ice.";
    GIANT_CHECK(!check_mod(m));
    const std::string text = save_mod(m);
    GIANT_CHECK(text.back() == '\n' && text.find("\"format\": \"giant-mod\"") != std::string::npos);
    Mod back;
    GIANT_CHECK(!load_mod(text, back) && back == m && save_mod(back) == text);
    // The least a manifest may say.
    Mod least;
    GIANT_CHECK(!load_mod(R"({"format":"giant-mod","format_version":1,"id":"a","name":"A","version":"0.1.0"})", least));
    GIANT_CHECK(least.id == "a" && least.name == "A" && least.version == v(0, 1, 0));
    GIANT_CHECK(least.needs.empty() && least.after.empty() && least.author.empty());
    // What is wrong is said, with where.
    Mod wrong = m;
    wrong.needs.push_back({"winter", {}});
    GIANT_CHECK(check_mod(wrong).value_or("").find("cannot need itself") != std::string::npos);
    wrong = m;
    wrong.needs.push_back({"base", {}});
    GIANT_CHECK(check_mod(wrong).value_or("").find("needed twice") != std::string::npos);
    wrong = m;
    wrong.after.push_back("base"); // (needed already)
    GIANT_CHECK(check_mod(wrong).value_or("").find("named twice") != std::string::npos);
    wrong = m;
    wrong.after.push_back("winter");
    GIANT_CHECK(check_mod(wrong).value_or("").find("after itself") != std::string::npos);
    wrong = m;
    wrong.name.clear();
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("name:"));
    wrong = m;
    wrong.description.assign(k_max_mod_text_bytes + 1, 'x');
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("description:"));
    wrong = m;
    wrong.needs.assign(k_max_mod_needs + 1, ModNeed{"x", {}});
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("needs:"));
    wrong = m;
    wrong.name.assign(k_max_name_bytes + 1, 'x');
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("name:"));
    wrong = m;
    wrong.author.assign(k_max_name_bytes + 1, 'x');
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("author:"));
    // Text shown to people and the AI is plain: no control characters (a description may have lines and tabs).
    wrong = m;
    wrong.name = "Two\nlines";
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("name:"));
    wrong = m;
    wrong.author = std::string("a\0b", 3);
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("author:"));
    wrong = m;
    wrong.description = "bell\a";
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("description:"));
    wrong = m;
    wrong.description = "tab\tand\nline, and caf\xc3\xa9";
    GIANT_CHECK(!check_mod(wrong));
    wrong = m;
    wrong.needs[0].id = "Base";
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("needs[0].id:"));
    wrong = m;
    wrong.after[0] = "nul";
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("after[0]:"));
    wrong = m;
    wrong.version.minor = 1000000;
    GIANT_CHECK(check_mod(wrong).value_or("").starts_with("version:"));
}

GIANT_TEST("mods: a manifest that is wrong is refused, and nothing of it kept") {
    const std::string head = R"({"format":"giant-mod","format_version":1,)";
    GIANT_CHECK(load_error("") == "the mod's mod.json is not JSON" && load_error("[]").find("must be an object") != std::string::npos);
    GIANT_CHECK(load_error(R"({"id":"a","name":"A","version":"1.0.0"})").starts_with("format:"));
    GIANT_CHECK(load_error(R"({"format":"giant-scene","format_version":1,"id":"a","name":"A","version":"1.0.0"})").starts_with("format:"));
    GIANT_CHECK(load_error(R"({"format":"giant-mod","format_version":2,"id":"a","name":"A","version":"1.0.0"})").starts_with("format_version:"));
    GIANT_CHECK(load_error(head + R"("name":"A","version":"1.0.0"})") == "id: missing");
    GIANT_CHECK(load_error(head + R"("id":"a","version":"1.0.0"})") == "name: missing");
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A"})") == "version: missing");
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":1})").starts_with("version:"));
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"1.0"})").starts_with("version:"));
    GIANT_CHECK(load_error(head + R"("id":"A","name":"A","version":"1.0.0"})").starts_with("id:"));
    GIANT_CHECK(load_error(head + R"("id":7,"name":"A","version":"1.0.0"})") == "id: must be text");
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"1.0.0","nedes":[]})") == "nedes: unknown key");
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"1.0.0","needs":{}})").starts_with("needs:"));
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"1.0.0","needs":["b"]})").starts_with("needs[0]:"));
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"1.0.0","needs":[{"id":"b","atleast":"1.0.0"}]})") ==
                "needs[0].atleast: unknown key");
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"1.0.0","needs":[{"id":"b","at_least":"new"}]})").starts_with("needs[0].at_least:"));
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"1.0.0","needs":[{"at_least":"1.0.0"}]})").starts_with("needs[0].id:"));
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"1.0.0","needs":[{"id":"a"}]})").find("itself") != std::string::npos);
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"1.0.0","after":"b"})").starts_with("after:"));
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"1.0.0","after":[1]})").starts_with("after[0]:"));
    GIANT_CHECK(load_error(std::string(k_max_mod_file_bytes + 1, ' ')).find("bytes") != std::string::npos);
    // Only a newer format is called newer; a long key is shown by its start.
    GIANT_CHECK(load_error(R"({"format":"giant-mod","format_version":2,"id":"a","name":"A","version":"1.0.0"})").find("newer editor") != std::string::npos);
    GIANT_CHECK(load_error(R"({"format":"giant-mod","format_version":"1","id":"a","name":"A","version":"1.0.0"})") == "format_version: must be 1");
    GIANT_CHECK(load_error(R"({"format":"giant-mod","id":"a","name":"A","version":"1.0.0"})") == "format_version: must be 1");
    const std::string long_key = load_error(head + "\"" + std::string(5000, 'k') + R"(":1,"id":"a","name":"A","version":"1.0.0"})");
    GIANT_CHECK(long_key.size() < 100 && long_key.ends_with("...: unknown key"));
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A","version":"01.0.0"})").starts_with("version:"));
    GIANT_CHECK(load_error(head + R"("id":"a","name":"A\u0007","version":"1.0.0"})").starts_with("name:"));
    // A need without at_least is any version.
    Mod kept = mod("kept");
    GIANT_CHECK(!load_mod(head + R"("id":"a","name":"A","version":"1.0.0","needs":[{"id":"b"}]})", kept) && kept.needs[0] == (ModNeed{"b", {}}));
    GIANT_CHECK(load_mod("{", kept) && kept.id == "a"); // a failed load keeps what was there
}

GIANT_TEST("mods: the list of those on is saved sorted and loaded strictly") {
    const ModsOn kept{{"kept"}, {}};
    const std::string text = save_enabled_mods({{"winter", "base", "winter"}, {}});
    ModsOn back = kept;
    GIANT_CHECK(!load_enabled_mods(text, back) && back.ids == (std::vector<std::string>{"base", "winter"}) && back.slots.empty());
    GIANT_CHECK(text.find("slots") == std::string::npos); // (none given: no key)
    GIANT_CHECK(!load_enabled_mods(save_enabled_mods({}), back) && back.ids.empty() && back.slots.empty());
    back = kept;
    const std::string head = R"({"format":"giant-mods","format_version":1,)";
    GIANT_CHECK(load_enabled_mods(head + R"("enabled":["a","a"]})", back));
    GIANT_CHECK(load_enabled_mods(head + R"("enabled":["A"]})", back));
    GIANT_CHECK(load_enabled_mods(R"({"format":"giant-mods","format_version":1})", back));
    GIANT_CHECK(load_enabled_mods(head + R"("enabled":[],"more":1})", back));
    GIANT_CHECK(load_enabled_mods(R"({"format":"giant-mod","format_version":1,"enabled":[]})", back));
    GIANT_CHECK(back == kept);
    std::string many = head + R"("enabled":[)";
    for (usize i = 0; i <= k_max_mods; ++i) {
        many += (i == 0 ? "\"m" : ",\"m") + std::to_string(i) + "\"";
    }
    GIANT_CHECK(load_enabled_mods(many + "]}", back) && back == kept); // one too many
    // Unsorted on disk (written by hand): loaded sorted.
    GIANT_CHECK(!load_enabled_mods(head + R"("enabled":["b","a"]})", back) && back.ids == std::vector<std::string>({"a", "b"}));
}

GIANT_TEST("mods: each mod's slot is its own for good, and the lowest free is given") {
    ModsOn on;
    GIANT_CHECK(!find_mod_slot(on, "winter") && give_mod_slot(on, "winter") == 0u && give_mod_slot(on, "base") == 1u);
    GIANT_CHECK(give_mod_slot(on, "winter") == 0u && find_mod_slot(on, "base") == 1u && on.slots.size() == 2 && on.slots[0].id == "base");
    // Kept through the file, whether the mod is on or not.
    on.ids = {"base"};
    ModsOn back;
    GIANT_CHECK(!load_enabled_mods(save_enabled_mods(on), back) && back == on);
    // One taken out by hand frees its slot: the next mod without one gets it.
    std::erase_if(back.slots, [](const ModSlot& s) { return s.id == "winter"; });
    GIANT_CHECK(give_mod_slot(back, "summer") == 0u && give_mod_slot(back, "autumn") == 2u);
    // Strict: an id that is none, a slot past the last, a slot given twice, not an object.
    const std::string head = R"({"format":"giant-mods","format_version":1,"enabled":[],"slots":)";
    const ModsOn kept = back;
    GIANT_CHECK(load_enabled_mods(head + R"({"A":0}})", back) && load_enabled_mods(head + R"({"a":512}})", back) &&
                load_enabled_mods(head + R"({"a":-1}})", back) && load_enabled_mods(head + R"({"a":1,"b":1}})", back) &&
                load_enabled_mods(head + R"([0]})", back) && load_enabled_mods(head + R"({"a":1.5}})", back) && back == kept);
    GIANT_CHECK(!load_enabled_mods(head + R"({"b":511,"a":0}})", back) && back.slots == std::vector<ModSlot>({{"a", 0}, {"b", 511}}));
    // Saved by id, whatever the order given; an id or a slot there twice keeps its first, so what is written loads.
    GIANT_CHECK(!load_enabled_mods(save_enabled_mods({{}, {{"b", 1}, {"a", 0}, {"c", 1}, {"a", 5}, {"d", 600}}}), back) &&
                back.slots == std::vector<ModSlot>({{"a", 0}, {"b", 1}}));
    // Every slot given: no more.
    ModsOn full;
    for (u32 i = 0; i < k_max_mod_slots; ++i) {
        GIANT_CHECK(give_mod_slot(full, "m" + std::to_string(i)) == i);
    }
    GIANT_CHECK(!give_mod_slot(full, "one_more") && give_mod_slot(full, "m7") == 7u);
    // The whole list, with the longest ids, fits the file it is read from.
    ModsOn longest;
    for (u32 i = 0; i < k_max_mod_slots; ++i) {
        std::string id = "m" + std::to_string(i);
        id.resize(k_max_mod_id_bytes, 'x');
        if (i < k_max_mods) longest.ids.push_back(id);
        longest.slots.push_back({id, i});
    }
    GIANT_CHECK(save_enabled_mods(longest).size() <= k_max_mod_file_bytes && !load_enabled_mods(save_enabled_mods(longest), back) &&
                back.slots.size() == k_max_mod_slots);
}

GIANT_TEST("mods: load after what is needed and what is asked, otherwise by id, the same every time") {
    // No ties to break but the id.
    std::vector<Mod> mods = {mod("c"), mod("a"), mod("b")};
    GIANT_CHECK(loaded(mods, all_on(mods)) == "a b c" && all_on(mods).problems.empty());
    // Needs come first, however the list is ordered.
    mods = {mod("a", {1, 0, 0}, {{"z", {}}}), mod("z"), mod("m", {1, 0, 0}, {{"a", {}}})};
    GIANT_CHECK(loaded(mods, all_on(mods)) == "z a m");
    std::swap(mods[0], mods[2]);
    GIANT_CHECK(loaded(mods, all_on(mods)) == "z a m");
    // `after` orders when the other is on, and asks nothing when it is not there or not on.
    mods = {mod("a", {1, 0, 0}, {}, {"b", "gone"}), mod("b")};
    GIANT_CHECK(loaded(mods, all_on(mods)) == "b a" && all_on(mods).problems.empty());
    const ModOrder alone = order_mods(mods, std::vector<u8>{1, 0});
    GIANT_CHECK(loaded(mods, alone) == "a" && alone.problems.empty());
    // A diamond: the shared one once, first.
    mods = {mod("top", {1, 0, 0}, {{"left", {}}, {"right", {}}}), mod("right", {1, 0, 0}, {{"base", {}}}),
            mod("left", {1, 0, 0}, {{"base", {}}}), mod("base")};
    GIANT_CHECK(loaded(mods, all_on(mods)) == "base left right top");
    // None on, none loaded, nothing wrong; a short `on` counts as off.
    GIANT_CHECK(order_mods(mods, std::vector<u8>(4, 0)) == ModOrder{} && order_mods(mods, {}) == ModOrder{});
    GIANT_CHECK(order_mods({}, {}) == ModOrder{});
}

GIANT_TEST("mods: a mod that cannot load is left out with the reason, and so are those needing it") {
    // Not there; not on; too old.
    std::vector<Mod> mods = {mod("a", {1, 0, 0}, {{"gone", {}}}), mod("b", {1, 0, 0}, {{"off", {}}}), mod("off"),
                             mod("c", {1, 0, 0}, {{"old", {2, 1, 0}}}), mod("old", {2, 0, 9}), mod("d", {1, 0, 0}, {{"old", {2, 0, 9}}}),
                             mod("e", {1, 0, 0}, {{"a", {}}})};
    const ModOrder order = order_mods(mods, std::vector<u8>{1, 1, 0, 1, 1, 1, 1});
    GIANT_CHECK(loaded(mods, order) == "old d");
    GIANT_CHECK(problem_of(mods, order, "a").find("\"gone\", which is not there") != std::string::npos);
    GIANT_CHECK(problem_of(mods, order, "b").find("\"off\", which is not on") != std::string::npos);
    GIANT_CHECK(problem_of(mods, order, "c").find("at 2.1.0 or newer, and 2.0.9 is there") != std::string::npos);
    GIANT_CHECK(problem_of(mods, order, "e").find("\"a\", which is not loaded") != std::string::npos);
    GIANT_CHECK(order.problems.size() == 4 && problem_of(mods, order, "off").empty());
    for (usize i = 1; i < order.problems.size(); ++i) {
        GIANT_CHECK(order.problems[i - 1].mod < order.problems[i].mod); // by place
    }
    // A circle of needs, one of afters, and a mod needing one in a circle: all left out, the rest loaded.
    mods = {mod("a", {1, 0, 0}, {{"b", {}}}), mod("b", {1, 0, 0}, {{"a", {}}}), mod("c", {1, 0, 0}, {{"a", {}}}), mod("fine"),
            mod("x", {1, 0, 0}, {}, {"y"}), mod("y", {1, 0, 0}, {}, {"x"})};
    const ModOrder circles = all_on(mods);
    GIANT_CHECK(loaded(mods, circles) == "fine" && circles.problems.size() == 5);
    // Each circle is named by its own mods; one that needed a mod in one is told that.
    GIANT_CHECK(problem_of(mods, circles, "a").ends_with(": a, b") && problem_of(mods, circles, "b").ends_with(": a, b"));
    GIANT_CHECK(problem_of(mods, circles, "x").ends_with(": x, y") && problem_of(mods, circles, "y").ends_with(": x, y"));
    GIANT_CHECK(problem_of(mods, circles, "c").find("\"a\", which is not loaded") != std::string::npos);
    // One that only asked to come after a mod in a circle, or after one left out for its need, loads without it;
    // and so does one after that.
    mods = {mod("a", {1, 0, 0}, {{"b", {}}}), mod("b", {1, 0, 0}, {{"a", {}}}), mod("c", {1, 0, 0}, {}, {"a"}),
            mod("d", {1, 0, 0}, {}, {"c", "e"}), mod("e", {1, 0, 0}, {{"gone", {}}})};
    const ModOrder soft = all_on(mods);
    GIANT_CHECK(loaded(mods, soft) == "c d" && soft.problems.size() == 3 && problem_of(mods, soft, "c").empty());
    // A mod needing itself (unchecked) is a circle of one; a circle within a circle's reach is told apart.
    mods = {mod("self"), mod("p", {1, 0, 0}, {{"q", {}}}), mod("q", {1, 0, 0}, {{"p", {}}, {"r", {}}}), mod("r", {1, 0, 0}, {{"s", {}}}),
            mod("s", {1, 0, 0}, {{"r", {}}})};
    mods[0].needs = {{"self", {}}};
    const ModOrder nested = all_on(mods);
    GIANT_CHECK(nested.order.empty() && nested.problems.size() == 5 && problem_of(mods, nested, "self").ends_with(": self"));
    GIANT_CHECK(problem_of(mods, nested, "r").ends_with(": r, s") && problem_of(mods, nested, "p").ends_with(": p, q") &&
                problem_of(mods, nested, "q").ends_with(": p, q"));
    // What a mod is told does not depend on the list's order.
    mods = {mod("i", {1, 0, 0}, {{"x", {}}, {"gone", {}}}), mod("x", {1, 0, 0}, {{"gone2", {}}})};
    const std::string told = problem_of(mods, all_on(mods), "i");
    std::swap(mods[0], mods[1]);
    GIANT_CHECK(told.find("\"gone\", which is not there") != std::string::npos && problem_of(mods, all_on(mods), "i") == told);
    // A circle of every mod there may be: each problem names a few and counts the rest.
    std::vector<Mod> ring;
    for (usize i = 0; i < k_max_mods; ++i) {
        ring.push_back(mod("r" + std::to_string(1000 + i), {1, 0, 0}, {{"r" + std::to_string(1000 + (i + 1) % k_max_mods), {}}}));
    }
    const ModOrder round = all_on(ring);
    GIANT_CHECK(round.order.empty() && round.problems.size() == k_max_mods && round.problems[0].message.size() < 200 &&
                round.problems[0].message.ends_with("and 248 more"));
    // An `on` longer than the list: the rest of it is not looked at. Two of an id, the first off: neither loads.
    mods = {mod("a"), mod("a", {2, 0, 0})};
    GIANT_CHECK(loaded(mods, order_mods(mods, std::vector<u8>{1, 0, 1, 1})) == "a");
    const ModOrder second = order_mods(mods, std::vector<u8>{0, 1});
    GIANT_CHECK(second.order.empty() && second.problems.size() == 1 && second.problems[0].mod == 1);
    // The same id twice: the first stands, the second is left out.
    mods = {mod("a"), mod("a", {2, 0, 0}), mod("b", {1, 0, 0}, {{"a", {2, 0, 0}}})};
    const ModOrder twice = all_on(mods);
    GIANT_CHECK(loaded(mods, twice) == "a" && twice.problems.size() == 2 && twice.problems[0].mod == 1 &&
                twice.problems[0].message.find("comes before it") != std::string::npos);
    // Every mod that is on is either loaded or has a problem, never both.
    // (`order` had 6 of its 7 on; the others all.)
    for (const auto& [o, on_count] : {std::pair<const ModOrder&, usize>{order, 6}, {circles, 6}, {twice, 3}, {soft, 5}, {nested, 5}}) {
        std::vector<usize> seen = o.order;
        for (const ModProblem& p : o.problems) {
            seen.push_back(p.mod);
        }
        std::sort(seen.begin(), seen.end());
        GIANT_CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end() && seen.size() == on_count);
    }
    // As many as there may be, each needing the one after it by id: loaded against the order of the ids.
    std::vector<Mod> many;
    for (usize i = 0; i < k_max_mods; ++i) {
        many.push_back(i + 1 == k_max_mods ? mod("m" + std::to_string(1000 + i))
                                           : mod("m" + std::to_string(1000 + i), {1, 0, 0}, {{"m" + std::to_string(1001 + i), {}}}));
    }
    const ModOrder chain = all_on(many);
    GIANT_CHECK(chain.order.size() == k_max_mods && chain.problems.empty() && chain.order.front() == k_max_mods - 1 && chain.order.back() == 0);
}

GIANT_TEST("mods: a mod's scripts are a .dll in its own folder, by a path that cannot leave it") {
    Mod m = mod("winter", {1, 0, 0}, {}, {});
    GIANT_CHECK(m.scripts.empty() && save_mod(m).find("scripts") == std::string::npos);
    m.scripts = "scripts/Winter.dll";
    Mod back;
    GIANT_CHECK(!check_mod(m) && !load_mod(save_mod(m), back) && back == m);
    m.scripts = "Winter.dll";
    GIANT_CHECK(!check_mod(m));
    for (const char* wrong : {"../Winter.dll", "/abs/Winter.dll", "c:/Winter.dll", "scripts\\Winter.dll", "mod:scripts/Winter.dll", "scripts/Winter.exe",
                              "scripts/Winter", ".dll", "scripts/.dll", "scripts//Winter.dll", "./Winter.dll", "//server/share/Winter.dll",
                              "scripts/nul.dll", "scripts/Winter.DLL", "scripts/ Winter.dll"}) {
        m.scripts = wrong;
        GIANT_CHECK(check_mod(m).value_or("").starts_with("scripts:"));
        GIANT_CHECK(load_mod(save_mod(m), back).has_value());
    }
    m.scripts = std::string(k_max_mod_scripts_bytes - 3, 'a') + ".dll";
    GIANT_CHECK(check_mod(m).value_or("").starts_with("scripts: must be at most"));
    m.scripts = std::string(k_max_mod_scripts_bytes - 4, 'a') + ".dll";
    GIANT_CHECK(!check_mod(m));
    // Written and empty, or not text: refused (none is said by leaving it out).
    const std::string head = R"({"format":"giant-mod","format_version":1,"id":"a","name":"A","version":"0.1.0",)";
    GIANT_CHECK(load_mod(head + R"("scripts":""})", back).has_value() && load_mod(head + R"("scripts":["a.dll"]})", back).has_value() &&
                !load_mod(head + R"("scripts":"a.dll"})", back) && back.scripts == "a.dll");
}

GIANT_TEST("mods: the list of mods whose scripts are allowed is saved sorted and loaded strictly") {
    const std::vector<ModScriptsAllowed> allowed = {{"/worlds/b", "winter"}, {"/worlds/a", "winter"}, {"/worlds/b", "base"}, {"/worlds/a", "winter"}};
    const std::string text = save_mods_scripts_allowed(allowed);
    std::vector<ModScriptsAllowed> back;
    GIANT_CHECK(text.back() == '\n' && text.find("\"format\": \"giant-mods-scripts\"") != std::string::npos && !load_mods_scripts_allowed(text, back));
    GIANT_CHECK(back == std::vector<ModScriptsAllowed>({{"/worlds/a", "winter"}, {"/worlds/b", "base"}, {"/worlds/b", "winter"}}) &&
                save_mods_scripts_allowed(back) == text);
    // What would not load is not written.
    const std::vector<ModScriptsAllowed> some = {{"", "winter"}, {"/w", "Winter"}, {std::string(k_max_allowed_world_bytes + 1, 'w'), "a"}, {"two\nlines", "a"}, {"/w", "a"}};
    GIANT_CHECK(!load_mods_scripts_allowed(save_mods_scripts_allowed(some), back) && back == std::vector<ModScriptsAllowed>({{"/w", "a"}}));
    GIANT_CHECK(!load_mods_scripts_allowed(save_mods_scripts_allowed({}), back) && back.empty());
    // A folder's name that is not UTF-8 would not be read back as it was written: not kept.
    GIANT_CHECK(mod_scripts_world_ok("/w/caf\xc3\xa9/\xe2\x82\xac/\xf0\x9f\x98\x80") && mod_scripts_world_ok(std::string(k_max_allowed_world_bytes, 'w')));
    // (The ends of each range.)
    for (const char* right : {"\xf4\x8f\xbf\xbf", "\xed\x9f\xbf", "\xe0\xa0\x80", "\xf0\x90\x80\x80", "\xc2\x80", "\xdf\xbf", "\xee\x80\x80"}) {
        GIANT_CHECK(mod_scripts_world_ok(right));
    }
    for (const char* wrong : {"\xf5\x80\x80\x80", "\xf0\x9f\x98", "\xf0\x8f\xbf\xbf", "\xc1\xbf", "\xe0\x9f\xbf", "\xf0\x9f\x98\x41"}) {
        GIANT_CHECK(!mod_scripts_world_ok(wrong));
    }
    for (const char* wrong : {"", "/w\xff", "/w\xfe", "/w\xc3", "/w\xc0\xaf", "/w\xe0\x80\xaf", "/w\xed\xa0\x80", "/w\xf4\x90\x80\x80", "/w\x80", "/w\xe2\x82",
                              "/w\ty", "/w\x7f"}) {
        GIANT_CHECK(!mod_scripts_world_ok(wrong));
    }
    const std::vector<ModScriptsAllowed> bytes = {{"/w\xff", "a"}, {"/w\xfe", "a"}, {"/w/caf\xc3\xa9", "a"}};
    GIANT_CHECK(!load_mods_scripts_allowed(save_mods_scripts_allowed(bytes), back) && back == std::vector<ModScriptsAllowed>({{"/w/caf\xc3\xa9", "a"}}));
    // Strict: nothing of a list that is wrong is kept.
    back = {{"/kept", "kept"}};
    const std::string head = R"({"format":"giant-mods-scripts","format_version":1,)";
    for (const char* wrong : {R"("allowed":{}})", R"("allowed":[1]})", R"("allowed":[{"world":"/w"}]})", R"("allowed":[{"world":"/w","mod":"A"}]})",
                              R"("allowed":[{"world":"","mod":"a"}]})", R"("allowed":[{"world":"/w","mod":"a","more":1}]})",
                              R"("allowed":[{"world":"/w","mod":"a"},{"world":"/w","mod":"a"}]})", R"("allowed":[],"other":1})", R"("other":1})"}) {
        GIANT_CHECK(load_mods_scripts_allowed(head + wrong, back).has_value() && back.size() == 1 && back[0].mod == "kept");
    }
    GIANT_CHECK(load_mods_scripts_allowed("", back).has_value() && load_mods_scripts_allowed(R"({"format":"giant-mods","format_version":1,"allowed":[]})", back).has_value());
}
