#include "giant/ai/commands.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

#include "giant_test.h"

#include <stdexcept>

using namespace giant;
using namespace giant::ai;

namespace {

// A tiny "scene": one number commands can change.
struct Fixture {
    CommandRegistry registry;
    int value = 0;

    Fixture() {
        GIANT_CHECK(registry.add({"set_value", "Sets the value.",
                                  Json::parse(R"({"type": "object", "additionalProperties": false,
                                      "required": ["value"],
                                      "properties": {"value": {"type": "integer"}}})"),
                                  false, [this](const Json& args, CommandContext& context) {
                                      const int before = value;
                                      const int after = args["value"].get<int>();
                                      value = after;
                                      context.record({[this, before] { value = before; },
                                                      [this, after] { value = after; }});
                                      return CommandResult::success({{"value", value}});
                                  }}));
        GIANT_CHECK(registry.add({"get_value", "Reads the value.",
                                  Json{{"type", "object"}}, true,
                                  [this](const Json&, CommandContext&) {
                                      return CommandResult::success({{"value", value}});
                                  }}));
        // Changes the value, then fails: the change must be rolled back.
        GIANT_CHECK(registry.add({"set_then_fail", "Always fails halfway.",
                                  Json{{"type", "object"}}, false,
                                  [this](const Json&, CommandContext& context) {
                                      const int before = value;
                                      value = 999;
                                      context.record({[this, before] { value = before; },
                                                      [this] { value = 999; }});
                                      return CommandResult::failure("broke halfway");
                                  }}));
        GIANT_CHECK(registry.add({"throws", "Throws.", Json{{"type", "object"}}, false,
                                  [](const Json&, CommandContext&) -> CommandResult {
                                      throw std::runtime_error("boom");
                                  }}));
    }
};

int g_asserts = 0;
void count_asserts(const char*, const char*, const char*, int) { ++g_asserts; }

} // namespace

GIANT_TEST("commands: a change can be undone and redone") {
    Fixture f;
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 5}}).ok);
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 7}}).ok);
    GIANT_CHECK(f.value == 7);
    GIANT_CHECK(f.registry.undo());
    GIANT_CHECK(f.value == 5);
    GIANT_CHECK(f.registry.execute("undo").ok);
    GIANT_CHECK(f.value == 0);
    GIANT_CHECK(!f.registry.undo());
    GIANT_CHECK(!f.registry.execute("undo").ok);
    GIANT_CHECK(f.registry.execute("redo").ok);
    GIANT_CHECK(f.value == 5);
    GIANT_CHECK(f.registry.redo());
    GIANT_CHECK(f.value == 7);
    GIANT_CHECK(!f.registry.redo());
}

GIANT_TEST("commands: a new change clears the redo history") {
    Fixture f;
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 1}}).ok);
    GIANT_CHECK(f.registry.undo());
    GIANT_CHECK(f.registry.can_redo());
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 2}}).ok);
    GIANT_CHECK(!f.registry.can_redo());
}

GIANT_TEST("commands: read-only commands and failures stay off the history") {
    Fixture f;
    const CommandResult read = f.registry.execute("get_value");
    GIANT_CHECK(read.ok && read.value["value"] == 0);
    GIANT_CHECK(!f.registry.can_undo());
    GIANT_CHECK(!f.registry.execute("set_value", {{"value", "five"}}).ok);
    GIANT_CHECK(!f.registry.execute("nope").ok);
    GIANT_CHECK(!f.registry.can_undo());
}

GIANT_TEST("commands: a failing command rolls back what it changed") {
    Fixture f;
    const CommandResult result = f.registry.execute("set_then_fail");
    GIANT_CHECK(!result.ok);
    GIANT_CHECK(result.error == "broke halfway");
    GIANT_CHECK(f.value == 0);
    GIANT_CHECK(!f.registry.can_undo());
}

GIANT_TEST("commands: an exception becomes a failure") {
    Fixture f;
    const CommandResult result = f.registry.execute("throws");
    GIANT_CHECK(!result.ok);
    GIANT_CHECK(result.error.find("boom") != std::string::npos);
    // The registry still works afterwards.
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 3}}).ok);
}

