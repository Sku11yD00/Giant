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
                            "1000000.0.0", "1.0.99999999999999999999", "1,0,0"}) {
        GIANT_CHECK(!mod_version_from_text(bad));
    }
    GIANT_CHECK(to_string(v(1, 20, 3)) == "1.20.3");
    GIANT_CHECK(v(1, 10, 0) > v(1, 9, 9) && v(2, 0, 0) > v(1, 99, 99) &&
                v(1, 0, 1) > v(1, 0, 0));
}

GIANT_TEST("mods: a manifest is checked, and saved and loaded as it was") {
    GIANT_CHECK(mod_id_ok("winter_2") && !mod_id_ok("") && !mod_id_ok("2winter") && !mod_id_ok("Winter") && !mod_id_ok("a-b") &&
                !mod_id_ok("a b") && !mod_id_ok("../x") && !mod_id_ok(std::string(65, 'a')) && mod_id_ok(std::string(64, 'a')));
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
    // A need without at_least is any version.
    Mod kept = mod("kept");
    GIANT_CHECK(!load_mod(head + R"("id":"a","name":"A","version":"1.0.0","needs":[{"id":"b"}]})", kept) && kept.needs[0] == (ModNeed{"b", {}}));
    GIANT_CHECK(load_mod("{", kept) && kept.id == "a"); // a failed load keeps what was there
}

GIANT_TEST("mods: the list of those on is saved sorted and loaded strictly") {
    const std::vector<std::string> ids = {"winter", "base", "winter"};
    const std::string text = save_enabled_mods(ids);
    std::vector<std::string> back = {"kept"};
    GIANT_CHECK(!load_enabled_mods(text, back) && back == (std::vector<std::string>{"base", "winter"}));
    GIANT_CHECK(!load_enabled_mods(save_enabled_mods({}), back) && back.empty());
    back = {"kept"};
    GIANT_CHECK(load_enabled_mods(R"({"format":"giant-mods","format_version":1,"enabled":["a","a"]})", back));
    GIANT_CHECK(load_enabled_mods(R"({"format":"giant-mods","format_version":1,"enabled":["A"]})", back));
    GIANT_CHECK(load_enabled_mods(R"({"format":"giant-mods","format_version":1})", back));
    GIANT_CHECK(load_enabled_mods(R"({"format":"giant-mods","format_version":1,"enabled":[],"more":1})", back));
    GIANT_CHECK(load_enabled_mods(R"({"format":"giant-mod","format_version":1,"enabled":[]})", back));
    GIANT_CHECK(back == std::vector<std::string>{"kept"});
    // Unsorted on disk (written by hand): loaded sorted.
    GIANT_CHECK(!load_enabled_mods(R"({"format":"giant-mods","format_version":1,"enabled":["b","a"]})", back) &&
                back == std::vector<std::string>({"a", "b"}));
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
    GIANT_CHECK(problem_of(mods, circles, "a").find("a, b, c, x, y") != std::string::npos);
    // The same id twice: the first stands, the second is left out.
    mods = {mod("a"), mod("a", {2, 0, 0}), mod("b", {1, 0, 0}, {{"a", {2, 0, 0}}})};
    const ModOrder twice = all_on(mods);
    GIANT_CHECK(loaded(mods, twice) == "a" && twice.problems.size() == 2 && twice.problems[0].mod == 1 &&
                twice.problems[0].message.find("comes before it") != std::string::npos);
    // Every mod that is on is either loaded or has a problem, never both.
    for (const ModOrder& o : {order, circles, twice}) {
        std::vector<usize> seen = o.order;
        for (const ModProblem& p : o.problems) {
            seen.push_back(p.mod);
        }
        std::sort(seen.begin(), seen.end());
        GIANT_CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
    }
    // As many as there may be, each needing the one before: in order, quickly.
    std::vector<Mod> many;
    for (usize i = 0; i < k_max_mods; ++i) {
        std::string id = "m" + std::string(i < 10 ? "00" : i < 100 ? "0" : "") + std::to_string(i);
        many.push_back(i == 0 ? mod(id) : mod(id, {1, 0, 0}, {{many.back().id, {}}}));
    }
    std::reverse(many.begin(), many.end());
    const ModOrder chain = all_on(many);
    GIANT_CHECK(chain.order.size() == k_max_mods && chain.problems.empty() && chain.order.front() == k_max_mods - 1 && chain.order.back() == 0);
}
