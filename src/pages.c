#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/pages.h"

#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/malloc_io.h"

#ifdef JEMALLOC_SYSCTL_VM_OVERCOMMIT
#include <sys/sysctl.h>
#ifdef __FreeBSD__
#include <vm/vm_param.h>
#endif
#endif
#ifdef __NetBSD__
#include <sys/bitops.h>	/* ilog2 */
#endif
#ifdef JEMALLOC_HAVE_VM_MAKE_TAG
#define PAGES_FD_TAG VM_MAKE_TAG(254U)
#else
#define PAGES_FD_TAG -1
#endif
#if defined(JEMALLOC_HAVE_PRCTL) && defined(JEMALLOC_PAGEID)
#include <sys/prctl.h>
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif
#endif

/******************************************************************************/
/* Defines/includes needed for special android code. */

#if defined(__ANDROID__)
#include <sys/prctl.h>
#endif

/******************************************************************************/
/* Data. */

/* Actual operating system page size, detected during bootstrap, <= PAGE. */
size_t	os_page;

#ifndef _WIN32
#  define PAGES_PROT_COMMIT (PROT_READ | PROT_WRITE)
#  define PAGES_PROT_DECOMMIT (PROT_NONE)
static int	mmap_flags;
#endif
#define os_overcommits true

/* Runtime support for lazy purge. Irrelevant when !pages_can_purge_lazy. */
static bool pages_can_purge_lazy_runtime = true;

#ifdef JEMALLOC_PAGEID
static int os_page_id(void *addr, size_t size, const char *name)
{
#ifdef JEMALLOC_HAVE_PRCTL
	/*
	 * While parsing `/proc/<pid>/maps` file, the block could appear as
	 * 7f4836000000-7f4836800000 rw-p 00000000 00:00 0 [anon:jemalloc_pg_overcommit]`
	 */
	return prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (uintptr_t)addr, size,
	    (uintptr_t)name);
#else
	return 0;
#endif
}
#endif

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void os_pages_unmap(void *addr, size_t size);

/******************************************************************************/

static void *
os_pages_map(void *addr, size_t size, size_t alignment, bool *commit) {
	assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
	assert(ALIGNMENT_CEILING(size, os_page) == size);
	assert(size != 0);

	if (os_overcommits) {
		*commit = true;
	}

	void *ret;
#ifdef _WIN32
	/*
	 * If VirtualAlloc can't allocate at the given address when one is
	 * given, it fails and returns NULL.
	 */
	ret = VirtualAlloc(addr, size, MEM_RESERVE | (*commit ? MEM_COMMIT : 0),
	    PAGE_READWRITE);
#else
	/*
	 * We don't use MAP_FIXED here, because it can cause the *replacement*
	 * of existing mappings, and we only want to create new mappings.
	 */
	{
		int flags = mmap_flags;
#ifdef __NetBSD__
		/*
		 * On NetBSD PAGE for a platform is defined to the
		 * maximum page size of all machine architectures
		 * for that platform, so that we can use the same
		 * binaries across all machine architectures.
		 */
		if (alignment > os_page || PAGE > os_page) {
			unsigned int a = ilog2(MAX(alignment, PAGE));
			flags |= MAP_ALIGNED(a);
		}
#endif
		int prot = *commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;

		ret = mmap(addr, size, prot, flags, PAGES_FD_TAG, 0);
	}
	assert(ret != NULL);

	if (ret == MAP_FAILED) {
		ret = NULL;
	} else if (addr != NULL && ret != addr) {
		/*
		 * We succeeded in mapping memory, but not in the right place.
		 */
		os_pages_unmap(ret, size);
		ret = NULL;
	}
#endif
#if defined(__ANDROID__)
	if (ret != NULL) {
		/* Name this memory as being used by libc */
		prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, ret, size,
		    "libc_malloc");
	}
#endif
	assert(ret == NULL || (addr == NULL && ret != addr) || (addr != NULL &&
	    ret == addr));
#ifdef JEMALLOC_PAGEID
	int n = os_page_id(ret, size,
	    os_overcommits ? "jemalloc_pg_overcommit" : "jemalloc_pg");
	assert(n == 0 || (n == -1 && get_errno() == EINVAL));
