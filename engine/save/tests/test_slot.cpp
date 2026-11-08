// Game save slots: the manifest, commits that only write what changed,
// crash safety, cleaning, the one before, and bad slots refused.
#include "giant/save/slot.h"

#include "giant_test.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace giant;
using namespace giant::save;
namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / ("giant_save_test_" + std::string(name));
    std::error_code e;
    fs::remove_all(dir, e);
    return dir;
}

void write(const fs::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

usize files_in(const fs::path& dir) {
    usize n = 0;
    std::error_code e;
    for (fs::directory_iterator it(dir, e), end; !e && it != end; it.increment(e)) n += it->is_regular_file() ? 1u : 0u;
    return n;
}

Meta meta(double seconds = 1.0) { return {"valley", hash_text("{\"world\":1}"), 1700000000, seconds}; }

} // namespace

GIANT_TEST("save manifest: round trips; strict about keys, names, files and the format") {
    Manifest m;
    m.world_name = "valley";
    m.world_hash = 0xfedcba9876543210ull;
    m.saved_unix = 1700000000;
    m.play_seconds = 12.5;
    m.generation = 3;
    m.sections["player"] = "sections/player.3.json";
    m.cells[{-2, 7}] = "cells/-2_7.2.json";
    m.cells[{0, 0}] = "cells/0_0.3.json";
    const std::string text = save_manifest(m);
    Manifest back;
    GIANT_CHECK(!load_manifest(text, back).has_value());
    GIANT_CHECK(back == m);
    GIANT_CHECK(save_manifest(back) == text);

    Manifest kept = back;
    const auto refused = [&](const std::string& t) { return load_manifest(t, kept).has_value() && kept == back; };
    GIANT_CHECK(refused("not json"));
    GIANT_CHECK(refused("[]"));
    std::string newer = text;
    newer.replace(newer.find("\"format\": 1"), 11, "\"format\": 2");
    GIANT_CHECK(refused(newer));
    std::string typo = text;
    typo.replace(typo.find("\"generation\""), 12, "\"generatoin\"");
    GIANT_CHECK(refused(typo));
    // A file outside the slot's folders, or naming another cell, or another section.
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"cells/-2_7.2.json", "../-2_7.2.json"},
                                  {"cells/-2_7.2.json", "cells/-2_8.2.json"},
                                  {"cells/-2_7.2.json", "cells/-2_7.x.json"},
                                  {"sections/player.3.json", "sections/weather.3.json"},
                                  {"sections/player.3.json", "sections/../../player.3.json"}}) {
        std::string bad = text;
        bad.replace(bad.find(from), from.size(), to);
        GIANT_CHECK(refused(bad));
    }
    GIANT_CHECK(valid_name("slot_1-a") && !valid_name("") && !valid_name("a/b") && !valid_name("..") &&
                !valid_name(std::string(65, 'a')));
    GIANT_CHECK(hash_text("a") != hash_text("b") && hash_text("") == 14695981039346656037ull);
}

