// Register access for the POSIX crash handlers: x86-64 (Linux, Rosetta) or arm64 (native Mac, Android).
#pragma once
#include <cstdint>
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
// The faulting instruction.
inline uintptr_t mac_or_linux_rip(const ucontext_t* uc) {
#if defined(__APPLE__) && defined(__aarch64__)
  return uc->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__)
  return uc->uc_mcontext->__ss.__rip;
#elif defined(__aarch64__)   // Android
  return uc->uc_mcontext.pc;
#else
  return uc->uc_mcontext.gregs[REG_RIP];
#endif
}
#if defined(__APPLE__) || defined(__aarch64__)
inline uintptr_t mac_stack_pointer(const ucontext_t* uc) {
#if !defined(__APPLE__)   // Android
  return uc->uc_mcontext.sp;
#elif defined(__aarch64__)
  return uc->uc_mcontext->__ss.__sp;
#else
  return uc->uc_mcontext->__ss.__rsp;
#endif
}
// arm64: the return address of a leaf function is still in the link register, not on the stack.
inline uintptr_t mac_link_register(const ucontext_t* uc) {
#if !defined(__APPLE__)   // Android
  return uc->uc_mcontext.regs[30];
#elif defined(__aarch64__)
  return uc->uc_mcontext->__ss.__lr;
#else
  (void)uc; return 0;
#endif
}
#endif