#endif
	return ret;
}

static void *
os_pages_trim(void *addr, size_t alloc_size, size_t leadsize, size_t size,
    bool *commit) {
	void *ret = (void *)((byte_t *)addr + leadsize);

	assert(alloc_size >= leadsize + size);
#ifdef _WIN32
	os_pages_unmap(addr, alloc_size);
	void *new_addr = os_pages_map(ret, size, PAGE, commit);
	if (new_addr == ret) {
		return ret;
	}
	if (new_addr != NULL) {
		os_pages_unmap(new_addr, size);
	}
	return NULL;
#else
	size_t trailsize = alloc_size - leadsize - size;

	if (leadsize != 0) {
		os_pages_unmap(addr, leadsize);
	}
	if (trailsize != 0) {
		os_pages_unmap((void *)((byte_t *)ret + size), trailsize);
	}
	return ret;
#endif
}

static void
os_pages_unmap(void *addr, size_t size) {
	assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
	assert(ALIGNMENT_CEILING(size, os_page) == size);

#ifdef _WIN32
	if (VirtualFree(addr, 0, MEM_RELEASE) == 0)
#else
	if (munmap(addr, size) == -1)
#endif
	{
		char buf[BUFERROR_BUF];

		buferror(get_errno(), buf, sizeof(buf));
		malloc_printf("<jemalloc>: Error in "
#ifdef _WIN32
		    "VirtualFree"
#else
		    "munmap"
#endif
		    "(): %s\n", buf);
		if (opt_abort) {
			abort();
		}
	}
}

static void *
pages_map_slow(size_t size, size_t alignment, bool *commit) {
	size_t alloc_size = size + alignment - os_page;
	/* Beware size_t wrap-around. */
	if (alloc_size < size) {
		return NULL;
	}

	void *ret;
	do {
		void *pages = os_pages_map(NULL, alloc_size, alignment, commit);
		if (pages == NULL) {
			return NULL;
		}
		size_t leadsize = ALIGNMENT_CEILING((uintptr_t)pages, alignment)
		    - (uintptr_t)pages;
		ret = os_pages_trim(pages, alloc_size, leadsize, size, commit);
	} while (ret == NULL);

	assert(ret != NULL);
	assert(PAGE_ADDR2BASE(ret) == ret);
	return ret;
}

void *
pages_map(void *addr, size_t size, size_t alignment, bool *commit) {
	assert(alignment >= PAGE);
	assert(ALIGNMENT_ADDR2BASE(addr, alignment) == addr);

#if defined(__FreeBSD__) && defined(MAP_EXCL)
	/*
	 * FreeBSD has mechanisms both to mmap at specific address without
	 * touching existing mappings, and to mmap with specific alignment.
	 */
	{
		if (os_overcommits) {
			*commit = true;
		}

		int prot = *commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;
		int flags = mmap_flags;

		if (addr != NULL) {
			flags |= MAP_FIXED | MAP_EXCL;
		} else {
			unsigned alignment_bits = ffs_zu(alignment);
			assert(alignment_bits > 0);
			flags |= MAP_ALIGNED(alignment_bits);
		}

		void *ret = mmap(addr, size, prot, flags, -1, 0);
		if (ret == MAP_FAILED) {
			ret = NULL;
		}

		return ret;
	}
#endif
	/*
	 * Ideally, there would be a way to specify alignment to mmap() (like
	 * NetBSD has), but in the absence of such a feature, we have to work
	 * hard to efficiently create aligned mappings.  The reliable, but
	 * slow method is to create a mapping that is over-sized, then trim the
	 * excess.  However, that always results in one or two calls to
	 * os_pages_unmap(), and it can leave holes in the process's virtual
	 * memory map if memory grows downward.
	 *
	 * Optimistically try mapping precisely the right amount before falling
	 * back to the slow method, with the expectation that the optimistic
	 * approach works most of the time.
	 */

	void *ret = os_pages_map(addr, size, os_page, commit);
	if (ret == NULL || ret == addr) {
		return ret;
	}
	assert(addr == NULL);
	if (ALIGNMENT_ADDR2OFFSET(ret, alignment) != 0) {
		os_pages_unmap(ret, size);
		return pages_map_slow(size, alignment, commit);
	}

	assert(PAGE_ADDR2BASE(ret) == ret);
	return ret;
}

