#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <gnu/lib-names.h>
#include <jemalloc/jemalloc.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "scalene.h"

static char MALLOC_SIGNAL_FILE[256];
static char MEMCPY_SIGNAL_FILE[256];
static const uint32_t SIGNAL_FILE_FLAGS = O_WRONLY | O_CREAT | O_SYNC | O_APPEND;
static const uint32_t SIGNAL_FILE_MODE = S_IRUSR | S_IWUSR;

static void *LIBC_HANDLE = nullptr;
static void *(*MEMCPY)(void *, const void *, size_t) = nullptr;
static void *(*MEMMOVE)(void *, const void *, size_t) = nullptr;
static char *(*STRCPY)(void *, const void *) = nullptr;

static bool RECORDING = false; // avoid self-recursion
static uint32_t MALLOC_TRIGGERED = 0;
static uint32_t FREE_TRIGGERED = 0;
static uint32_t MEMCPY_TRIGGERED = 0;
static uint32_t PHP_ALLOCS = 0;
static uint32_t C_ALLOCS = 0;
static uint32_t MALLOC_SAMPLE = 0;
static uint32_t CALL_STACK_SAMPLE = 0;
static uint32_t FREE_SAMPLE = 0;
static uint32_t MEMCPY_SAMPLE = 0;

[[gnu::constructor, gnu::unused]]
static void init() {
  // assemble signal file names
  int result = snprintf(MALLOC_SIGNAL_FILE, 255, "%s%d",
                        "/tmp/scalene-malloc-signal", getpid());
  if (result <= 0) {
    perror("snprintf() failed");
    abort();
  }

  result = snprintf(MEMCPY_SIGNAL_FILE, 255, "%s%d",
                    "/tmp/scalene-memcpy-signal", getpid());
  if (result <= 0) {
    perror("snprintf() failed");
    abort();
  }

  // disable signals until the profiler completes setup
  signal(MALLOC_SIGNAL, SIG_IGN);
  signal(FREE_SIGNAL, SIG_IGN);
  signal(MEMCPY_SIGNAL, SIG_IGN);

  // prepare to interpose on certain copying functions
  LIBC_HANDLE = dlopen(LIBC_SO, RTLD_LAZY);
  if (LIBC_HANDLE == nullptr) {
    fprintf(stderr, "dlopen(libc) failed: %s\n", dlerror());
    abort();
  }

  MEMCPY = (void *(*)(void *, const void *, size_t)) dlsym(LIBC_HANDLE, "memcpy");
  if (MEMCPY == nullptr) {
    fprintf(stderr, "dlsym(memcpy) failed: %s\n", dlerror());
    abort();
  }

  MEMMOVE = (void *(*)(void *, const void *, size_t)) dlsym(LIBC_HANDLE, "memmove");
  if (MEMMOVE == nullptr) {
    fprintf(stderr, "dlsym(memmove) failed: %s\n", dlerror());
    abort();
  }

  STRCPY = (char *(*)(void *, const void *)) dlsym(LIBC_HANDLE, "strcpy");
  if (STRCPY == nullptr) {
    fprintf(stderr, "dlsym(strcpy) failed: %s\n", dlerror());
    abort();
  }
}

[[gnu::destructor, gnu::unused]]
static void fini() {
  if ((unlink(MALLOC_SIGNAL_FILE) == -1) && (errno != ENOENT)) {
    perror("unlink() failed");
    abort();
  }
  if ((unlink(MEMCPY_SIGNAL_FILE) == -1) && (errno != ENOENT)) {
    perror("unlink() failed");
    abort();
  }
  if (dlclose(LIBC_HANDLE)) {
    fprintf(stderr, "dlclose(libc) failed: %s\n", dlerror());
    abort();
  }
}

static void update_malloc_signal_file(const uint8_t sig, const size_t size) {
  static char buf[256];

  if (PHP_ALLOCS == 0) {
    PHP_ALLOCS = 1; // prevents 0/0
  }

  int result = snprintf(buf, 255, "%s,%u,%ld,%lf\n",
                        (sig == MALLOC_SIGNAL) ? "M" : "F",
                        MALLOC_TRIGGERED + FREE_TRIGGERED,
                        size,
                        (double) PHP_ALLOCS / (PHP_ALLOCS + C_ALLOCS));
  if (result <= 0) {
    perror("snprintf() failed");
    abort();
  }

  int fd = open(MALLOC_SIGNAL_FILE, SIGNAL_FILE_FLAGS, SIGNAL_FILE_MODE);
  if (fd == -1) {
    perror("open() failed");
    abort();
  }

  if (write(fd, buf, strlen(buf)) == -1) {
    perror("write() failed");
    abort();
  }

  if (close(fd) == -1) {
    perror("close() failed");
    abort();
  }
}