GIANT_TEST("save slot: a commit writes only what changed; an open reads it back; old chunks cleaned") {
    const fs::path slot = fresh_dir("commit");
    GIANT_CHECK(!open_slot(slot).error.empty());

    Changes first;
    first.sections["player"] = "{\"x\":1}";
    first.sections["weather"] = "{\"preset\":\"rain\"}";
    first.cells[{0, 0}] = "cell 0 0";
    first.cells[{1, -1}] = "cell 1 -1";
    const Committed a = commit(slot, {}, first, meta(10.0));
    GIANT_CHECK(a.error.empty() && a.written == 4 && a.manifest.generation == 1);
    GIANT_CHECK(a.manifest.world_name == "valley" && a.manifest.play_seconds == 10.0);

    const Opened o = open_slot(slot);
    GIANT_CHECK(o.error.empty() && !o.from_previous && o.manifest == a.manifest);
    GIANT_CHECK(read_chunk(slot, o.manifest.cells.at({1, -1})) == "cell 1 -1");
    GIANT_CHECK(read_chunk(slot, o.manifest.sections.at("weather")) == "{\"preset\":\"rain\"}");
    GIANT_CHECK(!read_chunk(slot, "../save.json").has_value());
    GIANT_CHECK(!read_chunk(slot, "cells/../save.json").has_value());
    GIANT_CHECK(!read_chunk(slot, "cells/9_9.1.json").has_value()); // not there

    // The player moved and a cell emptied: two changes, the rest carried.
    Changes second;
    second.sections["player"] = "{\"x\":2}";
    second.cells[{1, -1}] = std::nullopt;
    const Committed b = commit(slot, a.manifest, second, meta(20.0));
    GIANT_CHECK(b.error.empty() && b.written == 1 && b.manifest.generation == 2);
    GIANT_CHECK(b.manifest.cells.size() == 1 && b.manifest.cells.count({0, 0}) == 1);
    GIANT_CHECK(b.manifest.sections.at("weather") == a.manifest.sections.at("weather"));
    GIANT_CHECK(read_chunk(slot, b.manifest.sections.at("player")) == "{\"x\":2}");
    // The one before is kept whole (its files too): opened when save.json is bad.
    GIANT_CHECK(read_chunk(slot, a.manifest.sections.at("player")) == "{\"x\":1}");
    GIANT_CHECK(read_chunk(slot, a.manifest.cells.at({1, -1})) == "cell 1 -1");

    // A third: the first generation's files no one names now are cleaned away.
    Changes third;
    third.sections["player"] = "{\"x\":3}";
    const Committed c = commit(slot, b.manifest, third, meta(30.0));
    GIANT_CHECK(c.error.empty() && c.removed == 2); // player.1 and the emptied cell
    GIANT_CHECK(!read_chunk(slot, a.manifest.cells.at({1, -1})).has_value());
    GIANT_CHECK(files_in(slot / "sections") == 3 && files_in(slot / "cells") == 1);

    // A stale base (an older generation) is refused, nothing written.
    const Committed stale = commit(slot, b.manifest, third, meta());
    GIANT_CHECK(!stale.error.empty() && stale.written == 0 && open_slot(slot).manifest == c.manifest);
    fs::remove_all(slot);
}

GIANT_TEST("save slot: a crash mid-save leaves the last save; a bad save.json opens the one before") {
    const fs::path slot = fresh_dir("crash");
    Changes first;
    first.sections["player"] = "p1";
    first.cells[{5, 5}] = "c1";
    const Committed a = commit(slot, {}, first, meta());
    Changes second;
    second.sections["player"] = "p2";
    const Committed b = commit(slot, a.manifest, second, meta());
    GIANT_CHECK(b.error.empty());

    // Died after writing chunks and a half manifest: those are only partial and unnamed files.
    write(slot / "cells" / "5_5.3.json", "c3");
    write(slot / "cells" / "5_5.3.json.partial", "c");
    write(slot / "save.json.partial", "{\"form");
    Opened o = open_slot(slot);
    GIANT_CHECK(o.error.empty() && o.manifest == b.manifest && read_chunk(slot, o.manifest.cells.at({5, 5})) == "c1");
    // The next commit cleans them.
    Changes third;
    third.sections["player"] = "p3";
    const Committed c = commit(slot, o.manifest, third, meta());
    GIANT_CHECK(c.error.empty() && c.removed >= 3);
    GIANT_CHECK(!fs::exists(slot / "save.json.partial") && !fs::exists(slot / "cells" / "5_5.3.json.partial"));

    // save.json broken (a disk that lost it): the one before opens, and a commit from it works.
    write(slot / "save.json", "{ broken");
    o = open_slot(slot);
    GIANT_CHECK(o.error.empty() && o.from_previous && o.manifest == b.manifest);
    GIANT_CHECK(read_chunk(slot, o.manifest.sections.at("player")) == "p2");
    Changes fourth;
    fourth.sections["player"] = "p4";
    const Committed d = commit(slot, o.manifest, fourth, meta());
    GIANT_CHECK(d.error.empty() && d.manifest.generation > c.manifest.generation); // no name of c's reused
    o = open_slot(slot);
    GIANT_CHECK(!o.from_previous && read_chunk(slot, o.manifest.sections.at("player")) == "p4");
    GIANT_CHECK(read_chunk(slot, o.manifest.cells.at({5, 5})) == "c1");

    // Both bad: an error, naming save.json.
    write(slot / "save.json", "x");
    write(slot / "save.prev.json", "y");
    o = open_slot(slot);
    GIANT_CHECK(!o.error.empty() && o.error.find("save.json") != std::string::npos);
    fs::remove_all(slot);
}

