# Libraries used for the Roxy Language

These libraries are directly embedded in the roxy library, which means as a user you don't need to do anything.

- **xxHash 0.8.2**
  - For fast XXH3 hash functions (64-bit)
  - Inlining turned on for best performance
- **tsl robin-map 1.2.1**
  - For implementing a fast intern table for strings
- **doctest 2.4.12**
  - Testing framework for unit and E2E tests
  - Header-only, minimal dependencies