/* The printf family inside the game library, for builds whose host does not share the game's
 * calling convention for variadic functions (arm64 macOS: the game is a Linux AArch64 ELF, which
 * passes variadic arguments in registers, while Apple's C library takes them on the stack and uses a
 * different va_list). Every other build takes these from the host's C library and compiles nothing here.
 *
 * The format is walked here, with the game's own va_list, and each conversion is handed to the host
 * one at a time through non-variadic helpers (the loader binds them: port/mac/elf_loader.cpp), so the
 * digits are exactly the host C library's.
 */
#if defined(__aarch64__) && defined(MU_WINDOW_BASE)

typedef __SIZE_TYPE__ size_t;
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_end __builtin_va_end
#define va_arg __builtin_va_arg
#define va_copy __builtin_va_copy
typedef struct FILE FILE;

#define MU_VISIBLE __attribute__((visibility("hidden")))

/* The window this library was built for, checked by the host's loader before anything runs. */
__attribute__((visibility("default"), used)) const unsigned long long mu_window_base_value = MU_WINDOW_BASE;

/* The host's formatters: `spec` is one complete conversion ("%-08.3lld"), the result goes to out
 * (at most n bytes with the terminator); the return is the length the whole text needs. */
#define MU_HOST_IMPORT __attribute__((weak))   /* bound by the host's loader, not the static linker */
MU_HOST_IMPORT int mu_host_fmt_int(char* out, size_t n, const char* spec, long long value);
MU_HOST_IMPORT int mu_host_fmt_double(char* out, size_t n, const char* spec, double value);
MU_HOST_IMPORT int mu_host_fmt_ptr(char* out, size_t n, const char* spec, const void* value);
MU_HOST_IMPORT int mu_host_fmt_str(char* out, size_t n, const char* spec, const char* value);
MU_HOST_IMPORT void mu_host_print(const char* text, size_t length);
size_t fwrite(const void* data, size_t size, size_t count, FILE* file);

/* Appends to a bounded buffer, counting what would have been written. */
typedef struct { char* buf; size_t cap, len; } Sink;
static void put(Sink* s, const char* text, size_t n)
{
    for (size_t i = 0; i < n; ++i, ++s->len) {
        if (s->len + 1 < s->cap) s->buf[s->len] = text[i];
    }
}
/* Appends a decimal number to a conversion spec being built (a '*' width or precision). */
static size_t spec_number(char* spec, size_t k, size_t cap, int value)
{
    char digits[12];
    size_t n = 0;
    unsigned int v = value < 0 ? 0u - (unsigned int) value : (unsigned int) value;
    do { digits[n++] = (char) ('0' + v % 10); v /= 10; } while (v && n < sizeof digits);
    if (value < 0 && k < cap) spec[k++] = '-';
    while (n && k < cap) spec[k++] = digits[--n];
    return k;
}

