#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>

static long (*orig_sysconf)(int) = NULL;
static long (*orig_syscall)(long, ...) = NULL;
static void *(*orig_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static int (*orig_munmap)(void *, size_t) = NULL;
static int (*orig_madvise)(void *, size_t, int) = NULL;

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

static size_t real_page_size(void) {
    if (!orig_sysconf) {
        orig_sysconf = (long (*)(int))dlsym(RTLD_NEXT, "sysconf");
    }
    long ps = orig_sysconf ? orig_sysconf(_SC_PAGE_SIZE) : 16384;
    return ps > 0 ? (size_t)ps : 16384;
}

long sysconf(int name) {
    if (name == _SC_PAGE_SIZE || name == _SC_PAGESIZE) {
        return 4096;
    }
    if (!orig_sysconf) {
        orig_sysconf = (long (*)(int))dlsym(RTLD_NEXT, "sysconf");
    }
    return orig_sysconf ? orig_sysconf(name) : 4096;
}

int getpagesize(void) {
    return 4096;
}

int madvise(void *addr, size_t length, int advice) {
    if (advice == MADV_HUGEPAGE) {
        // Antigravity's bundled Chromium hugepage text remapper assumes 2 MiB
        // aligned ELF mappings and can crash on this ARM64 runtime. Returning
        // an unsupported-advice error makes it take its documented small-page
        // fallback without affecting ordinary mappings.
        errno = EINVAL;
        return -1;
    }
    if (!orig_madvise) {
        orig_madvise = (int (*)(void *, size_t, int))dlsym(RTLD_NEXT, "madvise");
    }
    return orig_madvise ? orig_madvise(addr, length, advice) : -1;
}

struct MapEntry {
    void *user_ptr;
    void *real_ptr;
    size_t real_length;
};
#define MAX_MAPS 4096
static struct MapEntry maps[MAX_MAPS];
static int num_maps = 0;
static pthread_mutex_t map_mutex = PTHREAD_MUTEX_INITIALIZER;

static void record_map(void *user_ptr, void *real_ptr, size_t real_length) {
    pthread_mutex_lock(&map_mutex);
    if (num_maps < MAX_MAPS) {
        maps[num_maps].user_ptr = user_ptr;
        maps[num_maps].real_ptr = real_ptr;
        maps[num_maps].real_length = real_length;
        num_maps++;
    }
    pthread_mutex_unlock(&map_mutex);
}

static int unrecord_map(void *addr, void **out_real_ptr, size_t *out_real_length) {
    int found = 0;
    pthread_mutex_lock(&map_mutex);
    for (int i = 0; i < num_maps; i++) {
        if (maps[i].user_ptr == addr) {
            *out_real_ptr = maps[i].real_ptr;
            *out_real_length = maps[i].real_length;
            maps[i] = maps[num_maps - 1];
            num_maps--;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&map_mutex);
    return found;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!orig_mmap) {
        orig_mmap = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    }
    const size_t real_page = real_page_size();
    if (real_page > 4096 && fd >= 0 && (offset & (off_t)(real_page - 1)) != 0) {
        off_t aligned_offset = offset & ~(off_t)(real_page - 1);
        size_t diff = (size_t)(offset - aligned_offset);
        size_t aligned_length = length + diff;
        void *real_ptr = orig_mmap(addr, aligned_length, prot, flags, fd, aligned_offset);
        if (real_ptr == MAP_FAILED) {
            return MAP_FAILED;
        }
        void *user_ptr = (char *)real_ptr + diff;
        record_map(user_ptr, real_ptr, aligned_length);
        return user_ptr;
    }
    return orig_mmap(addr, length, prot, flags, fd, offset);
}

int munmap(void *addr, size_t length) {
    if (!orig_munmap) {
        orig_munmap = (int (*)(void *, size_t))dlsym(RTLD_NEXT, "munmap");
    }
    void *real_ptr = NULL;
    size_t real_length = 0;
    if (unrecord_map(addr, &real_ptr, &real_length)) {
        return orig_munmap(real_ptr, real_length);
    }
    return orig_munmap(addr, length);
}

long syscall(long number, ...) {
    if (!orig_syscall) {
        orig_syscall = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
    }
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
        if (real_page > 4096 && fd >= 0 && (offset & (off_t)(real_page - 1)) != 0) {
            off_t aligned_offset = offset & ~(off_t)(real_page - 1);
            size_t diff = (size_t)(offset - aligned_offset);
            size_t aligned_length = length + diff;
            long res = orig_syscall(__NR_mmap, addr, aligned_length, prot, flags, fd, aligned_offset);
            if ((void *)res == MAP_FAILED) {
                return res;
            }
            void *user_ptr = (char *)res + diff;
            record_map(user_ptr, (void *)res, aligned_length);
            return (long)user_ptr;
        }
    } else if (number == __NR_munmap) {
        void *addr = (void *)a1;
        void *real_ptr = NULL;
        size_t real_length = 0;
        if (unrecord_map(addr, &real_ptr, &real_length)) {
            return orig_syscall(__NR_munmap, real_ptr, real_length);
        }
    }

    return orig_syscall(number, a1, a2, a3, a4, a5, a6);
}
