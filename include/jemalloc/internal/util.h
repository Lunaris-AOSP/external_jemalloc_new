#ifndef JEMALLOC_INTERNAL_UTIL_H
#define JEMALLOC_INTERNAL_UTIL_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_types.h"

#define UTIL_INLINE static inline

/* Junk fill patterns. */
#ifndef JEMALLOC_ALLOC_JUNK
#  define JEMALLOC_ALLOC_JUNK	((uint8_t)0xa5)
#endif
#ifndef JEMALLOC_FREE_JUNK
#  define JEMALLOC_FREE_JUNK	((uint8_t)0x5a)
#endif

/*
 * Wrap a cpp argument that contains commas such that it isn't broken up into
 * multiple arguments.
 */
#define JEMALLOC_ARG_CONCAT(...) __VA_ARGS__

/* cpp macro definition stringification. */
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

/*
 * Silence compiler warnings due to uninitialized values.  This is used
 * wherever the compiler fails to recognize that the variable is never used
 * uninitialized.
 */
#define JEMALLOC_CC_SILENCE_INIT(v) = v

#ifdef __GNUC__
#  define likely(x)   __builtin_expect(!!(x), 1)
#  define unlikely(x) __builtin_expect(!!(x), 0)
#else
#  define likely(x)   !!(x)
#  define unlikely(x) !!(x)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#  include <stddef.h> /* Use the C23 unreachable() macro. */
#else
#  if !defined(JEMALLOC_INTERNAL_UNREACHABLE)
#    error JEMALLOC_INTERNAL_UNREACHABLE should have been defined by configure
#  endif
#  define unreachable() JEMALLOC_INTERNAL_UNREACHABLE()
#endif

/* Set error code. */
UTIL_INLINE void
set_errno(int errnum) {
#ifdef _WIN32
	SetLastError(errnum);
#else
	errno = errnum;
#endif
}

/* Get last error code. */
UTIL_INLINE int
get_errno(void) {
#ifdef _WIN32
	return GetLastError();
#else
	return errno;
#endif
}

#ifdef _MSC_VER
#define util_assume __assume
#elif defined(__clang__) && (__clang_major__ > 3 || \
    (__clang_major__ == 3 && __clang_minor__ >= 6))
#define util_assume __builtin_assume
#else
#define util_assume(expr)		\
	do {				\
		if (!(expr)) {		\
			unreachable();	\
		}			\
	} while(0)
#endif

/* Allows compiler constant folding on inlined paths. */
#if defined(__has_builtin)
#  if __has_builtin(__builtin_constant_p)
#    define util_compile_time_const(x) __builtin_constant_p(x)
#  endif
#endif
#ifndef util_compile_time_const
#  define util_compile_time_const(x) (false)
#endif

/* ptr should be valid. */
JEMALLOC_ALWAYS_INLINE void
util_prefetch_read(void *ptr) {
	/*
	 * This should arguably be a config check; but any version of GCC so old
	 * that it doesn't support __builtin_prefetch is also too old to build
	 * jemalloc.
	 */
#ifdef __GNUC__
	if (config_debug) {
		/* Enforce the "valid ptr" requirement. */
		*(volatile char *)ptr;
	}
	__builtin_prefetch(ptr, /* read or write */ 0, /* locality hint */ 3);
#else
	*(volatile char *)ptr;
#endif
}

JEMALLOC_ALWAYS_INLINE void
util_prefetch_write(void *ptr) {
#ifdef __GNUC__
	if (config_debug) {
		*(volatile char *)ptr;
	}
	/*
	 * The only difference from the read variant is that this has a 1 as the
	 * second argument (the write hint).
	 */
	__builtin_prefetch(ptr, 1, 3);
#else
	*(volatile char *)ptr;
#endif
}

JEMALLOC_ALWAYS_INLINE void
util_prefetch_read_range(void *ptr, size_t sz) {
	for (size_t i = 0; i < sz; i += CACHELINE) {
		util_prefetch_read((void *)((byte_t *)ptr + i));
	}
}

JEMALLOC_ALWAYS_INLINE void
util_prefetch_write_range(void *ptr, size_t sz) {
	for (size_t i = 0; i < sz; i += CACHELINE) {
		util_prefetch_write((void *)((byte_t *)ptr + i));
	}
}

#undef UTIL_INLINE

/*
 * Reads the settings in the following format:
 * key1-key2:value|key3-key4:value|...
 * Note it does not handle the ending '\0'.
 */
bool
multi_setting_parse_next(const char **setting_segment_cur, size_t *len_left,
    size_t *key_start, size_t *key_end, size_t *value);
#endif /* JEMALLOC_INTERNAL_UTIL_H */
