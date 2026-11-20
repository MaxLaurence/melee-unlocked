// <execinfo.h> for Android before API 33 (bionic has backtrace() from Android 13 on): the same three
// functions over the unwinder the NDK always carries. Found ahead of the NDK's own header
// (port/android/CMakeLists.txt puts this folder first).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <dlfcn.h>
#include <unwind.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct mu_backtrace_state { void** frames; int count, capacity; };
static inline _Unwind_Reason_Code mu_backtrace_step(struct _Unwind_Context* context, void* arg) {
  struct mu_backtrace_state* s = (struct mu_backtrace_state*)arg;
  if (s->count >= s->capacity) return _URC_END_OF_STACK;
  const uintptr_t ip = _Unwind_GetIP(context);
  if (ip) s->frames[s->count++] = (void*)ip;
  return _URC_NO_REASON;
}
static inline int backtrace(void** frames, int capacity) {
  struct mu_backtrace_state s = {frames, 0, capacity};
  _Unwind_Backtrace(mu_backtrace_step, &s);
  return s.count;
}
static inline void backtrace_symbols_fd(void* const* frames, int count, int fd) {
  for (int i = 0; i < count; ++i) {
    Dl_info info;
    char line[512];
    int n;
    if (dladdr(frames[i], &info) && info.dli_fname)
      n = snprintf(line, sizeof line, "%s(%s+0x%lx) [%p]\n", info.dli_fname, info.dli_sname ? info.dli_sname : "",
                   (unsigned long)((uintptr_t)frames[i] - (uintptr_t)(info.dli_sname ? info.dli_saddr : info.dli_fbase)), frames[i]);
    else
      n = snprintf(line, sizeof line, "[%p]\n", frames[i]);
    if (n > 0) (void)!write(fd, line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1));
  }
}