GIANT_TEST("save slot: an empty base over an old slot is a whole new save; a base from elsewhere is refused") {
    const fs::path root = fresh_dir("slots");
    const fs::path one = root / "one", two = root / "two";
    Changes all;
    all.sections["player"] = "p";
    all.cells[{0, 0}] = "c0";
    all.cells[{0, 1}] = "c1";
    const Committed a = commit(one, {}, all, meta());
    // Over it with an empty base: only what is given, the old chunks left out (cleaned later).
    Changes only;
    only.sections["player"] = "q";
    const Committed b = commit(one, {}, only, meta());
    GIANT_CHECK(b.error.empty() && b.manifest.generation == 2 && b.manifest.cells.empty());
    // Slot one's manifest is not slot two's base.
    const Committed c = commit(two, a.manifest, only, meta());
    GIANT_CHECK(!c.error.empty());
    GIANT_CHECK(commit(two, {}, all, meta()).error.empty());
    // A bad section name, or a chunk too big: nothing.
    Changes bad;
    bad.sections["../x"] = "p";
    GIANT_CHECK(!commit(two, {}, bad, meta()).error.empty());

    const std::vector<Listed> listed = list_slots(root);
    GIANT_CHECK(listed.size() == 2 && listed[0].name == "one" && listed[1].name == "two");
    GIANT_CHECK(listed[0].opened.error.empty() && listed[0].opened.manifest.generation == 2);
    GIANT_CHECK(list_slots(root / "none").empty());
    GIANT_CHECK(delete_slot(root, "one") && !fs::exists(one));
    GIANT_CHECK(!delete_slot(root, "one") && !delete_slot(root, ".."));
    GIANT_CHECK(list_slots(root).size() == 1);
    fs::remove_all(root);
}

GIANT_TEST("save slot: what would not read back is not committed; numbers and names are checked whole") {
    const fs::path slot = fresh_dir("checks");
    // Play seconds past the limit are held at it; a world name not UTF-8 still writes and reads.
    Meta m = meta(2e12);
    m.world_name = "\xFF\xFE";
    Changes c;
    c.sections["player"] = "p";
    const Committed a = commit(slot, {}, c, m);
    GIANT_CHECK(a.error.empty() && a.manifest.play_seconds == k_max_play_seconds);
    const Opened o = open_slot(slot);
    GIANT_CHECK(o.error.empty() && !o.from_previous && o.manifest == a.manifest);
    // Numbers: whole, in range, not bools or fractions.
    Manifest back;
    std::string text = save_manifest(a.manifest);
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"\"format\": 1", "\"format\": 4294967297"},
                                  {"\"format\": 1", "\"format\": true"},
                                  {"\"format\": 1", "\"format\": 1.5"},
                                  {"\"saved_unix\": 1700000000", "\"saved_unix\": 1e300"},
                                  {"\"saved_unix\": 1700000000", "\"saved_unix\": false"},
                                  {"\"play_seconds\": 1000000000000.0", "\"play_seconds\": true"},
                                  {"\"generation\": 1", "\"generation\": -1"}}) {
        std::string bad = text;
        GIANT_CHECK(bad.find(from) != std::string::npos);
        bad.replace(bad.find(from), from.size(), to);
        GIANT_CHECK(load_manifest(bad, back).has_value());
    }
    Manifest cells = a.manifest;
    cells.cells[{3, 0}] = "cells/3_0.1.json";
    text = save_manifest(cells);
    GIANT_CHECK(!load_manifest(text, back).has_value());
    for (const std::string& to : {std::string("\"x\": 4294967299"), std::string("\"x\": true"), std::string("\"x\": 3.0")}) {
        std::string bad = text;
        bad.replace(bad.find("\"x\": 3"), 6, to);
        GIANT_CHECK(load_manifest(bad, back).has_value());
    }
    // Names: lowercase only, no Windows devices.
    for (const char* name : {"Player", "con", "nul", "com1", "lpt9", "AUX"}) {
        GIANT_CHECK(!valid_name(name));
        Changes bad;
        bad.sections[name] = "x";
        GIANT_CHECK(!commit(slot, open_slot(slot).manifest, bad, meta()).error.empty());
    }
    GIANT_CHECK(valid_name("console") && valid_name("com10") && valid_name("com"));
    fs::remove_all(slot);
}

