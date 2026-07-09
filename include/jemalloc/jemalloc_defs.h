/* include/jemalloc/jemalloc_defs.h.  Generated from jemalloc_defs.h.in by configure.  */
/* Defined if __attribute__((...)) syntax is supported. */
#define JEMALLOC_HAVE_ATTR 

/* Defined if alloc_size attribute is supported. */
#define JEMALLOC_HAVE_ATTR_ALLOC_SIZE 

/* Defined if format_arg(...) attribute is supported. */
#define JEMALLOC_HAVE_ATTR_FORMAT_ARG 

/* Defined if format(gnu_printf, ...) attribute is supported. */
#if !defined(__BIONIC__)
#define JEMALLOC_HAVE_ATTR_FORMAT_GNU_PRINTF
#endif

/* Defined if format(printf, ...) attribute is supported. */
#define JEMALLOC_HAVE_ATTR_FORMAT_PRINTF 

/* Defined if fallthrough attribute is supported. */
#define JEMALLOC_HAVE_ATTR_FALLTHROUGH 

/* Defined if cold attribute is supported. */
#define JEMALLOC_HAVE_ATTR_COLD 

/* Defined if deprecated attribute is supported. */
#define JEMALLOC_HAVE_ATTR_DEPRECATED 

/*
 * Define overrides for non-standard allocator-related functions if they are
 * present on the system.
 */
#define JEMALLOC_OVERRIDE_MEMALIGN 
#define JEMALLOC_OVERRIDE_VALLOC 
#if !defined(__BIONIC__)
#define JEMALLOC_OVERRIDE_PVALLOC
#endif 

/*
 * At least Linux omits the "const" in:
 *
 *   size_t malloc_usable_size(const void *ptr);
 *
 * Match the operating system's prototype.
 */
#if defined(__BIONIC__)
#define JEMALLOC_USABLE_SIZE_CONST const
#else
#define JEMALLOC_USABLE_SIZE_CONST
#endif 

/*
 * If defined, specify throw() for the public function prototypes when compiling
 * with C++.  The only justification for this is to match the prototypes that
 * glibc defines.
 */
#if !defined(__BIONIC__)
#define JEMALLOC_USE_CXX_THROW
#endif 

/*
 * If undefined, disables reading configuration from environment variable or file
 */
#if !defined(__BIONIC__)
#define JEMALLOC_CONFIG_ENV
#endif 
#if !defined(__BIONIC__)
#define JEMALLOC_CONFIG_FILE
#endif 

#ifdef _MSC_VER
#  ifdef _WIN64
#    define LG_SIZEOF_PTR_WIN 3
#  else
#    define LG_SIZEOF_PTR_WIN 2
#  endif
#endif

/* sizeof(void *) == 2^LG_SIZEOF_PTR. */
#if defined(__LP64__)
#define LG_SIZEOF_PTR 3
#else
#define LG_SIZEOF_PTR 2
#endif
