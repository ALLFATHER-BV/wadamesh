#pragma once
// Lua app host (LUA_APPS.md). Phase 0: WADA_LUA_SPIKE builds run a boot-time
// measurement pass and print [LUA] lines to Serial — flash delta, state cost,
// script heap, hook overhead. The real host API lands here as the phases build.
#if defined(WADA_LUA_SPIKE)
void luaSpikeRun();
#endif
