#include "crash.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "log.h"
#include "mem.h"

namespace re1cc {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetFilterFn g_orig_set_filter = nullptr;

const char* code_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    default: return "exception";
  }
}

void describe(void* addr, char* out, size_t n) {
  HMODULE owner = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(addr), &owner) &&
      owner) {
    GetModuleFileNameA(owner, path, MAX_PATH);
    const char* leaf = std::strrchr(path, '\\');
    std::snprintf(out, n, "%s+0x%X", leaf ? leaf + 1 : path,
                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(addr) -
                                        reinterpret_cast<uintptr_t>(owner)));
  } else {
    std::snprintf(out, n, "%p (no module)", addr);
  }
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    void* at = info->ExceptionRecord->ExceptionAddress;
    char where[MAX_PATH + 32];
    describe(at, where, sizeof(where));
    logf("CRASH: %s (0x%08lX) at %s (thread %lu)", code_name(info->ExceptionRecord->ExceptionCode),
         info->ExceptionRecord->ExceptionCode, where, GetCurrentThreadId());
    // For an access violation the record carries what was touched and how; a
    // null or freed address there is the difference between a bad pointer and
    // a bad instruction, and it is not in the register dump below.
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
      const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
      logf("       %s address %08lX", kind == 1 ? "writing" : kind == 8 ? "executing" : "reading",
           static_cast<unsigned long>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    if (info->ContextRecord) {
      logf("       eip=%08lX esp=%08lX ebp=%08lX eax=%08lX ecx=%08lX edx=%08lX",
           info->ContextRecord->Eip, info->ContextRecord->Esp, info->ContextRecord->Ebp,
           info->ContextRecord->Eax, info->ContextRecord->Ecx, info->ContextRecord->Edx);
      // A few return addresses off the stack, for the ones that land in a module.
      auto* sp = reinterpret_cast<void**>(info->ContextRecord->Esp);
      for (int i = 0, shown = 0; i < 128 && shown < 8; ++i) {
        if (!mem::readable(sp + i, sizeof(void*))) break;
        void* v = sp[i];
        HMODULE owner = nullptr;
        if (v && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCSTR>(v), &owner) &&
            owner) {
          describe(v, where, sizeof(where));
          logf("       stack[%03d] %s", i, where);
          ++shown;
        }
      }
    }
  }
  if (!g_previous) return EXCEPTION_CONTINUE_SEARCH;
  // A packer's own tricks can raise an exception on purpose and handle it in the
  // filter it set; that is no crash, and the log should say so.
  const LONG r = g_previous(info);
  if (r == EXCEPTION_CONTINUE_EXECUTION) logf("       (the filter we chain to handled it and the program went on - not a crash)");
  return r;
}

// SetUnhandledExceptionFilter has one global slot, and the game's CRT takes it
// during start-up, after our DllMain has run - and a CRT's fatal-error path
// sets it to null on purpose before calling UnhandledExceptionFilter, so that
// nothing intercepts the report. Either way ours would never run and a crash
// would leave no line in the log. Enigma resolves the game's imports at run
// time, so there is no import slot to take in DllMain: the game's
// SetUnhandledExceptionFilter is handed this hook when it resolves through the
// GetProcAddress hook (or its slot is re-pointed once the image is unpacked),
// and reassert_crash_filter() covers every path we do not see.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hk_set_filter(LPTOP_LEVEL_EXCEPTION_FILTER next) {
  LPTOP_LEVEL_EXCEPTION_FILTER previous = g_previous;
  g_previous = next;
  if (g_orig_set_filter) g_orig_set_filter(&on_exception);
  logf("crash: the game set its top-level exception filter to %p; ours stays on top (chaining to it)", next);
  return previous;
}

}  // namespace

void describe_address(void* addr, char* out, unsigned n) { describe(addr, out, n); }

void install_crash_logger() {
  g_orig_set_filter = &SetUnhandledExceptionFilter;
  g_previous = SetUnhandledExceptionFilter(&on_exception);
  logf("crash logger installed (the game's own filter is chained once it sets one)");
}

void* crash_filter_hook() { return reinterpret_cast<void*>(&hk_set_filter); }

void reassert_crash_filter() {
  LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(&on_exception);
  if (cur == &on_exception) return;
  if (cur) g_previous = cur;
  logf("crash: the top-level filter had been replaced (%p) - ours is back on top%s", reinterpret_cast<void*>(cur),
       cur ? ", chaining to it" : "");
}

}  // namespace re1cc
