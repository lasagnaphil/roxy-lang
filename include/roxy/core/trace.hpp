#pragma once

// Thin wrapper over the Tracy profiler client.
//
// Every macro below is a no-op unless the build was configured with
// -DENABLE_TRACY=ON, which vendors third_party/tracy, links Tracy::TracyClient,
// and defines ROXY_TRACY. This keeps Tracy's headers out of the general codebase
// and makes instrumentation genuinely zero-cost (compiles to nothing) when off.
//
// Usage:
//   void some_phase() {
//       ROXY_ZONE("phase-name");   // RAII: times to end of the enclosing scope
//       ...
//   }
//
// Terminal capture/export workflow: see docs/internals/profiling.md.

#if defined(ROXY_TRACY)
#include <tracy/Tracy.hpp>

// Named zone spanning the enclosing C++ scope. `name` must be a string literal
// (Tracy stores the pointer, not a copy).
#define ROXY_ZONE(name) ZoneScopedN(name)

// Frame boundary — segments the Tracy timeline (one per compile in --repeat).
#define ROXY_FRAME_MARK FrameMark
#else
#define ROXY_ZONE(name) do {} while (0)
#define ROXY_FRAME_MARK do {} while (0)
#endif