void
pages_unmap(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);

	os_pages_unmap(addr, size);
}

static bool
os_pages_commit(void *addr, size_t size, bool commit) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);

#ifdef _WIN32
	return (commit ? (addr != VirtualAlloc(addr, size, MEM_COMMIT,
	    PAGE_READWRITE)) : (!VirtualFree(addr, size, MEM_DECOMMIT)));
#else
	{
		int prot = commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;
		void *result = mmap(addr, size, prot, mmap_flags | MAP_FIXED,
		    PAGES_FD_TAG, 0);
		if (result == MAP_FAILED) {
			return true;
		}
		if (result != addr) {
			/*
			 * We succeeded in mapping memory, but not in the right
			 * place.
			 */
			os_pages_unmap(result, size);
			return true;
		}
		return false;
	}
#endif
}

static bool
pages_commit_impl(void *addr, size_t size, bool commit) {
	if (os_overcommits) {
		return true;
	}

	return os_pages_commit(addr, size, commit);
}

bool
pages_commit(void *addr, size_t size) {
	return pages_commit_impl(addr, size, true);
}

bool
pages_decommit(void *addr, size_t size) {
	return pages_commit_impl(addr, size, false);
}

void
pages_mark_guards(void *head, void *tail) {
	assert(head != NULL || tail != NULL);
	assert(head == NULL || tail == NULL ||
	    (uintptr_t)head < (uintptr_t)tail);
#ifdef JEMALLOC_HAVE_MPROTECT
	if (head != NULL) {
		mprotect(head, PAGE, PROT_NONE);
	}
	if (tail != NULL) {
		mprotect(tail, PAGE, PROT_NONE);
	}
#else
	/* Decommit sets to PROT_NONE / MEM_DECOMMIT. */
	if (head != NULL) {
		os_pages_commit(head, PAGE, false);
	}
	if (tail != NULL) {
		os_pages_commit(tail, PAGE, false);
	}
#endif
}

void
pages_unmark_guards(void *head, void *tail) {
	assert(head != NULL || tail != NULL);
	assert(head == NULL || tail == NULL ||
	    (uintptr_t)head < (uintptr_t)tail);
#ifdef JEMALLOC_HAVE_MPROTECT
	bool head_and_tail = (head != NULL) && (tail != NULL);
	size_t range = head_and_tail ?
	    (uintptr_t)tail - (uintptr_t)head + PAGE :
	    SIZE_T_MAX;
	/*
	 * The amount of work that the kernel does in mprotect depends on the
	 * range argument.  SC_LARGE_MINCLASS is an arbitrary threshold chosen
	 * to prevent kernel from doing too much work that would outweigh the
	 * savings of performing one less system call.
	 */
	bool ranged_mprotect = head_and_tail && range <= SC_LARGE_MINCLASS;
	if (ranged_mprotect) {
		mprotect(head, range, PROT_READ | PROT_WRITE);
	} else {
		if (head != NULL) {
			mprotect(head, PAGE, PROT_READ | PROT_WRITE);
		}
		if (tail != NULL) {
			mprotect(tail, PAGE, PROT_READ | PROT_WRITE);
		}
	}
#else
	if (head != NULL) {
		os_pages_commit(head, PAGE, true);
	}
	if (tail != NULL) {
		os_pages_commit(tail, PAGE, true);
	}
#endif
}

