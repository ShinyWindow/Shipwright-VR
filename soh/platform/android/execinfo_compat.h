// SOH [VR] Android: bionic only declares backtrace() / backtrace_symbols() from API 33, and the
// app targets API 29. ZAPD's crash handler (ZAPDTR, upstream, not patched) calls both; the root
// CMakeLists.txt force-includes this header into ZAPDLib and compiles execinfo_compat.c into it.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int soh_android_backtrace(void** buffer, int size);
char** soh_android_backtrace_symbols(void* const* buffer, int size);

#ifdef __cplusplus
}
#endif

#define backtrace soh_android_backtrace
#define backtrace_symbols soh_android_backtrace_symbols
