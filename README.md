# The Roxy scripting language

This is Roxy, an embeddable scripting language targeted towards game development.

The main objectives are:

- A statically typed language that runs on a bytecode VM (with optional AOT compilation via C code generation)
- A GC-less language which uses [constraint references](https://verdagon.dev/blog/vale-memory-safe-cpp) and [generational references](https://verdagon.dev/blog/generational-references) for memory management
- A language that can be easily embedded in C++ codebases with minimal overhead

Currently this project is work in progress. But there's quite a lot implemented already, look at the examples/ for what you can already do!

See the docs/ folder for more detailed design notes about the language.