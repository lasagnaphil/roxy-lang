# Libraries used for the Roxy Language

## Vendored (third-party)

These libraries are directly embedded in the roxy library, which means as a user you don't need to do anything.

- **xxHash 0.8.2**
  - For fast XXH3 hash functions (64-bit)
  - Inlining turned on for best performance
- **tsl robin-map 1.2.1**
  - For implementing a fast intern table for strings
- **doctest 2.4.12**
  - Testing framework for unit and E2E tests
  - Header-only, minimal dependencies

## Optional (git submodules)

These are **not** required for a normal build — they sit under `third_party/`,
are pinned to a release tag, and are wired in behind an `ENABLE_<DEP>` CMake
option (default OFF). Fetch with `git submodule update --init <path>`.

- **Tracy 0.13.1** (`third_party/tracy`, `-DENABLE_TRACY=ON`)
  - Cross-platform instrumented profiler; the client compiles into `roxy` and the
    `tracy-capture` / `tracy-csvexport` tools give a fully terminal (headless)
    capture-and-export workflow. Zero overhead when the option is off.
  - See `docs/internals/profiling.md` → "Tracy".

## Custom (in `roxy_core`)

- **rx::String** (`include/roxy/core/string.hpp`)
  - Custom string class with Small String Optimization (SSO, 22 chars inline)
  - Replaces `std::string` throughout the codebase
- **rx::format_to / rx::format** (`include/roxy/core/format.hpp`)
  - Python-style `{}` format strings with format specifiers (`{:04x}`, `{:>10}`, etc.)
  - `rx::format_to(buf, "fmt", args...)` writes to a `char*` buffer
  - `rx::format("fmt", args...)` returns an `rx::String`
  - Replaces the previously vendored `fmt` library
- **rx::StaticString\<N\>** (`include/roxy/core/static_string.hpp`)
  - Fixed-capacity string for stack-allocated formatting (no heap allocation)
  - Used for name mangling and diagnostic messages in the compiler