/* Data that comes off the disc keeps the console's layout in memory: big-endian scalars, MSB-first
 * bitfields and 32-bit pointer slots. Nothing is converted at load. A struct that maps such data is
 * marked DISC_STRUCT and the compiler swaps each field as it is read or written, which costs one
 * instruction and leaves the file's own bytes (and the vertex and texture data the GPU decoder
 * reads in place) exactly as they were.
 *
 * Three rules the compiler enforces or that bite silently:
 *   - the address of a scalar field of a DISC_STRUCT cannot be taken; copy it out first
 *   - the order does not reach into a nested struct: a Vec3 inside a DISC_STRUCT stays host order
 *     unless the nested type is itself big-endian, hence the _BE twins below
 *   - an array of plain scalars reached through a pointer is not covered; use the be_* wrappers
 */
#ifndef MU_DISC_H
#define MU_DISC_H

/* Compiler types only: no system header may reach game code (see mu_gxpipe.h). */
typedef __UINT32_TYPE__ mu_u32;
typedef __UINTPTR_TYPE__ mu_uptr;

#define DISC_STRUCT __attribute__((scalar_storage_order("big-endian")))

/* A pointer slot inside disc data. The archive loader relocates it to a real address, and MEM1 and
 * the game image both sit in one 4 GB window (MU_WINDOW_BASE below) so that address fits. It is a union on purpose: using one as if
 * it were a pointer is a compile error rather than a truncation, and the zero-length member carries
 * the pointed-to type so DP() can hand back a typed pointer (it adds no size, and packed/aligned(4)
 * keep it from raising the slot's alignment to 8). */
#define DISC_PTR(T) union __attribute__((packed, aligned(4), scalar_storage_order("big-endian"))) { mu_u32 raw; T* type_[0]; }

/* Where the console's 4 GB address space sits in this process. Zero on x86-64, where MEM1 is at
 * 0x80000000 and the game image follows it. On arm64 macOS nothing may be mapped below 4 GB, so the
 * whole window moves up to MU_WINDOW_BASE (4 GB aligned, set by the build): a slot still holds the
 * address's low 32 bits, the very number the console and the x86 build store, and reading one puts
 * the window back on top. Zero stays a null pointer. */
#ifndef MU_WINDOW_BASE
#define MU_WINDOW_BASE 0
#endif
#if MU_WINDOW_BASE
static inline mu_uptr mu_widen32(mu_u32 value)
{
    return value ? ((mu_uptr) MU_WINDOW_BASE | value) : 0;
}
#else
#define mu_widen32(value) ((mu_uptr) (mu_u32) (value))   /* a constant expression, as it always was */
#endif
#define DP(slot) ((__typeof__((slot).type_[0])) mu_widen32((slot).raw))
#define DISC_GET(T, slot) ((T*) mu_widen32((slot).raw))
#define DISC_SET(slot, ptr) ((slot).raw = mu_addr32(ptr))
#define DISC_NULL(slot) ((slot).raw == 0)
/* A slot the decompilation types as a pointer (UNK_T) but the game uses as a 32-bit number. */
#define DISC_RAW(slot) ((slot).raw)
/* A slot in static data that is initialised with an address. A 32-bit address is not a constant the
 * compiler can emit, so the initialiser holds 0 and a constructor in the same file sets the slot at
 * load time with DISC_SET (see the MU_NATIVE block at the end of such files). */
#define DISC_STATIC(ptr) { 0 }

/* Narrowing a pointer to the 32 bits a disc slot or a hardware register holds. Anything the game
 * can legitimately point at is below 4 GB; a stack address or a host allocation is not, and the
 * debug build stops on it. */
void mu_addr32_failed(const void* ptr, const char* file, int line);
static inline mu_u32 mu_addr32_checked(const void* ptr, const char* file, int line)
{
    mu_uptr value = (mu_uptr) ptr;
#ifndef NDEBUG
    if (value && (value >> 32) != ((mu_uptr) MU_WINDOW_BASE >> 32)) {
        mu_addr32_failed(ptr, file, line);
    }
#endif
    return (mu_u32) value;
}
#define mu_addr32(ptr) mu_addr32_checked((ptr), __FILE__, __LINE__)

/* A 32-bit word that holds an address, widened for a cast to a pointer. The game keeps addresses in
 * s32 and int words all over; they all have bit 31 set here, and a signed word would come back
 * sign-extended. tools/cast_census.py --fix puts this on every narrow int-to-pointer cast. */
#define MU_Z(x) mu_widen32((mu_u32) (x))
/* A console address the game writes as a number (0x80000000, an arena bound) as a pointer here. */
#define MU_WINDOW(addr) ((void*) mu_widen32((mu_u32) (addr)))
/* The same for a nonzero constant, as a constant expression (static initialisers). */
#define MU_WINDOW_CONST(addr) ((void*) ((mu_uptr) MU_WINDOW_BASE | (mu_u32) (addr)))
#define MU_ADDR32(ptr) mu_addr32(ptr)

/* Elements of a scalar array reached through a pointer: f32* weights becomes be_f32* weights. */
typedef struct { float v; } DISC_STRUCT be_f32;
typedef struct { __UINT32_TYPE__ v; } DISC_STRUCT be_u32;
typedef struct { __INT32_TYPE__ v; } DISC_STRUCT be_s32;
typedef struct { __UINT16_TYPE__ v; } DISC_STRUCT be_u16;
typedef struct { __INT16_TYPE__ v; } DISC_STRUCT be_s16;

/* Big-endian twins of the maths types, for use inside disc structs. */
typedef struct { float x, y, z; } DISC_STRUCT Vec3_BE;
typedef struct { float x, y, z, w; } DISC_STRUCT Vec4_BE;
typedef struct { float x, y; } DISC_STRUCT Vec2_BE;
typedef struct { float x, y, z, w; } DISC_STRUCT Quaternion_BE;
typedef struct { float m[3][4]; } DISC_STRUCT Mtx_BE;
typedef struct { float m[4][4]; } DISC_STRUCT Mtx44_BE;

/* The value of a be_* element. */
#define BEV(x) ((x).v)

/* A host-order copy of a big-endian vector (the maths types are visible wherever these are used). */
#define BE_VEC2(v) ({ __typeof__(v) be_ = (v); (Vec2) { be_.x, be_.y }; })
#define BE_VEC3(v) ({ __typeof__(v) be_ = (v); (Vec3) { be_.x, be_.y, be_.z }; })
#define BE_QUAT(v) ({ __typeof__(v) be_ = (v); (Quaternion) { be_.x, be_.y, be_.z, be_.w }; })

#endif
