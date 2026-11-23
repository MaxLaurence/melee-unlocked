// Loads the GCC-built Linux game library (melee_game.so) into the Mac host: x86-64 under Rosetta, or
// AArch64 natively on Apple silicon. Built with -fno-stack-protector; Linux TLS accesses are rejected
// and executable code is never patched.
//
// The game's 32-bit pointer slots need MEM1 and the image in one 4 GB window (include/mu_disc.h in
// sourceport/game). x86-64 puts it at zero, as on the console. arm64 macOS maps nothing below 4 GB,
// so there the window starts at kWindowBase and the image is loaded with that bias. Apple silicon also
// runs code only from signed pages: the arm64 game ships inside a signed Mach-O container
// (port/mac/game_container.py) whose code signature is registered with the kernel before the
// executable segment is mapped straight from that file, at its address in the window. Android (also
// AArch64, the same game build with its own window base) maps the code straight from the plain ELF
// the package installs beside the program (libmelee_game.so in the app's native library folder, the
// one place an app may map code from).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "elf_loader.h"
#include "elf_types.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <stdexcept>
#ifdef __APPLE__
#include <mach-o/loader.h>
#endif
#include <sys/mman.h>
#include <unistd.h>

namespace mac_elf {
namespace {
#if defined(__aarch64__)
constexpr uint16_t kMachine = 183;   // EM_AARCH64
constexpr const char* kMachineName = "AArch64";
#else
constexpr uint16_t kMachine = 62;    // EM_X86_64
constexpr const char* kMachineName = "x86-64";
#endif
constexpr uintptr_t bias = kWindowBase;   // the ELF's addresses are console addresses (0x82800000...)
constexpr uintptr_t window_start = bias + 0x7f000000, window_end = bias + 0x84000000;
constexpr uintptr_t mem1 = bias + 0x80000000, image_base = 0x82800000;   // image_base: an ELF address
const size_t page = size_t(getpagesize());   // 16 KB on Apple silicon, 4 KB under Rosetta and on Android
bool reserved = false;
int reservation_error = 0;
uintptr_t floor_page(uintptr_t p) { return p & ~(page - 1); }
uintptr_t ceil_page(uintptr_t p) { return (p + page - 1) & ~(page - 1); }
void require(bool ok, const std::string& why) { if (!ok) throw std::runtime_error("game ELF: " + why); }
void reserve_early() {
  if (reserved) return;
  // A hint without MAP_FIXED cannot replace any existing region. Keep the entire window reserved for
  // the process's lifetime, including gaps between ELF segments.
  void* p = mmap(reinterpret_cast<void*>(window_start), window_end - window_start, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) { reservation_error = errno; return; }
  if (uintptr_t(p) != window_start) { munmap(p, window_end - window_start); reservation_error = EEXIST; return; }
  reserved = true;
}
__attribute__((constructor(101))) void earliest_reservation() { reserve_early(); }
void protect(uintptr_t at, size_t n, int prot) {
  require(mprotect(reinterpret_cast<void*>(at), n, prot) == 0, "mprotect at " + std::to_string(at) + ": " + std::strerror(errno));
}
// MAP_FIXED is used only inside our successfully acquired PROT_NONE reservation.
void require_owned(uintptr_t at, size_t n) {
  require(reserved && at >= window_start && at < window_end && n <= window_end - at, "mapping outside owned window");
}
void map_owned(uintptr_t at, size_t n) {
  require_owned(at, n);
  void* p = mmap(reinterpret_cast<void*>(at), n, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
  require(p != MAP_FAILED && uintptr_t(p) == at, "fixed mapping failed: " + std::string(std::strerror(errno)));
}

// The printf family's conversions, one at a time, for an AArch64 game (shim/mu_stdio_window.c): the
// game walks the format with its own va_list and hands each conversion here without varargs.
int fmt_int(char* out, size_t n, const char* spec, long long value) {
  return std::strstr(spec, "ll") ? std::snprintf(out, n, spec, value) : std::snprintf(out, n, spec, int(value));
}
int fmt_double(char* out, size_t n, const char* spec, double value) { return std::snprintf(out, n, spec, value); }
int fmt_ptr(char* out, size_t n, const char* spec, const void* value) { return std::snprintf(out, n, spec, value); }
int fmt_str(char* out, size_t n, const char* spec, const char* value) { return std::snprintf(out, n, spec, value); }
void print(const char* text, size_t length) { std::fwrite(text, 1, length, stdout); }

struct Import { const char* name; uintptr_t address; };
#define IMPORT(f) {#f, reinterpret_cast<uintptr_t>(&::f)}
const Import* find_import(const char* name) {
  // Only full-width integers, pointers and doubles cross here, which the Linux and Apple calling
  // conventions pass alike on both architectures. FILE* is opaque to the game: every fopen/fwrite goes
  // to Darwin, so no Linux FILE layout is ever inspected. long and size_t are 64-bit on all four.
  static const Import table[] = {
    IMPORT(strtoul),
    {"strchr", reinterpret_cast<uintptr_t>(static_cast<char* (*)(char*, int)>(&::strchr))},
    IMPORT(strlen), IMPORT(memcmp), IMPORT(strncmp), IMPORT(strncpy),
    IMPORT(memset), IMPORT(memcpy), IMPORT(strcpy), IMPORT(fclose),
    {"strrchr", reinterpret_cast<uintptr_t>(static_cast<char* (*)(char*, int)>(&::strrchr))},
    {"memchr", reinterpret_cast<uintptr_t>(static_cast<void* (*)(void*, int, size_t)>(&::memchr))},
    IMPORT(fflush), IMPORT(fopen), IMPORT(getenv), IMPORT(fabsf), IMPORT(strcmp), IMPORT(atoi),
    {"fabs", reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(&::fabs))},
    IMPORT(memmove), IMPORT(sqrtf), IMPORT(fwrite), IMPORT(strcat),
    // Host memory for host-side work only (shim/mu_css_state.c builds a log line in it): above 4 GB
    // here, so never a pointer the game keeps in a 32-bit word.
    IMPORT(malloc), IMPORT(free),
#if defined(__aarch64__)
    {"mu_host_fmt_int", reinterpret_cast<uintptr_t>(&fmt_int)},
    {"mu_host_fmt_double", reinterpret_cast<uintptr_t>(&fmt_double)},
    {"mu_host_fmt_ptr", reinterpret_cast<uintptr_t>(&fmt_ptr)},
    {"mu_host_fmt_str", reinterpret_cast<uintptr_t>(&fmt_str)},
    {"mu_host_print", reinterpret_cast<uintptr_t>(&print)},
#else
    // x86-64 Darwin and glibc share the SysV variadic convention and va_list layout.
    IMPORT(printf), IMPORT(sprintf), IMPORT(vsprintf), IMPORT(vsnprintf), IMPORT(fprintf), IMPORT(snprintf),
#endif
  };
  // fabs[f] is exact sign-bit clearing; sqrtf is IEEE single precision on both.
  // Simulation's other math stays bound to its own ELF definitions (DT_SYMBOLIC).
  for (const auto& item : table) if (!std::strcmp(name, item.name)) return &item;
  return nullptr;
}
#undef IMPORT
uintptr_t import(const char* name) {
  require(std::strcmp(name, "__stack_chk_fail") != 0, "Linux stack protector import; rebuild melee_game.so with -fno-stack-protector");
  if (const Import* found = find_import(name)) return found->address;
#if defined(__aarch64__)
  for (const char* variadic : {"printf", "sprintf", "snprintf", "vsnprintf", "vsprintf", "fprintf", "vprintf", "vfprintf"})
    require(std::strcmp(name, variadic) != 0, std::string("variadic import ") + name +
            ": Linux and Apple AArch64 pass variadic arguments differently (the game must define it, shim/mu_stdio_window.c)");
#endif
  throw std::runtime_error(std::string("Mac ELF: unlisted strong import: ") + name);
}

// The signed Mach-O container of an AArch64 game: where the ELF sits in it (its __TEXT,__elf section)
// and its code signature, registered with the kernel so the executable pages can be mapped from it.
struct Container { uint64_t elf_offset = 0, elf_size = 0; };
#ifdef __APPLE__
Container open_container(int fd, const std::vector<uint8_t>& file) {
  mach_header_64 h;
  require(file.size() > sizeof h, "short container");
  std::memcpy(&h, file.data(), sizeof h);
  require(h.magic == MH_MAGIC_64 && h.sizeofcmds <= file.size() - sizeof h, "not a Mach-O container");
  Container c;
  uint32_t sig_off = 0, sig_size = 0;
  const uint8_t* p = file.data() + sizeof h;
  for (uint32_t i = 0; i < h.ncmds; ++i) {
    load_command lc; std::memcpy(&lc, p, sizeof lc);
    require(lc.cmdsize >= sizeof lc && p + lc.cmdsize <= file.data() + sizeof h + h.sizeofcmds, "bad load command");
    if (lc.cmd == LC_CODE_SIGNATURE) {
      linkedit_data_command sig; std::memcpy(&sig, p, sizeof sig);
      sig_off = sig.dataoff; sig_size = sig.datasize;
    } else if (lc.cmd == LC_SEGMENT_64) {
      segment_command_64 seg; std::memcpy(&seg, p, sizeof seg);
      for (uint32_t s = 0; s < seg.nsects; ++s) {
        section_64 sec; std::memcpy(&sec, p + sizeof seg + s * sizeof sec, sizeof sec);
        if (!std::strncmp(sec.sectname, "__elf", sizeof sec.sectname)) { c.elf_offset = sec.offset; c.elf_size = sec.size; }
      }
    }
    p += lc.cmdsize;
  }
  require(c.elf_size && c.elf_offset % 0x4000 == 0 && c.elf_offset + c.elf_size <= file.size(), "container has no aligned __elf section");
  require(sig_size && uint64_t(sig_off) + sig_size <= file.size(), "container is not signed");
  fsignatures_t fs{};
  fs.fs_file_start = 0;
  fs.fs_blob_start = reinterpret_cast<void*>(uintptr_t(sig_off));
  fs.fs_blob_size = sig_size;
  require(fcntl(fd, F_ADDFILESIGS_RETURN, &fs) == 0, std::string("the kernel refused the container's signature: ") + std::strerror(errno));
  require(uint64_t(fs.fs_file_start) >= c.elf_offset + c.elf_size, "the signature does not cover the game's code");
  return c;
}
#endif
} // namespace

bool reserve_window(std::string* error) {
  if (reserved) return true;
  char text[160];
  std::snprintf(text, sizeof text, "cannot reserve 0x%lx..0x%lx without replacing mappings: ", (unsigned long)window_start,
                (unsigned long)window_end);
  if (error) *error = text + std::string(std::strerror(reservation_error));
  return false;
}
bool activate_mem1(std::string* error) {
  if (!reserve_window(error)) return false;
  if (mprotect(reinterpret_cast<void*>(mem1), bias + image_base - mem1, PROT_READ | PROT_WRITE)) {
    if (error) *error = "cannot activate MEM1: " + std::string(std::strerror(errno));
    return false;
  }
  std::fprintf(stderr, "Mac layout: owned window 0x%lx..0x%lx; MEM1 0x%lx..0x%lx\n", (unsigned long)window_start,
               (unsigned long)window_end, (unsigned long)mem1, (unsigned long)(bias + image_base));
  return true;
}
void deactivate_mem1() {
  if (reserved) {
    madvise(reinterpret_cast<void*>(mem1), bias + image_base - mem1, MADV_DONTNEED);
    mprotect(reinterpret_cast<void*>(mem1), bias + image_base - mem1, PROT_NONE);
  }
}

bool Image::load(const std::string& path, std::string* error) {
  int fd = -1;
  try {
    require(mappings_.empty(), "image already loaded");
    require(reserve_window(error), error ? *error : "window reservation failed");
    fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "cannot open " + path);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(bool(input), "cannot open " + path);
    const auto length = input.tellg();
    require(length >= std::streamoff(sizeof(Ehdr)) && length < 96 * 1024 * 1024, "invalid game file size");
    std::vector<uint8_t> whole(static_cast<size_t>(length));
    input.seekg(0); input.read(reinterpret_cast<char*>(whole.data()), whole.size());
    require(bool(input), "short game file read");
    // The ELF itself, or (AArch64) the signed container around it.
    uint64_t elf_offset = 0, elf_size = whole.size();
#ifdef __APPLE__
    const bool container = whole.size() >= 4 && *reinterpret_cast<const uint32_t*>(whole.data()) == MH_MAGIC_64;
#if defined(__aarch64__)
    require(container, path + " is not the signed game container Apple silicon needs (port/mac/game_container.py)");
#endif
    if (container) { const Container c = open_container(fd, whole); elf_offset = c.elf_offset; elf_size = c.elf_size; }
#endif
    const uint8_t* file = whole.data() + elf_offset;
    const size_t file_size = size_t(elf_size);
    Ehdr eh; std::memcpy(&eh, file, sizeof eh);
    require(!std::memcmp(eh.ident, "\177ELF\2\1\1", 7) && eh.type == 3 && eh.machine == kMachine && eh.version == 1,
            std::string("expected a little-endian ") + kMachineName + " ET_DYN");
    // The ELF's own length, which the build's .snapexcl names (# dll_size): the container's section is
    // padded to 16 KB, and the linker puts the section headers last.
    const uint64_t headers_end = eh.shoff + uint64_t(eh.shnum) * eh.shentsize;
    elf_size_ = eh.shnum && headers_end <= elf_size ? headers_end : elf_size;
    require(eh.ehsize == sizeof(Ehdr) && eh.phentsize == sizeof(Phdr) && eh.phnum > 0 && eh.phnum < 100 &&
            eh.phoff <= file_size && uint64_t(eh.phnum) * sizeof(Phdr) <= file_size - eh.phoff, "invalid program headers");
    std::vector<Phdr> headers(eh.phnum);
    std::memcpy(headers.data(), file + eh.phoff, headers.size() * sizeof(Phdr));
    std::vector<std::pair<uintptr_t, uintptr_t>> pages;
    const Phdr* dynamic = nullptr;
    for (const auto& p : headers) {
      require(p.type != PT_TLS, "TLS segments are unsupported");
      if (p.type == PT_DYNAMIC) { require(!dynamic, "multiple dynamic segments"); dynamic = &p; }
      if (p.type != PT_LOAD) continue;
      require(p.memsz && p.filesz <= p.memsz && p.offset <= file_size && p.filesz <= file_size - p.offset &&
              p.vaddr >= image_base && bias + p.vaddr < window_end && p.memsz <= window_end - bias - p.vaddr &&
              !(p.flags & ~7u) && !((p.flags & PF_X) && (p.flags & PF_W)) &&
              p.align >= page && !(p.align & (p.align - 1)) && p.vaddr % p.align == p.offset % p.align,
              "invalid/out-of-window PT_LOAD");
      require(!(p.flags & PF_X) || p.memsz == p.filesz, "an executable segment with zero fill");
      const auto lo = floor_page(p.vaddr), hi = ceil_page(p.vaddr + p.memsz);
      for (const auto& old : pages) require(hi <= old.first || lo >= old.second, "overlapping load pages");
      pages.push_back({lo, hi});
      size_ = std::max(size_, uint32_t(p.vaddr + p.memsz - image_base));
    }
    // Linux thread-local storage (the stack protector's canary lives there) has no counterpart here.
    size_t tls_sites = 0;
    for (const auto& p : headers) if (p.type == PT_LOAD && (p.flags & PF_X)) {
      const auto* code = file + p.offset;
#if defined(__aarch64__)
      for (size_t i = 0; i + 4 <= p.filesz; i += 4) {
        uint32_t insn; std::memcpy(&insn, code + i, 4);
        if ((insn & 0xFFFFFFE0u) == 0xD53BD040u) ++tls_sites;   // mrs xN, tpidr_el0
      }
#else
      for (size_t i = 0; i + 8 <= p.filesz; ++i) {
        if (code[i] != 0x64) continue;
        size_t op = i + 1;
        if (code[op] >= 0x40 && code[op] <= 0x4f) ++op;
        if (op + 7 > p.filesz) continue;
        const auto opcode = code[op];
        if (opcode != 0x8b && opcode != 0x89 && opcode != 0x2b && opcode != 0x29 &&
            opcode != 0x33 && opcode != 0x31 && opcode != 0x3b && opcode != 0x39) continue;
        uint32_t displacement; std::memcpy(&displacement, code + op + 3, 4);
        if ((code[op + 1] & 0xc7) != 4 || code[op + 2] != 0x25 || displacement != 0x28) continue;
        ++tls_sites; i = op + 6;
      }
#endif
    }
    require(tls_sites == 0, "found " + std::to_string(tls_sites) +
            " Linux thread-pointer accesses (stack canaries); rebuild melee_game.so with -fno-stack-protector (no code patching)");
    require(dynamic && !pages.empty() && std::min_element(pages.begin(), pages.end())->first == image_base, "missing image base or PT_DYNAMIC");
    for (const auto& p : headers) if (p.type == PT_LOAD) {
      const uintptr_t lo = bias + floor_page(p.vaddr), hi = bias + ceil_page(p.vaddr + p.memsz);
#if defined(__aarch64__)
      if (p.flags & PF_X) {
        // Straight from the file: on the Mac its pages are the ones the kernel checks against the
        // signature; Android lets an app map code only from its installed libraries.
        require_owned(lo, hi - lo);
        void* at = mmap(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_EXEC, MAP_FIXED | MAP_PRIVATE, fd,
                        off_t(elf_offset + floor_page(p.offset)));
        require(at != MAP_FAILED && uintptr_t(at) == lo, "cannot map the game's code: " + std::string(std::strerror(errno)));
        mappings_.push_back({reinterpret_cast<void*>(bias + p.vaddr), uint32_t(p.memsz)});
        continue;
      }
#endif
      map_owned(lo, hi - lo);
      mappings_.push_back({reinterpret_cast<void*>(bias + p.vaddr), uint32_t(p.memsz)});
      if (p.flags & PF_W) writable_.push_back(mappings_.back());
      std::memcpy(reinterpret_cast<void*>(bias + p.vaddr), file + p.offset, p.filesz);
    }
    close(fd); fd = -1;
    // Addresses below are the ELF's own (console) addresses; `at` turns one into this process's.
    auto at = [](uint64_t v) { return bias + uintptr_t(v); };
    auto valid = [&](uint64_t v, uint64_t n, uint32_t flags = 0) {
      for (const auto& p : headers)
        if (p.type == PT_LOAD && v >= p.vaddr && v - p.vaddr <= p.memsz && n <= p.memsz - (v - p.vaddr) && (p.flags & flags) == flags) return true;
      return false;
    };
    require(valid(dynamic->vaddr, dynamic->memsz) && dynamic->memsz % sizeof(Dyn) == 0, "invalid dynamic table");
    std::map<int64_t, uint64_t> dt;
    std::vector<uint64_t> needed;
    bool terminated = false;
    for (size_t i = 0; i < dynamic->memsz / sizeof(Dyn); ++i) {
      const auto d = reinterpret_cast<const Dyn*>(at(dynamic->vaddr))[i];
      if (d.tag == DT_NULL) { terminated = true; break; }
      switch (d.tag) {
        case DT_NEEDED: case DT_PLTRELSZ: case DT_PLTGOT: case DT_HASH: case DT_STRTAB:
        case DT_SYMTAB: case DT_RELA: case DT_RELASZ: case DT_RELAENT: case DT_STRSZ:
        case DT_SYMENT: case DT_INIT: case DT_FINI: case DT_SONAME: case DT_SYMBOLIC:
        case DT_PLTREL: case DT_JMPREL: case DT_INIT_ARRAY: case DT_FINI_ARRAY:
        case DT_INIT_ARRAYSZ: case DT_FINI_ARRAYSZ: case DT_RUNPATH: case DT_FLAGS:
        case DT_GNU_HASH: case DT_VERSYM: case DT_RELACOUNT: case DT_VERNEED: case DT_VERNEEDNUM:
        case DT_BIND_NOW: case DT_FLAGS_1: break;
        default: require(false, "unexpected dynamic tag " + std::to_string(d.tag));
      }
      if (d.tag == DT_NEEDED) needed.push_back(d.value);
      if (d.tag != DT_NEEDED) require(!dt.count(d.tag), "duplicate dynamic tag");
      dt[d.tag] = d.value;
    }
    require(terminated && dt[DT_SYMENT] == sizeof(Sym) && (!dt[DT_RELASZ] || dt[DT_RELAENT] == sizeof(Rela)) &&
            (!dt[DT_PLTRELSZ] || dt[DT_PLTREL] == DT_RELA) && (dt[DT_FLAGS] & ~(2ull | 8ull)) == 0,
            "unsupported dynamic format/flags");   // DF_SYMBOLIC, DF_BIND_NOW
    symtab_ = at(dt[DT_SYMTAB]); strtab_ = at(dt[DT_STRTAB]); string_bytes_ = dt[DT_STRSZ];
    require(string_bytes_ && valid(dt[DT_STRTAB], string_bytes_), "invalid string table");
    for (auto offset : needed) {
      require(offset < string_bytes_, "invalid dependency name");
      const char* dep = reinterpret_cast<const char*>(strtab_ + offset);
      require(std::memchr(dep, 0, string_bytes_ - offset), "unterminated dependency name");
      require(!std::strcmp(dep, "libc.so.6") || !std::strcmp(dep, "libm.so.6"), std::string("unexpected dependency: ") + dep);
    }
    if (dt.count(DT_HASH)) {
      require(valid(dt[DT_HASH], 8), "invalid DT_HASH");
      symbol_count_ = reinterpret_cast<const uint32_t*>(at(dt[DT_HASH]))[1];
    } else {
      const uint64_t hash_at = dt[DT_GNU_HASH];
      require(valid(hash_at, 16), "missing/invalid GNU hash");
      const auto* hash = reinterpret_cast<const uint32_t*>(at(hash_at));
      const uint32_t buckets = hash[0], first = hash[1], bloom = hash[2];
      require(buckets && bloom && buckets < 100000 && bloom < 100000, "invalid GNU hash sizes");
      const uint64_t bucket_at = hash_at + 16 + uint64_t(bloom) * 8;
      require(valid(bucket_at, uint64_t(buckets) * 4), "invalid GNU hash buckets");
      const auto* bucket = reinterpret_cast<const uint32_t*>(at(bucket_at));
      const uint64_t chains = bucket_at + uint64_t(buckets) * 4;
      symbol_count_ = first;
      for (uint32_t b = 0; b < buckets; ++b) if (bucket[b]) {
        uint32_t s = bucket[b]; require(s >= first, "invalid GNU hash index");
        for (;;) {
          require(s < 100000 && valid(chains + uint64_t(s - first) * 4, 4), "unterminated GNU hash chain");
          const auto value = *reinterpret_cast<const uint32_t*>(at(chains + uint64_t(s - first) * 4));
          symbol_count_ = std::max(symbol_count_, size_t(s) + 1);
          if (value & 1) break;
          ++s;
        }
      }
    }
    require(symbol_count_ && symbol_count_ < 100000 && valid(dt[DT_SYMTAB], symbol_count_ * sizeof(Sym)), "invalid symbol table");
    const auto* symbols = reinterpret_cast<const Sym*>(symtab_);
    auto name = [&](const Sym& s) {
      require(s.name < string_bytes_, "invalid symbol name");
      const char* n = reinterpret_cast<const char*>(strtab_ + s.name);
      require(std::memchr(n, 0, string_bytes_ - s.name), "unterminated symbol name"); return n;
    };
    std::vector<uintptr_t> resolved(symbol_count_);
    size_t strong = 0, weak = 0, weak_bound = 0;
    for (size_t i = 1; i < symbol_count_; ++i) {
      const auto& s = symbols[i]; const char* n = name(s);
      require((s.info & 15) != 6 && (s.info & 15) != 10, "TLS/IFUNC symbols unsupported");
      // Zero-size linker markers such as __bss_end__ may sit exactly at a segment's end.
      if (s.shndx) { require(s.shndx != 0xfff1 && valid(s.value, s.size), std::string("invalid defined symbol ") + n); resolved[i] = at(s.value); }
      else if ((s.info >> 4) == 2) {
        // An undefined weak symbol: one of the host's own helpers when the table has it, else absent.
        const Import* found = find_import(n);
        resolved[i] = found ? found->address : 0;
        ++weak; if (found) ++weak_bound;
      }
      else { resolved[i] = import(n); ++strong; }
    }
#if defined(__aarch64__)
    constexpr uint32_t R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027;
#else
    constexpr uint32_t R_ABS64 = 1, R_GLOB_DAT = 6, R_JUMP_SLOT = 7, R_RELATIVE = 8;
#endif
    size_t relocations = 0;
    auto relocate = [&](uint64_t table, uint64_t bytes) {
      if (!bytes) return;
      require(bytes % sizeof(Rela) == 0 && valid(table, bytes), "invalid RELA range");
      for (size_t i = 0; i < bytes / sizeof(Rela); ++i) {
        const auto r = reinterpret_cast<const Rela*>(at(table))[i];
        const uint32_t type = uint32_t(r.info); const uint64_t sym = r.info >> 32;
        require(valid(r.offset, 8, PF_W) && sym < symbol_count_, "invalid relocation target/symbol");
        uint64_t value;
        if (type == R_RELATIVE) { require(sym == 0, "RELATIVE with symbol"); value = at(uint64_t(r.addend)); }
        else if (type == R_ABS64 || type == R_GLOB_DAT || type == R_JUMP_SLOT) value = resolved[sym] + r.addend;
        else { require(false, "unsupported relocation type " + std::to_string(type)); value = 0; }
        std::memcpy(reinterpret_cast<void*>(at(r.offset)), &value, 8); ++relocations;
      }
    };
    relocate(dt[DT_RELA], dt[DT_RELASZ]); relocate(dt[DT_JMPREL], dt[DT_PLTRELSZ]);
    std::fprintf(stderr, "Mac ELF: %s, %zu symbols (%zu strong imports, %zu weak of which %zu host helpers), %zu relocations; "
                 "thread-pointer sites %zu (no patching)\n", kMachineName, symbol_count_, strong, weak, weak_bound, relocations, tls_sites);
    for (const auto& p : headers) if (p.type == PT_LOAD) {
#if defined(__aarch64__)
      if (p.flags & PF_X) continue;   // mapped read+execute from the file
#endif
      const int prot = ((p.flags & PF_R) ? PROT_READ : 0) | ((p.flags & PF_W) ? PROT_WRITE : 0) | ((p.flags & PF_X) ? PROT_EXEC : 0);
      protect(bias + floor_page(p.vaddr), ceil_page(p.vaddr + p.memsz) - floor_page(p.vaddr), prot);
    }
    for (const auto& p : headers) if (p.type == PT_LOAD)
      std::fprintf(stderr, "Mac layout: PT_LOAD 0x%llx..0x%llx %c%c%c\n", (unsigned long long)at(p.vaddr),
                   (unsigned long long)at(p.vaddr + p.memsz), (p.flags & PF_R) ? 'r' : '-', (p.flags & PF_W) ? 'w' : '-', (p.flags & PF_X) ? 'x' : '-');
    std::fprintf(stderr, "Mac layout: image 0x%lx..0x%lx\n", (unsigned long)at(image_base), (unsigned long)at(image_base + size_));
#if defined(__aarch64__)
    // The game was built for this window (sourceport/game -DMU_WINDOW_BASE); a mismatch would turn
    // every 32-bit pointer slot into a wild pointer.
    {
      const auto* base = static_cast<const uint64_t*>(symbol("mu_window_base_value"));
      require(base && *base == kWindowBase, "the game library was built for another address window");
    }
#endif
    auto array = [&](int64_t address_tag, int64_t size_tag) {
      const auto table = dt[address_tag], bytes = dt[size_tag];
      require(bytes % 8 == 0 && (!bytes || valid(table, bytes)), "invalid init/fini array");
      std::vector<uintptr_t> result;
      for (size_t i = 0; i < bytes / 8; ++i) {
        const auto fn = reinterpret_cast<const uintptr_t*>(at(table))[i];   // relocated: a process address
        require(fn >= bias && valid(fn - bias, 1, PF_X), "init/fini entry outside executable segment"); result.push_back(fn);
      }
      return result;
    };
    auto init_array = array(DT_INIT_ARRAY, DT_INIT_ARRAYSZ);
    fini_array_ = array(DT_FINI_ARRAY, DT_FINI_ARRAYSZ); fini_ = dt[DT_FINI] ? at(dt[DT_FINI]) : 0;
    require(!dt[DT_FINI] || valid(dt[DT_FINI], 1, PF_X), "invalid DT_FINI");
    require(!dt[DT_INIT] || valid(dt[DT_INIT], 1, PF_X), "invalid DT_INIT");
    if (dt[DT_INIT]) reinterpret_cast<void (*)()>(at(dt[DT_INIT]))();
    for (auto fn : init_array) reinterpret_cast<void (*)()>(fn)();
    initialized_ = true;
    return true;
  } catch (const std::exception& e) {
    if (fd >= 0) close(fd);
    if (error) *error = e.what();
    unload();
    return false;
  }
}
void* Image::symbol(const char* name) const {
  const auto* symbols = reinterpret_cast<const Sym*>(symtab_);
  for (size_t i = 1; i < symbol_count_; ++i) {
    const auto& s = symbols[i];
    if (s.shndx && s.name < string_bytes_ && !std::strcmp(name, reinterpret_cast<const char*>(strtab_ + s.name)))
      return reinterpret_cast<void*>(bias + s.value);
  }
  return nullptr;
}
void Image::unload() {
  if (initialized_) {
    for (auto it = fini_array_.rbegin(); it != fini_array_.rend(); ++it) reinterpret_cast<void (*)()>(*it)();
    if (fini_) reinterpret_cast<void (*)()>(fini_)();
  }
  for (const auto& r : mappings_) {
    const auto lo = floor_page(uintptr_t(r.address)), end = ceil_page(uintptr_t(r.address) + r.size);
    // Retain the reservation while releasing the backing pages.
    mmap(reinterpret_cast<void*>(lo), end - lo, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
  }
  mappings_.clear(); writable_.clear(); fini_array_.clear();
  fini_ = symtab_ = strtab_ = 0; symbol_count_ = string_bytes_ = 0; size_ = 0; initialized_ = false;
}
} // namespace mac_elf