static void update_memcpy_signal_file() {
  static char buf[256];

  int result = snprintf(buf, 255, "%u, %u\n", MEMCPY_TRIGGERED, MEMCPY_SAMPLE);
  if (result <= 0) {
    perror("snprintf() failed");
    abort();
  }

  int fd = open(MEMCPY_SIGNAL_FILE, SIGNAL_FILE_FLAGS, SIGNAL_FILE_MODE);
  if (fd == -1) {
    perror("open() failed");
    abort();
  }

  if (write(fd, buf, strlen(buf)) == -1) {
    perror("write() failed");
    abort();
  }

  if (close(fd) == -1) {
    perror("close() failed");
    abort();
  }
}

static void record_call_stack(size_t size) {
  if (RECORDING) {
    return; // avoid self-recursion
  } else {
    RECORDING = true;
  }

  static void *frames[CALL_STACK_INSPECTION_DEPTH];
  static Dl_info dl_info;

  int n = backtrace(frames, CALL_STACK_INSPECTION_DEPTH);
  for (int i = 0; i < n; i++) {
    if (dladdr(frames[i], &dl_info)) {
      if (dl_info.dli_sname != nullptr) {
        if ((strcasestr(dl_info.dli_sname, "zend") != nullptr) ||
            (strcasestr(dl_info.dli_sname, "tsrm") != nullptr))
        {
          PHP_ALLOCS += size;
          return;
        }
      }
    }
  }

  C_ALLOCS += size;
  RECORDING = false;
}

static void record_alloc(size_t size) {
  if (RECORDING) {
    return; // avoid self-recursion
  } else {
    RECORDING = true;
  }

  MALLOC_SAMPLE += size;
  CALL_STACK_SAMPLE += size;

  if (CALL_STACK_SAMPLE >= CALL_STACK_SAMPLING_RATE) {
    record_call_stack(size);
    CALL_STACK_SAMPLE = 0;
  }

  if (MALLOC_SAMPLE >= MALLOC_SAMPLING_RATE) {
    update_malloc_signal_file(MALLOC_SIGNAL, MALLOC_SAMPLE);
    MALLOC_SAMPLE = 0;
    PHP_ALLOCS = 0;
    C_ALLOCS = 0;
    MALLOC_TRIGGERED += 1;

    if (raise(MALLOC_SIGNAL)) {
      perror("raise() failed");
      abort();
    }
  }

  RECORDING = false;
}

static void record_free(size_t size) {
  if (RECORDING) {
    return; // avoid self-recursion
  } else {
    RECORDING = true;
  }

  FREE_SAMPLE += size;

  if (FREE_SAMPLE >= FREE_SAMPLING_RATE) {
    update_malloc_signal_file(FREE_SIGNAL, FREE_SAMPLE);
    FREE_SAMPLE = 0;
    FREE_TRIGGERED += 1;

    if (raise(FREE_SIGNAL)) {
      perror("raise() failed");
      abort();
    }
  }

  RECORDING = false;
}

static void record_copy(size_t size) {
  if (RECORDING) {
    return; // avoid self-recursion
  } else {
    RECORDING = true;
  }

  MEMCPY_SAMPLE += size;

  if (MEMCPY_SAMPLE >= MEMCPY_SAMPLING_RATE) {
    update_memcpy_signal_file();
    MEMCPY_SAMPLE = 0;
    MEMCPY_TRIGGERED += 1;

    if (raise(MEMCPY_SIGNAL)) {
      perror("raise() failed");
      abort();
    }
  }

  RECORDING = false;
}

void *malloc(size_t size) {
//  fprintf(stderr, "malloc(%ld)\n", size);
  void *p = je_malloc(size);
  if (p != nullptr) {
    record_alloc(je_malloc_usable_size(p));
  }

  return p;
}

void free(void *ptr) {
//  fprintf(stderr, "free(%p)\n", ptr);
  if (ptr == nullptr) {
    return;
  }

  size_t size = je_malloc_usable_size(ptr);
  je_free(ptr);
  record_free(size);
}

void *realloc(void *ptr, size_t size) {
//  fprintf(stderr, "realloc(%p, %ld)\n", ptr, size);
  size_t old_size = je_malloc_usable_size(ptr);
  void *p = je_realloc(ptr, size);
  size_t new_size = je_malloc_usable_size(p);

  if (new_size > old_size) {
    record_alloc(new_size - old_size);
  } else if (new_size < old_size) {
    record_free(old_size - new_size);
  }

  return p;
}

void *calloc(size_t num, size_t size) {
//  fprintf(stderr, "calloc(%ld, %ld)\n", num, size);
  void *p = je_calloc(num, size);
  if (p != nullptr) {
    record_alloc(je_malloc_usable_size(p));
  }

  return p;
}

void *memcpy(void *dest, const void *src, size_t n) {
//  fprintf(stderr, "memcpy(%p, %p, %ld)\n", dest, src, n);
  record_copy(n);
  return (*MEMCPY)(dest, src, n);
}

//void *memmove(void *dest, const void *src, size_t n) {
//  fprintf(stderr, "memmove(%p, %p, %ld)\n", dest, src, n);
//  record_copy(n);
//  return (*MEMMOVE)(dest, src, n);
//}

char *strcpy(char *dest, const char *src) {
//  fprintf(stderr, "strcpy(%p, %p)\n", dest, src);
  record_copy(strlen(src) + 1);
  return (*STRCPY)(dest, src);
}
