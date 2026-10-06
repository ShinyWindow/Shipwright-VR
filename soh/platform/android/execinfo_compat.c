// SOH [VR] backtrace() / backtrace_symbols() for Android below API 33 (see execinfo_compat.h).
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unwind.h>

typedef struct {
    void** buffer;
    int size;
    int count;
} UnwindState;

static _Unwind_Reason_Code soh_unwind_frame(struct _Unwind_Context* ctx, void* arg) {
    UnwindState* state = (UnwindState*)arg;
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc != 0) {
        if (state->count >= state->size) {
            return _URC_END_OF_STACK;
        }
        state->buffer[state->count++] = (void*)pc;
    }
    return _URC_NO_REASON;
}

int soh_android_backtrace(void** buffer, int size) {
    UnwindState state = { buffer, size, 0 };
    _Unwind_Backtrace(soh_unwind_frame, &state);
    return state.count;
}

// One malloc'd block: the pointer array followed by the strings, freed with a single free() like
// glibc's. "module(symbol+0xoff) [0xaddr]".
char** soh_android_backtrace_symbols(void* const* buffer, int size) {
    enum { kLine = 256 };
    char** out = (char**)malloc((size_t)size * (sizeof(char*) + kLine));
    if (out == NULL) {
        return NULL;
    }
    char* text = (char*)(out + size);
    for (int i = 0; i < size; i++) {
        Dl_info info;
        memset(&info, 0, sizeof(info));
        out[i] = text + (size_t)i * kLine;
        if (dladdr(buffer[i], &info) != 0) {
            // No symbol: the offset is from the module base instead.
            const char* base = (const char*)(info.dli_saddr != NULL ? info.dli_saddr : info.dli_fbase);
            snprintf(out[i], kLine, "%s(%s+0x%zx) [%p]", info.dli_fname ? info.dli_fname : "?",
                     info.dli_sname ? info.dli_sname : "", (size_t)((const char*)buffer[i] - base), buffer[i]);
        } else {
            snprintf(out[i], kLine, "[%p]", buffer[i]);
        }
    }
    return out;
}