MU_VISIBLE int vsnprintf(char* buffer, size_t size, const char* format, va_list args)
{
    Sink s = {buffer, buffer ? size : 0, 0};
    va_list ap;
    va_copy(ap, args);
    for (const char* p = format; *p;) {
        if (*p != '%') {
            const char* q = p;
            while (*q && *q != '%') ++q;
            put(&s, p, (size_t) (q - p));
            p = q;
            continue;
        }
        if (p[1] == '%') { put(&s, "%", 1); p += 2; continue; }
        /* One conversion: %[flags][width][.precision][length]conversion, '*' read from the arguments.
         * The spec keeps room for "ll", the conversion and the terminator (4 bytes). */
        char spec[64];
        const size_t cap = sizeof spec - 4;
        size_t k = 0;
        spec[k++] = *p++;
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') { if (k < cap) spec[k++] = *p; ++p; }
        if (*p == '*') { k = spec_number(spec, k, cap, va_arg(ap, int)); ++p; }
        else while (*p >= '0' && *p <= '9') { if (k < cap) spec[k++] = *p; ++p; }
        if (*p == '.') {
            if (k < cap) spec[k++] = '.';
            ++p;
            if (*p == '*') { k = spec_number(spec, k, cap, va_arg(ap, int)); ++p; }
            else while (*p >= '0' && *p <= '9') { if (k < cap) spec[k++] = *p; ++p; }
        }
        /* Length: hh h l ll j z t L. Integers are read at their promoted width and handed on as long long. */
        int longs = 0, halves = 0, wide = 0, long_double = 0;
        for (;;) {
            if (*p == 'l') { ++longs; ++p; }
            else if (*p == 'h') { ++halves; ++p; }
            else if (*p == 'j') { longs = 2; ++p; }
            else if (*p == 'z' || *p == 't') { wide = 1; ++p; }
            else if (*p == 'L') { long_double = 1; ++p; }
            else break;
        }
        const char conv = *p ? *p++ : 0;
        char tmp[512];
        int need = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            const int is_signed = conv == 'd' || conv == 'i';
            long long v;
            if (longs >= 2) v = va_arg(ap, long long);
            else if (longs == 1 || wide) v = is_signed ? (long long) va_arg(ap, long) : (long long) va_arg(ap, unsigned long);
            else if (is_signed) v = va_arg(ap, int);
            else v = (long long) va_arg(ap, unsigned int);
            if (halves == 1) v = is_signed ? (long long) (short) v : (long long) (unsigned short) v;
            else if (halves >= 2) v = is_signed ? (long long) (signed char) v : (long long) (unsigned char) v;
            spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv; spec[k] = 0;
            need = mu_host_fmt_int(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'c': {
            const int v = va_arg(ap, int);
            spec[k++] = 'c'; spec[k] = 0;
            need = mu_host_fmt_int(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            const double v = long_double ? (double) va_arg(ap, long double) : va_arg(ap, double);
            spec[k++] = conv; spec[k] = 0;
            need = mu_host_fmt_double(tmp, sizeof tmp, spec, v);
            break;
        }
        case 's': {
            const char* v = va_arg(ap, const char*);
            if (!v) v = "(null)";
            spec[k++] = 's'; spec[k] = 0;
            need = mu_host_fmt_str(tmp, sizeof tmp, spec, v);
            if (need >= (int) sizeof tmp) {
                /* Longer than the scratch: written straight through, without width or precision. */
                size_t n = 0;
                while (v[n]) ++n;
                put(&s, v, n);
                need = 0;
            }
            break;
        }
        case 'p': {
            const void* v = va_arg(ap, const void*);
            spec[k++] = 'p'; spec[k] = 0;
            need = mu_host_fmt_ptr(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'n': {
            int* v = va_arg(ap, int*);
            if (v) *v = (int) s.len;
            break;
        }
        default:
            /* Not a conversion: shown as written. */
            if (conv) spec[k++] = conv;
            put(&s, spec, k);
            break;
        }
        if (need > (int) sizeof tmp - 1) need = (int) sizeof tmp - 1;
        if (need > 0) put(&s, tmp, (size_t) need);
    }
    va_end(ap);
    if (s.cap) s.buf[s.len < s.cap ? s.len : s.cap - 1] = 0;
    return (int) s.len;
}

MU_VISIBLE int vsprintf(char* buffer, const char* format, va_list args)
{
    return vsnprintf(buffer, (size_t) 1 << 30, format, args);
}
MU_VISIBLE int snprintf(char* buffer, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(buffer, size, format, args);
    va_end(args);
    return n;
}
MU_VISIBLE int sprintf(char* buffer, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = vsprintf(buffer, format, args);
    va_end(args);
    return n;
}
MU_VISIBLE int vprintf(const char* format, va_list args)
{
    char text[2048];
    const int n = vsnprintf(text, sizeof text, format, args);
    mu_host_print(text, n < (int) sizeof text ? (size_t) n : sizeof text - 1);
    return n;
}
MU_VISIBLE int printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = vprintf(format, args);
    va_end(args);
    return n;
}
MU_VISIBLE int fprintf(FILE* file, const char* format, ...)
{
    char text[4096];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    const size_t len = n < (int) sizeof text ? (size_t) n : sizeof text - 1;
    if (file) fwrite(text, 1, len, file);
    else mu_host_print(text, len);
    return n;
}

#endif