GIANT_TEST("save slot: a newer engine's save is never fallen back from or written over") {
    const fs::path slot = fresh_dir("newer");
    Changes c;
    c.sections["player"] = "p1";
    const Committed a = commit(slot, {}, c, meta());
    c.sections["player"] = "p2";
    const Committed b = commit(slot, a.manifest, c, meta());
    GIANT_CHECK(b.error.empty());
    // A newer engine saved over it (its own format).
    std::string newer = save_manifest(b.manifest);
    newer.replace(newer.find("\"format\": 1"), 11, "\"format\": 2");
    write(slot / "save.json", newer);
    const Opened o = open_slot(slot);
    GIANT_CHECK(!o.error.empty() && o.error.find("newer") != std::string::npos && !o.from_previous);
    const Committed d = commit(slot, a.manifest, c, meta());
    GIANT_CHECK(!d.error.empty() && d.written == 0);
    GIANT_CHECK(!commit(slot, {}, c, meta()).error.empty());
    std::string now;
    {
        // Closed before the rename below: Windows will not rename a file held open.
        std::ifstream f(slot / "save.json");
        now.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    GIANT_CHECK(now == newer && read_chunk(slot, b.manifest.sections.at("player")) == "p2");
    // Only the one before newer (save.json lost): still not written over, its chunks kept.
    fs::rename(slot / "save.json", slot / "save.prev.json");
    GIANT_CHECK(!open_slot(slot).error.empty());
    GIANT_CHECK(!commit(slot, {}, c, meta()).error.empty());
    GIANT_CHECK(read_chunk(slot, b.manifest.sections.at("player")) == "p2");
    fs::remove_all(slot);
}

GIANT_TEST("save slot: a commit from the one before keeps the one before's chunks") {
    const fs::path slot = fresh_dir("from_previous");
    Changes c;
    c.sections["player"] = "p1";
    c.cells[{1, 1}] = "c1";
    const Committed a = commit(slot, {}, c, meta());
    Changes d;
    d.sections["player"] = "p2";
    d.cells[{1, 1}] = "c2";
    const Committed b = commit(slot, a.manifest, d, meta());
    GIANT_CHECK(b.error.empty());
    write(slot / "save.json", "broken");
    const Opened o = open_slot(slot);
    GIANT_CHECK(o.from_previous && o.manifest == a.manifest);
    Changes e;
    e.sections["player"] = "p3";
    const Committed f = commit(slot, o.manifest, e, meta());
    GIANT_CHECK(f.error.empty());
    // Its carried cell (generation 1's) is there, and so is everything the one before (a) names.
    GIANT_CHECK(read_chunk(slot, f.manifest.cells.at({1, 1})) == "c1");
    for (const auto& [k, file] : a.manifest.sections) GIANT_CHECK(read_chunk(slot, file).has_value());
    GIANT_CHECK(open_slot(slot).manifest == f.manifest);
    fs::remove_all(slot);
}