bool
pages_purge_lazy(void *addr, size_t size) {
	assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
	assert(PAGE_CEILING(size) == size);

	if (!pages_can_purge_lazy) {
		return true;
	}
	if (!pages_can_purge_lazy_runtime) {
		/*
		 * Built with lazy purge enabled, but detected it was not
		 * supported on the current system.
		 */
		return true;
	}

#ifdef _WIN32
	VirtualAlloc(addr, size, MEM_RESET, PAGE_READWRITE);
	return false;
#elif defined(JEMALLOC_PURGE_MADVISE_FREE)
	return (madvise(addr, size,
#  ifdef MADV_FREE
	    MADV_FREE
#  else
	    JEMALLOC_MADV_FREE
#  endif
	    ) != 0);
#elif defined(JEMALLOC_PURGE_MADVISE_DONTNEED) && \
    !defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS)
	return (madvise(addr, size, MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED) && \
    !defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED_ZEROS)
	return (posix_madvise(addr, size, POSIX_MADV_DONTNEED) != 0);
#else
	not_reached();
#endif
}

bool
pages_purge_forced(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);

	if (!pages_can_purge_forced) {
		return true;
	}

#if defined(JEMALLOC_PURGE_MADVISE_DONTNEED) && \
    defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS)
	return (madvise(addr, size, MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED) && \
    defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED_ZEROS)
	return (posix_madvise(addr, size, POSIX_MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_MAPS_COALESCE)
	/* Try to overlay a new demand-zeroed mapping. */
	return pages_commit(addr, size);
#else
	not_reached();
#endif
}

static bool
pages_huge_impl(void *addr, size_t size, bool aligned) {
	if (aligned) {
		assert(HUGEPAGE_ADDR2BASE(addr) == addr);
		assert(HUGEPAGE_CEILING(size) == size);
	}
#if defined(JEMALLOC_HAVE_MADVISE_HUGE)
	return (madvise(addr, size, MADV_HUGEPAGE) != 0);
#elif defined(JEMALLOC_HAVE_MEMCNTL)
	struct memcntl_mha m = {0};
	m.mha_cmd = MHA_MAPSIZE_VA;
	m.mha_pagesize = HUGEPAGE;
	return (memcntl(addr, size, MC_HAT_ADVISE, (caddr_t)&m, 0, 0) == 0);
#else
	return true;
#endif
}

bool
pages_huge(void *addr, size_t size) {
	return pages_huge_impl(addr, size, true);
}

static bool
pages_huge_unaligned(void *addr, size_t size) {
	return pages_huge_impl(addr, size, false);
}

static bool
pages_nohuge_impl(void *addr, size_t size, bool aligned) {
	if (aligned) {
		assert(HUGEPAGE_ADDR2BASE(addr) == addr);
		assert(HUGEPAGE_CEILING(size) == size);
	}

#ifdef JEMALLOC_HAVE_MADVISE_HUGE
	return (madvise(addr, size, MADV_NOHUGEPAGE) != 0);
#else
	return false;
#endif
}

bool
pages_nohuge(void *addr, size_t size) {
	return pages_nohuge_impl(addr, size, true);
}

static bool
pages_nohuge_unaligned(void *addr, size_t size) {
	return pages_nohuge_impl(addr, size, false);
}

bool
pages_dontdump(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);
#if defined(JEMALLOC_MADVISE_DONTDUMP)
	return madvise(addr, size, MADV_DONTDUMP) != 0;
#elif defined(JEMALLOC_MADVISE_NOCORE)
	return madvise(addr, size, MADV_NOCORE) != 0;
#else
	return false;
#endif
}

bool
pages_dodump(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);
#if defined(JEMALLOC_MADVISE_DONTDUMP)
	return madvise(addr, size, MADV_DODUMP) != 0;
#elif defined(JEMALLOC_MADVISE_NOCORE)
	return madvise(addr, size, MADV_CORE) != 0;
#else
	return false;
#endif
}

#ifdef JEMALLOC_HAVE_PROCESS_MADVISE
#include <sys/mman.h>
#include <sys/syscall.h>
static int pidfd;

static bool
init_process_madvise(void) {
	if (opt_process_madvise_max_batch == 0) {
		return false;
	}

	if (opt_process_madvise_max_batch > PROCESS_MADVISE_MAX_BATCH_LIMIT) {
		opt_process_madvise_max_batch = PROCESS_MADVISE_MAX_BATCH_LIMIT;
	}
	pid_t pid = getpid();
	pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
	if (pidfd == -1) {
		return true;
	}

	return false;
}

