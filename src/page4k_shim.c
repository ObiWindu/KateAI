#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#define SHIM_EXPORT __attribute__((visibility("default")))

static long (*orig_sysconf)(int) = NULL;
static int (*orig_getpagesize)(void) = NULL;
static long (*orig_syscall)(long, ...) = NULL;
static void *(*orig_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static int (*orig_munmap)(void *, size_t) = NULL;
static int (*orig_madvise)(void *, size_t, int) = NULL;
static size_t cached_real_page_size = 0;
static __thread int in_page_query = 0;

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

static long (*get_orig_sysconf(void))(int) {
    long (*fn)(int) = __atomic_load_n(&orig_sysconf, __ATOMIC_ACQUIRE);
    if (!fn) {
        fn = (long (*)(int))dlsym(RTLD_NEXT, "sysconf");
        __atomic_store_n(&orig_sysconf, fn, __ATOMIC_RELEASE);
    }
    return fn;
}

static int (*get_orig_getpagesize(void))(void) {
    int (*fn)(void) = __atomic_load_n(&orig_getpagesize, __ATOMIC_ACQUIRE);
    if (!fn) {
        fn = (int (*)(void))dlsym(RTLD_NEXT, "getpagesize");
        __atomic_store_n(&orig_getpagesize, fn, __ATOMIC_RELEASE);
    }
    return fn;
}

static long (*get_orig_syscall(void))(long, ...) {
    long (*fn)(long, ...) = __atomic_load_n(&orig_syscall, __ATOMIC_ACQUIRE);
    if (!fn) {
        fn = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
        __atomic_store_n(&orig_syscall, fn, __ATOMIC_RELEASE);
    }
    return fn;
}

static void *(*get_orig_mmap(void))(void *, size_t, int, int, int, off_t) {
    void *(*fn)(void *, size_t, int, int, int, off_t) = __atomic_load_n(&orig_mmap, __ATOMIC_ACQUIRE);
    if (!fn) {
        fn = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
        __atomic_store_n(&orig_mmap, fn, __ATOMIC_RELEASE);
    }
    return fn;
}

static int (*get_orig_munmap(void))(void *, size_t) {
    int (*fn)(void *, size_t) = __atomic_load_n(&orig_munmap, __ATOMIC_ACQUIRE);
    if (!fn) {
        fn = (int (*)(void *, size_t))dlsym(RTLD_NEXT, "munmap");
        __atomic_store_n(&orig_munmap, fn, __ATOMIC_RELEASE);
    }
    return fn;
}

static int (*get_orig_madvise(void))(void *, size_t, int) {
    int (*fn)(void *, size_t, int) = __atomic_load_n(&orig_madvise, __ATOMIC_ACQUIRE);
    if (!fn) {
        fn = (int (*)(void *, size_t, int))dlsym(RTLD_NEXT, "madvise");
        __atomic_store_n(&orig_madvise, fn, __ATOMIC_RELEASE);
    }
    return fn;
}

static size_t real_page_size(void) {
    size_t cached = __atomic_load_n(&cached_real_page_size, __ATOMIC_ACQUIRE);
    if (cached != 0) {
        return cached;
    }
    long (*fn)(int) = get_orig_sysconf();
    long ps = fn ? fn(_SC_PAGE_SIZE) : 16384;
    size_t resolved = ps > 0 ? (size_t)ps : 16384;
    __atomic_store_n(&cached_real_page_size, resolved, __ATOMIC_RELEASE);
    return resolved;
}

// Legacy agy_acp_server.par embeds Google startup initializers (bamm2 and usps
// `verify_page_size`) that CHECK_EQ(sysconf(_SC_PAGE_SIZE), 4096) and abort on
// >4 KiB kernels (e.g. 16 KiB Apple Silicon / Asahi Linux).
//
// Returning a spoofed 4096 to the whole process is fatal to memory allocators:
// TCMalloc (8 KiB logical pages) caches sysconf(_SC_PAGESIZE) in
// `tcmalloc_internal::GetPageSize()` and uses it to align `madvise(MADV_DONTNEED)`
// ranges inward. When lied to with 4096 on a 16 KiB kernel, TCMalloc issues
// 8 KiB madvise calls via direct `svc #0`; the kernel rounds the length up to
// 16 KiB and silently zeroes the adjacent 8 KiB of live Python heap objects,
// causing SIGSEGV in _PyEval_EvalFrameDefault after a few prompts/tool calls.
//
// Only spoof 4096 for the startup `verify_page_size` assertions; every real
// allocator (TCMalloc, mimalloc, Abseil LowLevelAlloc, CPython) and any child
// process must observe the true kernel page size.
// Resolve the exact exported symbol containing `addr` (verifying `addr` lies
// within [dli_saddr, dli_saddr + st_size)). Plain dladdr() returns the nearest
// preceding symbol even for static/internal functions sitting past its end.
static const char *exact_symbol_name(void *addr) {
    if (!addr) {
        return NULL;
    }
    // Return addresses point to the instruction *after* the call. If the call
    // was the last instruction in the caller, addr equals start + st_size.
    // Probing addr - 1 lands strictly inside the caller function.
    void *probe = (void *)((uintptr_t)addr - 1);
    Dl_info info;
    const ElfW(Sym) *sym = NULL;
    if (!dladdr1(probe, &info, (void **)&sym, RTLD_DL_SYMENT) || !info.dli_sname || !info.dli_saddr) {
        return NULL;
    }
    if (sym && sym->st_size > 0) {
        uintptr_t pc = (uintptr_t)probe;
        uintptr_t start = (uintptr_t)info.dli_saddr;
        if (pc < start || pc >= start + (uintptr_t)sym->st_size) {
            return NULL;
        }
    }
    return info.dli_sname;
}

__attribute__((noinline))
static int caller_needs_4k_page_size(void *ret0, uintptr_t fp0) {
    const char *sym0 = exact_symbol_name(ret0);
    if (sym0) {
        return strstr(sym0, "verify_page_size") != NULL
            || strstr(sym0, "GoogleInitializer") != NULL;
    }

    // Static startup initializers (e.g. google_init_module_*verify_page_size)
    // are not in .dynsym, so exact_symbol_name(ret0) is NULL. Walk at most two
    // caller frames via the frame pointer chain to see whether the immediate
    // caller is GoogleInitializer::TypeData::RunIfNecessary / RunOne.
    uintptr_t fp = fp0;
    for (int depth = 0; depth < 3; depth++) {
        if (!fp || (fp & 0x7) != 0) {
            break;
        }
        if (fp < fp0 || (fp - fp0) > (256 * 1024)) {
            break;
        }
        uintptr_t next_fp = ((const uintptr_t *)fp)[0];
        void *lr = (void *)((const uintptr_t *)fp)[1];
        if (depth >= 1 && lr) {
            const char *parent_sym = exact_symbol_name(lr);
            if (parent_sym) {
                if (strstr(parent_sym, "GoogleInitializer") != NULL
                    || strstr(parent_sym, "verify_page_size") != NULL) {
                    return 1;
                }
                break;
            }
        }
        if (next_fp <= fp || (next_fp - fp0) > (256 * 1024)) {
            break;
        }
        fp = next_fp;
    }
    return 0;
}

SHIM_EXPORT long sysconf(int name) {
    if (name == _SC_PAGE_SIZE || name == _SC_PAGESIZE) {
        const size_t real_ps = real_page_size();
        if (real_ps > 4096 && !in_page_query) {
            in_page_query = 1;
            const int spoof = caller_needs_4k_page_size(
                __builtin_return_address(0),
                (uintptr_t)__builtin_frame_address(0));
            in_page_query = 0;
            if (spoof) {
                return 4096;
            }
        }
        return (long)real_ps;
    }
    long (*fn)(int) = get_orig_sysconf();
    return fn ? fn(name) : -1;
}

SHIM_EXPORT int getpagesize(void) {
    const size_t real_ps = real_page_size();
    if (real_ps > 4096 && !in_page_query) {
        in_page_query = 1;
        const int spoof = caller_needs_4k_page_size(
            __builtin_return_address(0),
            (uintptr_t)__builtin_frame_address(0));
        in_page_query = 0;
        if (spoof) {
            return 4096;
        }
    }
    int (*fn)(void) = get_orig_getpagesize();
    return fn ? fn() : (int)real_ps;
}

SHIM_EXPORT int madvise(void *addr, size_t length, int advice) {
    if (advice == MADV_HUGEPAGE) {
        // Antigravity's bundled hugepage text remapper assumes 2 MiB aligned
        // ELF mappings and can fail on >4 KiB ARM64 kernels. Returning EINVAL
        // makes callers take their small-page fallback.
        errno = EINVAL;
        return -1;
    }
    int (*fn)(void *, size_t, int) = get_orig_madvise();
    return fn ? fn(addr, length, advice) : -1;
}

struct MapEntry {
    void *user_ptr;
    void *real_ptr;
    size_t real_length;
};
#define INITIAL_MAPS 4096
#define MAX_MAPS_CAP (1024 * 1024)
static struct MapEntry static_maps[INITIAL_MAPS];
static struct MapEntry *maps_table = static_maps;
static size_t maps_capacity = INITIAL_MAPS;
static size_t num_maps = 0;
static pthread_mutex_t map_mutex = PTHREAD_MUTEX_INITIALIZER;

static int grow_maps_locked(void) {
    if (maps_capacity >= MAX_MAPS_CAP) {
        return 0;
    }
    void *(*orig_mmap_fn)(void *, size_t, int, int, int, off_t) = get_orig_mmap();
    int (*orig_munmap_fn)(void *, size_t) = get_orig_munmap();
    if (!orig_mmap_fn || !orig_munmap_fn) {
        return 0;
    }
    size_t new_cap = maps_capacity * 2;
    if (new_cap > MAX_MAPS_CAP) {
        new_cap = MAX_MAPS_CAP;
    }
    size_t new_bytes = new_cap * sizeof(struct MapEntry);
    void *mem = orig_mmap_fn(NULL, new_bytes, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return 0;
    }
    memcpy(mem, maps_table, num_maps * sizeof(struct MapEntry));
    if (maps_table != static_maps) {
        orig_munmap_fn(maps_table, maps_capacity * sizeof(struct MapEntry));
    }
    maps_table = (struct MapEntry *)mem;
    maps_capacity = new_cap;
    return 1;
}

static int record_map(void *user_ptr, void *real_ptr, size_t real_length) {
    int ok = 1;
    pthread_mutex_lock(&map_mutex);
    if (num_maps >= maps_capacity) {
        ok = grow_maps_locked();
    }
    if (ok) {
        maps_table[num_maps].user_ptr = user_ptr;
        maps_table[num_maps].real_ptr = real_ptr;
        maps_table[num_maps].real_length = real_length;
        num_maps++;
    }
    pthread_mutex_unlock(&map_mutex);
    return ok;
}

static int unrecord_map(void *addr, void **out_real_ptr, size_t *out_real_length) {
    int found = 0;
    pthread_mutex_lock(&map_mutex);
    for (size_t i = 0; i < num_maps; i++) {
        if (maps_table[i].user_ptr == addr) {
            *out_real_ptr = maps_table[i].real_ptr;
            *out_real_length = maps_table[i].real_length;
            maps_table[i] = maps_table[num_maps - 1];
            num_maps--;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&map_mutex);
    return found;
}

SHIM_EXPORT void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    void *(*fn)(void *, size_t, int, int, int, off_t) = get_orig_mmap();
    const size_t real_page = real_page_size();
    if (real_page > 4096 && fd >= 0 && !(flags & (MAP_FIXED | MAP_ANONYMOUS))
        && (offset & (off_t)(real_page - 1)) != 0) {
        off_t aligned_offset = offset & ~(off_t)(real_page - 1);
        size_t diff = (size_t)(offset - aligned_offset);
        size_t aligned_length = length + diff;
        void *real_ptr = fn(addr, aligned_length, prot, flags, fd, aligned_offset);
        if (real_ptr == MAP_FAILED) {
            return MAP_FAILED;
        }
        void *user_ptr = (char *)real_ptr + diff;
        if (!record_map(user_ptr, real_ptr, aligned_length)) {
            int (*munmap_fn)(void *, size_t) = get_orig_munmap();
            if (munmap_fn) {
                munmap_fn(real_ptr, aligned_length);
            }
            errno = ENOMEM;
            return MAP_FAILED;
        }
        return user_ptr;
    }
    return fn(addr, length, prot, flags, fd, offset);
}

SHIM_EXPORT int munmap(void *addr, size_t length) {
    int (*fn)(void *, size_t) = get_orig_munmap();
    void *real_ptr = NULL;
    size_t real_length = 0;
    if (unrecord_map(addr, &real_ptr, &real_length)) {
        return fn(real_ptr, real_length);
    }
    return fn(addr, length);
}

SHIM_EXPORT long syscall(long number, ...) {
    long (*fn)(long, ...) = get_orig_syscall();
    va_list args;
    va_start(args, number);
    long a1 = va_arg(args, long);
    long a2 = va_arg(args, long);
    long a3 = va_arg(args, long);
    long a4 = va_arg(args, long);
    long a5 = va_arg(args, long);
    long a6 = va_arg(args, long);
    va_end(args);

    if (number == __NR_mmap) {
        void *addr = (void *)a1;
        size_t length = (size_t)a2;
        int prot = (int)a3;
        int flags = (int)a4;
        int fd = (int)a5;
        off_t offset = (off_t)a6;

        const size_t real_page = real_page_size();
        if (real_page > 4096 && fd >= 0 && !(flags & (MAP_FIXED | MAP_ANONYMOUS))
            && (offset & (off_t)(real_page - 1)) != 0) {
            off_t aligned_offset = offset & ~(off_t)(real_page - 1);
            size_t diff = (size_t)(offset - aligned_offset);
            size_t aligned_length = length + diff;
            long res = fn(__NR_mmap, addr, aligned_length, prot, flags, fd, aligned_offset);
            if ((void *)res == MAP_FAILED) {
                return res;
            }
            void *user_ptr = (char *)res + diff;
            if (!record_map(user_ptr, (void *)res, aligned_length)) {
                fn(__NR_munmap, (void *)res, aligned_length);
                errno = ENOMEM;
                return (long)MAP_FAILED;
            }
            return (long)user_ptr;
        }
    } else if (number == __NR_munmap) {
        void *addr = (void *)a1;
        void *real_ptr = NULL;
        size_t real_length = 0;
        if (unrecord_map(addr, &real_ptr, &real_length)) {
            return fn(__NR_munmap, real_ptr, real_length);
        }
    }

    return fn(number, a1, a2, a3, a4, a5, a6);
}