GIANT_TEST("commands: arguments are checked against the schema") {
    Fixture f;
    const CommandResult missing = f.registry.execute("set_value");
    GIANT_CHECK(!missing.ok);
    GIANT_CHECK(missing.error.find("value: is required") != std::string::npos);
    GIANT_CHECK(!f.registry.execute("set_value", {{"value", 1}, {"extra", 2}}).ok);
    GIANT_CHECK(f.registry.execute("get_value", Json()).ok); // null means no arguments
}

GIANT_TEST("commands: bad registrations are refused") {
    set_console_logging(false);
    Fixture f;
    const auto run = [](const Json&, CommandContext&) { return CommandResult::success(); };
    GIANT_CHECK(!f.registry.add({"set_value", "duplicate", Json{{"type", "object"}}, false, run}));
    GIANT_CHECK(!f.registry.add({"9lives", "", Json{{"type", "object"}}, false, run}));
    GIANT_CHECK(!f.registry.add({"has.dot", "", Json{{"type", "object"}}, false, run}));
    GIANT_CHECK(!f.registry.add({std::string(65, 'a'), "", Json{{"type", "object"}}, false, run}));
    GIANT_CHECK(!f.registry.add({"no_run", "", Json{{"type", "object"}}, false, {}}));
    GIANT_CHECK(f.registry.add({std::string(64, 'a'), "", Json{{"type", "object"}}, false, run}));
    set_console_logging(true);
}

GIANT_TEST("commands: a read-only command that changes something is caught") {
    set_assert_handler(&count_asserts);
    g_asserts = 0;
    CommandRegistry registry;
    int value = 0;
    GIANT_CHECK(registry.add({"sneaky", "", Json{{"type", "object"}}, true,
                              [&value](const Json&, CommandContext& context) {
                                  value = 1;
                                  context.record({[&value] { value = 0; }, [&value] { value = 1; }});
                                  return CommandResult::success();
                              }}));
    GIANT_CHECK(!registry.execute("sneaky").ok);
    GIANT_CHECK(g_asserts == 1);
    GIANT_CHECK(value == 0);
    GIANT_CHECK(!registry.can_undo());
    set_assert_handler(nullptr);
}

GIANT_TEST("commands: history is capped and listed oldest first") {
    Fixture f;
    for (int i = 1; i <= static_cast<int>(CommandRegistry::k_max_history) + 10; ++i) {
        GIANT_CHECK(f.registry.execute("set_value", {{"value", i}}).ok);
    }
    const auto history = f.registry.undo_history();
    GIANT_CHECK(history.size() == CommandRegistry::k_max_history);
    GIANT_CHECK(history.back().args["value"] == static_cast<int>(CommandRegistry::k_max_history) + 10);
    GIANT_CHECK(f.registry.undo() && f.registry.undo());
    const auto redo = f.registry.redo_history();
    GIANT_CHECK(redo.size() == 2);
    GIANT_CHECK(redo.front().args["value"] == static_cast<int>(CommandRegistry::k_max_history) + 9);
    GIANT_CHECK(f.registry.undo_names().size() == history.size() - 2);
    const CommandResult listed = f.registry.execute("get_history");
    GIANT_CHECK(listed.ok && listed.value["redo"].size() == 2);
}

GIANT_TEST("commands: a non-standard exception does not wedge the registry") {
    CommandRegistry registry;
    GIANT_CHECK(registry.add({"throws_int", "", Json{{"type", "object"}}, false,
                              [](const Json&, CommandContext&) -> CommandResult { throw 42; }}));
    GIANT_CHECK(!registry.execute("throws_int").ok);
    GIANT_CHECK(!registry.execute("throws_int").ok); // still usable, no assert
    GIANT_CHECK(registry.execute("get_history").ok);
}

GIANT_TEST("commands: an undo that throws clears the history instead of escaping") {
    set_console_logging(false);
    CommandRegistry registry;
    int value = 0;
    GIANT_CHECK(registry.add({"bad_undo", "", Json{{"type", "object"}}, false,
                              [&value](const Json&, CommandContext& context) {
                                  value = 1;
                                  context.record({[]() { throw std::runtime_error("no"); },
                                                  [&value] { value = 1; }});
                                  return CommandResult::success();
                              }}));
    GIANT_CHECK(registry.execute("bad_undo").ok);
    GIANT_CHECK(registry.can_undo());
    GIANT_CHECK(!registry.execute("undo").ok);
    GIANT_CHECK(!registry.can_undo() && !registry.can_redo());
    set_console_logging(true);
}

