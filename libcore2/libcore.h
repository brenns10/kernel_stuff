/**
 * @file libcore.h a tiny, basic framework for accessing /proc/kcore and
 * vmcores.
 *
 * Uses libkdumpfile for accessing the underlying core.
 * Uses libctf for getting basic structure / type information.
 *
 * Inspired by "libcore" within oled-tools, but with improvements:
 * Removes the need to compile with makedumpfile
 * Removes the need to hand-code struct offsets
 */
#ifndef LIBCORE_H_
#define LIBCORE_H_

#include <stdint.h>
#include <stddef.h>

typedef struct kcore kcore_t;

typedef enum kcore_status {
	KCORE_OK = 0,
	KCORE_ERR_CTF,
	KCORE_ERR_LIBKDUMPFILE,
	KCORE_ERR_OS,
	KCORE_NOT_FOUND,
	KCORE_ALREADY_INITIALIZED,
	KCORE_NOT_IMPLEMENTED,
	KCORE_MEMORY,
	KCORE_FMT,
	_KCORE_MAX,
} kcore_status_t;

/** Create a kcore_t handle. Only failure case is memory allocation */
kcore_t *kcore_alloc(void);

/**
 * Initialize kcore by opening @a path and handling symbol/types
 *
 * After this returns successfully, the handle is initialized and ready to be
 * used for struct lookups, symbol lookups, and memory reads.
 *
 * @param[in] ctx handle to initialize
 * @param[in] path filename to open
 * @param[in] ctf optional path to CTF archive (may be NULL)
 * @returns KCORE_OK on success
 */
kcore_status_t kcore_init(kcore_t *ctx, const char *path, const char *ctf);

/**
 * Destroy the kcore handle and free all held resources
 */
void kcore_free(kcore_t *ctx);

/** Exit with error status given message, printing any relevant kcore error */
void kcore_fail(kcore_t *ctx, const char *fmt, ...);

/** Lookup a symbol by @a name, and set @a addr to its address
 * @param[in] ctx kcore handle
 * @param[in] name Symbol name to search
 * @param[out] addr Pointer to variable
 * @returns KCORE_OK on success
 */
kcore_status_t kcore_sym_lookup(kcore_t *ctx, const char *name, uint64_t *addr);

/** Get the offset of @a field within @a strct
 *
 * @param[in] ctx kcore handle
 * @param[in] strct structure name
 * @param[in] field struct field
 * @param[out] offset the returned structure offset
 */
kcore_status_t kcore_struct_offset(kcore_t *ctx, const char *strct,
				   const char *field, uint32_t *offset);

/* Read integers or pointer. */
kcore_status_t kcore_read_u64(kcore_t *ctx, uint64_t addr, uint64_t *val);
kcore_status_t kcore_read_u32(kcore_t *ctx, uint64_t addr, uint32_t *val);
kcore_status_t kcore_read_u16(kcore_t *ctx, uint64_t addr, uint16_t *val);
kcore_status_t kcore_read_u8(kcore_t *ctx, uint64_t addr, uint8_t *val);

/* Read integers or pointer: no failure */
#define read_n(tp) \
	static inline uint ## tp ## _t kcore_read_u ## tp ## _n (kcore_t *ctx, uint64_t addr) { \
		uint ## tp ## _t val; \
		kcore_status_t s = kcore_read_u ## tp (ctx, addr, &val); \
		if (s != KCORE_OK) \
			kcore_fail(ctx, "kcore_read_u" #tp ": 0x%lx", addr); \
		return val; \
	}
read_n(64)
read_n(32)
read_n(16)
read_n(8)
#undef read_n

/* Lookup symbol and read value, no failure */
#define sym_n(tp) \
	static inline uint ## tp ## _t kcore_sym_u ## tp ## _n (kcore_t *ctx, const char *name) { \
		uint64_t addr; \
		kcore_status_t s = kcore_sym_lookup(ctx, name, &addr); \
		if (s != KCORE_OK) \
			kcore_fail(ctx, "kcore_sym_u" #tp ": lookup sym \"%s\"", name); \
		uint ## tp ## _t val; \
		s = kcore_read_u ## tp (ctx, addr, &val); \
		if (s != KCORE_OK) \
			kcore_fail(ctx, "kcore_sym_u" #tp ": read sym %s at 0x%lx", name, addr); \
		return val; \
	}

sym_n(64);
sym_n(32);
sym_n(16);
sym_n(8);
#undef sym_n

/* Read arbitrary data */
kcore_status_t kcore_read(kcore_t *ctx, uint64_t addr, void *buf, size_t bytes);

kcore_status_t kcore_read_string(kcore_t *ctx, uint64_t addr, char **buf_ret);

#endif // LIBCORE_H_
