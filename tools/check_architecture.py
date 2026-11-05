#!/usr/bin/env python3
"""Fails if an engine module uses a module it is not allowed to use.

The allowed dependencies are the table in docs/architecture.md; this script
reads that table, so the doc and the check can never disagree. It checks:

- the table itself: every dependency names a real module, and there are no
  cycles (modules only talk downward);
- every `#include <giant/<module>/...>` in engine/ and editor/;
- relative includes that climb out of a module (`..`) or reach into another
  module's private `src/` folder;
- C++20 `import` of another module;
- `giant_module(<name> ... DEPENDS ...)` in CMake files.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENGINE = ROOT / "engine"
EDITOR = ROOT / "editor"
ARCH_DOC = ROOT / "docs" / "architecture.md"

GIANT_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]giant/([^/>"]+)/', re.MULTILINE)
ANY_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
IMPORT_RE = re.compile(r"^\s*(?:export\s+)?import\s+giant\.([A-Za-z0-9_]+)", re.MULTILINE)
CMAKE_MODULE_RE = re.compile(r"giant_module\(\s*([A-Za-z0-9_]+)(.*?)\)", re.DOTALL)
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inl", ".c", ".cc", ".cpp", ".cxx", ".ixx", ".cppm"}
EMPTY_MARKER = "(nothing)"
EVERYTHING = "everything"


def fail(message: str) -> None:
    sys.exit(f"Architecture check failed: {message}")


def read_allowed() -> dict[str, set[str]]:
    """Parses and validates the "Allowed module dependencies" table."""
    allowed: dict[str, set[str]] = {}
    in_table = False
    for line in ARCH_DOC.read_text(encoding="utf-8").splitlines():
        if line.startswith("## Allowed module dependencies"):
            in_table = True
            continue
        if in_table and line.startswith("## "):
            break
        if not in_table or not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if cells[0] == "Module" or set("".join(cells)) <= {"-", " ", ":"}:
            continue
        if len(cells) != 2:
            fail(f"bad row in {ARCH_DOC.name}: {line!r}")
        module, deps = cells
        if module in allowed:
            fail(f"module '{module}' is listed twice")
        if deps == EVERYTHING:
            allowed[module] = {"*"}
        elif deps == EMPTY_MARKER:
            allowed[module] = set()
        else:
            allowed[module] = {d.strip() for d in deps.split(",")}
    if not allowed:
        fail(f"could not read the dependency table from {ARCH_DOC}")

    for module, deps in allowed.items():
        for dep in deps - {"*"}:
            if dep not in allowed:
                fail(f"'{module}' depends on unknown module '{dep}'")
            if dep == module:
                fail(f"'{module}' lists itself as a dependency")

    # Modules only talk downward: the graph (ignoring "everything") is acyclic.
    visiting: set[str] = set()
    done: set[str] = set()

    def visit(module: str, path: list[str]) -> None:
        if module in done:
            return
        if module in visiting:
            fail("dependency cycle: " + " -> ".join(path + [module]))
        visiting.add(module)
        for dep in sorted(allowed[module] - {"*"}):
            visit(dep, path + [module])
        visiting.discard(module)
        done.add(module)

    for module in sorted(allowed):
        visit(module, [])
    return allowed


def permits(allowed: dict[str, set[str]], module: str, used: str) -> bool:
    deps = allowed[module]
    return used == module or "*" in deps or used in deps


def check_sources(module: str, folder: Path, allowed: dict[str, set[str]], errors: list[str]) -> None:
    for path in sorted(folder.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(ROOT)

        def where(pos: int) -> str:
            return f"{rel}:{text.count(chr(10), 0, pos) + 1}"

        for match in GIANT_INCLUDE_RE.finditer(text):
            used = match.group(1)
            if used not in allowed:
                errors.append(f"{where(match.start())}: includes unknown module '{used}'")
            elif not permits(allowed, module, used):
                errors.append(f"{where(match.start())}: '{module}' may not include '{used}'")

        for match in ANY_INCLUDE_RE.finditer(text):
            target = match.group(1)
            if target.startswith("giant/"):
                continue
            if ".." in Path(target).parts:
                resolved = (path.parent / target).resolve()
                if not resolved.is_relative_to(folder.resolve()):
                    errors.append(
                        f"{where(match.start())}: relative include '{target}' leaves "
                        f"'{module}'; include <giant/<module>/...> instead"
                    )
            elif target.startswith(("engine/", "editor/")):
                errors.append(f"{where(match.start())}: '{target}' uses a repo path; include <giant/<module>/...> instead")
            elif "/src/" in f"/{target}":
                errors.append(f"{where(match.start())}: '{target}' reaches into a private src/ folder")

        for match in IMPORT_RE.finditer(text):
            used = match.group(1)
            if used not in allowed or not permits(allowed, module, used):
                errors.append(f"{where(match.start())}: '{module}' may not import '{used}'")


LINK_RE = re.compile(r"target_link_libraries\(\s*(\S+)(.*?)\)", re.DOTALL | re.IGNORECASE)


def check_cmake(allowed: dict[str, set[str]], errors: list[str]) -> None:
    for cmake in sorted(list(ENGINE.rglob("CMakeLists.txt")) + list(EDITOR.rglob("CMakeLists.txt"))):
        text = re.sub(r"#[^\n]*", "", cmake.read_text(encoding="utf-8"))
        rel = cmake.relative_to(ROOT)
        for match in LINK_RE.finditer(text):
            target, body = match.group(1), match.group(2)
            linked = set(re.findall(r"giant::([A-Za-z0-9_]+)", body))
            linked |= set(re.findall(r"\bgiant_([A-Za-z0-9_]+)\b", body))
            if not re.fullmatch(r"giant_[A-Za-z0-9_]+", target):
                if linked or ("$" in body and "giant" in body):
                    errors.append(f"{rel}: link giant modules on a target named giant_<module>, not '{target}'")
                continue
            name = target[len("giant_"):]
            if name.endswith("_tests"):
                # A module's tests may use what the module itself may use.
                name = name[: -len("_tests")]
                linked -= {name}
            if name not in allowed:
                errors.append(f"{rel}: '{target}' links giant modules but '{name}' is not in the table")
                continue
            for dep in sorted(linked - {"test_main"}):
                if not permits(allowed, name, dep):
                    errors.append(f"{rel}: giant_{name} may not link {dep}")
            if "$" in body and "giant" in body:
                errors.append(f"{rel}: link giant modules by name, not through variables")
        for match in CMAKE_MODULE_RE.finditer(text):
            name, body = match.group(1), match.group(2)
            if name not in allowed:
                errors.append(f"{rel}: giant_module '{name}' is not in the table")
                continue
            # Walk the arguments the way cmake_parse_arguments does: every
            # DEPENDS group counts, and each dependency must be a plain name.
            mode = None
            for token in body.split():
                if token in ("SOURCES", "DEPENDS"):
                    mode = token
                    continue
                if mode != "DEPENDS":
                    continue
                if not re.fullmatch(r"[A-Za-z0-9_]+", token):
                    errors.append(f"{rel}: giant_module '{name}' must list DEPENDS by plain name, not '{token}'")
                elif not permits(allowed, name, token):
                    errors.append(f"{rel}: '{name}' may not DEPEND on '{token}'")


def main() -> int:
    allowed = read_allowed()
    errors: list[str] = []

    for module_dir in sorted(p for p in ENGINE.iterdir() if p.is_dir()):
        module = module_dir.name
        if module not in allowed:
            errors.append(f"engine/{module}: not listed in docs/architecture.md")
            continue
        check_sources(module, module_dir, allowed, errors)

    if EDITOR.is_dir() and "editor" in allowed:
        check_sources("editor", EDITOR, allowed, errors)

    check_cmake(allowed, errors)

    if errors:
        print("Architecture check failed:")
        for e in errors:
            print("  " + e)
        return 1
    print(f"Architecture check passed ({len(allowed)} modules).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
