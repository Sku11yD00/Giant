# C++ coding rules

The engine uses a strict subset of C++20. The goal is code that an AI or a
person can read cold, and that sanitizers can check.

## Ownership and memory

- No owning raw pointers. Own with values, `std::unique_ptr`, or engine
  handles. Raw pointers and references are for borrowing only.
- No `new`/`delete` outside allocators. The one exception is a deliberately
  leaked singleton that must outlive static destruction (the log).
- Hot paths use plain arrays of structs indexed by handles, not object graphs.

## Errors

- Programmer mistakes: `GIANT_ASSERT`. Expected failures (missing file, bad
  data): return a result the caller must check (`[[nodiscard]]`).
- No exceptions across module boundaries. Engine code is built with
  exceptions enabled only because the standard library needs them.

## Threads

- Use the job system (`giant/core/jobs.h`), never raw `std::thread` in
  module code.
- Shared mutable state needs a comment naming what protects it.
- Jobs must not throw, and a job may only wait on work it submitted itself.
- Log sinks must not add or remove sinks; logging from inside a sink goes
  straight to stderr.

## Naming and layout

- Types `PascalCase`, functions and variables `snake_case`, constants
  `k_snake_case`, macros `GIANT_UPPER_CASE`.
- Everything lives in `namespace giant` (and a nested namespace per module
  where useful).
- Public headers in `engine/<module>/include/giant/<module>/`, private code in
  `engine/<module>/src/`, tests in `engine/<module>/tests/`.
- Formatting: `.clang-format` at the repo root.

## Checks that run on every push

- Warnings as errors on MSVC, Clang and GCC.
- Linux debug builds with AddressSanitizer and UndefinedBehaviorSanitizer,
  and with ThreadSanitizer, on GCC and Clang.
- All unit tests.
- `tools/check_architecture.py`.