GIANT_TEST("commands: a rollback that throws does not escape") {
    set_console_logging(false);
    CommandRegistry registry;
    GIANT_CHECK(registry.add({"fail_bad_rollback", "", Json{{"type", "object"}}, false,
                              [](const Json&, CommandContext& context) {
                                  context.record({[]() { throw std::runtime_error("no"); }, [] {}});
                                  return CommandResult::failure("failed");
                              }}));
    GIANT_CHECK(!registry.execute("fail_bad_rollback").ok);
    set_console_logging(true);
}

GIANT_TEST("commands: input schemas must describe an object") {
    set_console_logging(false);
    CommandRegistry registry;
    const auto run = [](const Json&, CommandContext&) { return CommandResult::success(); };
    GIANT_CHECK(!registry.add({"no_type", "", Json::object(), false, run}));
    GIANT_CHECK(!registry.add({"string_type", "", Json{{"type", "string"}}, false, run}));
    set_console_logging(true);
}

GIANT_TEST("commands: runs in one merge group become one undo step") {
    Fixture f;
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 1}}).ok);
    for (int v = 2; v <= 5; ++v) {
        GIANT_CHECK(f.registry.execute("set_value", {{"value", v}}, {77, true}).ok);
    }
    GIANT_CHECK(f.registry.undo_history().size() == 2);
    GIANT_CHECK(f.registry.undo_history().back().args["value"] == 5);
    GIANT_CHECK(f.registry.undo());
    GIANT_CHECK(f.value == 1); // back to before the drag
    GIANT_CHECK(f.registry.redo());
    GIANT_CHECK(f.value == 5);
    // A new group starts a new step.
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 6}}, {78, true}).ok);
    GIANT_CHECK(f.registry.undo_history().size() == 3);
}

GIANT_TEST("commands: game logic can change things without touching the undo history") {
    Fixture f;
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 1}}).ok);
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 9}}, {0, false}).ok);
    GIANT_CHECK(f.value == 9);
    GIANT_CHECK(f.registry.undo_history().size() == 1);
}

GIANT_TEST("commands: history names come in the same order as the history") {
    Fixture f;
    // A second undoable command, so the order shows.
    GIANT_CHECK(f.registry.add({"bump_value", "Adds one.", Json{{"type", "object"}}, false,
                                [&f](const Json&, CommandContext& context) {
                                    ++f.value;
                                    context.record({[&f] { --f.value; }, [&f] { ++f.value; }});
                                    return CommandResult::success({});
                                }}));
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 3}}).ok);
    GIANT_CHECK(f.registry.execute("bump_value").ok);
    GIANT_CHECK((f.registry.undo_names() == std::vector<std::string>{"set_value", "bump_value"}));
    GIANT_CHECK(f.registry.undo() && f.registry.undo());
    // Next to redo first, as redo_history.
    GIANT_CHECK((f.registry.redo_names() == std::vector<std::string>{"set_value", "bump_value"}));
    GIANT_CHECK(f.registry.redo_history().front().command == "set_value");
}

GIANT_TEST("commands: a command can forget the history (opening a file)") {
    Fixture f;
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 1}}).ok);
    GIANT_CHECK(f.registry.execute("set_value", {{"value", 2}}).ok);
    GIANT_CHECK(f.registry.undo());
    GIANT_CHECK(f.registry.can_undo() && f.registry.can_redo());
    GIANT_CHECK(f.registry.add({"open_file", "Forgets the history.", Json{{"type", "object"}}, false,
                                [](const Json&, CommandContext& context) {
                                    context.forget_history();
                                    return CommandResult::success();
                                }}));
    GIANT_CHECK(f.registry.execute("open_file").ok);
    GIANT_CHECK(!f.registry.can_undo() && !f.registry.can_redo());
}