static bool
pages_purge_process_madvise_impl(void *vec, size_t vec_len,
    size_t total_bytes) {
	size_t purged_bytes = (size_t)syscall(SYS_process_madvise, pidfd,
	    (struct iovec *)vec, vec_len, MADV_DONTNEED, 0);

	return purged_bytes != total_bytes;
}

#else

static bool
init_process_madvise(void) {
	return false;
}

static bool
pages_purge_process_madvise_impl(void *vec, size_t vec_len,
    size_t total_bytes) {
	not_reached();
	return true;
}

#endif

bool
pages_purge_process_madvise(void *vec, size_t vec_len, size_t total_bytes) {
	return pages_purge_process_madvise_impl(vec, vec_len, total_bytes);
}

static size_t
os_page_detect(void) {
#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return si.dwPageSize;
#elif defined(__FreeBSD__)
	/*
	 * This returns the value obtained from
	 * the auxv vector, avoiding a syscall.
	 */
	return getpagesize();
#else
	long result = sysconf(_SC_PAGESIZE);
	if (result == -1) {
		return LG_PAGE;
	}
	return (size_t)result;
#endif
}

#ifdef JEMALLOC_SYSCTL_VM_OVERCOMMIT
static bool
os_overcommits_sysctl(void) {
	int vm_overcommit;
	size_t sz;

	sz = sizeof(vm_overcommit);
#if defined(__FreeBSD__) && defined(VM_OVERCOMMIT)
	int mib[2];

	mib[0] = CTL_VM;
	mib[1] = VM_OVERCOMMIT;
	if (sysctl(mib, 2, &vm_overcommit, &sz, NULL, 0) != 0) {
		return false; /* Error. */
	}
#else
	if (sysctlbyname("vm.overcommit", &vm_overcommit, &sz, NULL, 0) != 0) {
		return false; /* Error. */
	}
#endif

	return ((vm_overcommit & 0x3) == 0);
}
#endif

#ifdef JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY
/*
 * Use syscall(2) rather than {open,read,close}(2) when possible to avoid
 * reentry during bootstrapping if another library has interposed system call
 * wrappers.
 */
static bool
os_overcommits_proc(void) {
	int fd;
	char buf[1];

#if defined(O_CLOEXEC)
	fd = malloc_open("/proc/sys/vm/overcommit_memory", O_RDONLY | O_CLOEXEC);
#else
	fd = malloc_open("/proc/sys/vm/overcommit_memory", O_RDONLY);
	if (fd != -1) {
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	}
#endif

	if (fd == -1) {
		return false; /* Error. */
	}

	ssize_t nread = malloc_read_fd(fd, &buf, sizeof(buf));
	malloc_close(fd);

	if (nread < 1) {
		return false; /* Error. */
	}
	/*
	 * /proc/sys/vm/overcommit_memory meanings:
	 * 0: Heuristic overcommit.
	 * 1: Always overcommit.
	 * 2: Never overcommit.
	 */
	return (buf[0] == '0' || buf[0] == '1');
}
#endif

bool
pages_boot(void) {
	os_page = os_page_detect();
	if (os_page > PAGE) {
		malloc_write("<jemalloc>: Unsupported system page size\n");
		if (opt_abort) {
			abort();
		}
		return true;
	}

#ifndef _WIN32
	mmap_flags = MAP_PRIVATE | MAP_ANON;
#endif

#ifdef __FreeBSD__
	/*
	 * FreeBSD doesn't need the check; madvise(2) is known to work.
	 */
#else
	/* Detect lazy purge runtime support. */
	if (pages_can_purge_lazy) {
		bool committed = false;
		void *madv_free_page = os_pages_map(NULL, PAGE, PAGE, &committed);
		if (madv_free_page == NULL) {
			return true;
		}
		assert(pages_can_purge_lazy_runtime);
		if (pages_purge_lazy(madv_free_page, PAGE)) {
			pages_can_purge_lazy_runtime = false;
		}
		os_pages_unmap(madv_free_page, PAGE);
	}
#endif
	if (init_process_madvise()) {
		if (opt_abort) {
			abort();
		}
		return true;
	}

	return false;
}
