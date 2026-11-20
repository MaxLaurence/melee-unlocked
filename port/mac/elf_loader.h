// Headless macOS ELF bridge. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mu_host.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mac_elf {
// Where the console's 4 GB address space sits in this process: MEM1 at kWindowBase + 0x80000000, the
// game image at + 0x82800000. Zero under Rosetta; on Apple silicon nothing may be mapped below 4 GB,
// so the window starts at 1 TB (the game library is built with the same -DMU_WINDOW_BASE). Android's
// arm64 kernels give a process 39 bits (512 GB) and ART's heap fills the low 4 GB, so there it is at
// 256 GB (port/android/build-game.sh).
#if defined(__ANDROID__)
constexpr uintptr_t kWindowBase = 0x4000000000ull;
#elif defined(__aarch64__)
constexpr uintptr_t kWindowBase = 0x10000000000ull;
#else
constexpr uintptr_t kWindowBase = 0;
#endif
// Called by an early constructor, before host allocations. Never overwrites mappings.
bool reserve_window(std::string* error);
bool activate_mem1(std::string* error);
void deactivate_mem1();
class Image {
 public:
  bool load(const std::string& path, std::string* error);
  void unload();
  void* symbol(const char* name) const;
  const std::vector<MuStateRegion>& mappings() const { return mappings_; }
  const std::vector<MuStateRegion>& writable() const { return writable_; }
  uint32_t size() const { return size_; }
  uint64_t elf_size() const { return elf_size_; }   // the ELF's bytes (inside the arm64 container too)
 private:
  std::vector<MuStateRegion> mappings_, writable_;
  std::vector<uintptr_t> fini_array_;
  uintptr_t fini_ = 0, symtab_ = 0, strtab_ = 0;
  size_t symbol_count_ = 0, string_bytes_ = 0;
  uint32_t size_ = 0;
  uint64_t elf_size_ = 0;
  bool initialized_ = false;
};
} // namespace mac_elf
