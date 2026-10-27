#pragma once

// Zones for the compositor's per-frame work, alongside andromedafx's own zones
// for the render pass. No-ops without -Dtracy=enabled.
#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#define ANDROMEDA_ZONE(name) ZoneScopedN(name)
#else
#define ANDROMEDA_ZONE(name)
#endif
