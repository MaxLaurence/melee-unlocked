// The processor check before anything AVX2 runs. Rosetta on macOS 15 implements AVX2/FMA even when
// CPUID does not advertise them; the native arm64 build has nothing to check.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#if defined(__x86_64__)
#include <cpuid.h>
#endif
#include <sys/sysctl.h>
namespace mac_host {
inline bool cpu_ok() {
#if defined(__aarch64__)
  return true;
#else
  int translated = 0; size_t bytes = sizeof translated;
  if (sysctlbyname("sysctl.proc_translated", &translated, &bytes, nullptr, 0) == 0 && translated == 1) {
    if (__builtin_available(macOS 15.0, *)) return true;
    return false;
  }
  unsigned a, b, c, d;
  __cpuid_count(0, 0, a, b, c, d); if (a < 7) return false;
  __cpuid_count(1, 0, a, b, c, d);
  constexpr unsigned required = (1u << 12) | (1u << 27) | (1u << 28);
  if ((c & required) != required) return false;
  unsigned low, high;
  __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
  if ((low & 6) != 6) return false;
  __cpuid_count(7, 0, a, b, c, d); return b & (1u << 5);
#endif
}
}
