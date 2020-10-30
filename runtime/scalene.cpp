#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <gnu/lib-names.h>
#include <jemalloc/jemalloc.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>

#include "scalene.h"

static std::mutex SIGNAL_FILES_LOCK;
static char MALLOC_SIGNAL_FILE_NAME[256];
static char MEMCPY_SIGNAL_FILE_NAME[256];
static int MALLOC_SIGNAL_FILE = -1;
static int MEMCPY_SIGNAL_FILE = -1;
static void *MALLOC_SIGNAL_FILE_MAPPING = nullptr;
static void *MEMCPY_SIGNAL_FILE_MAPPING = nullptr;
static size_t MALLOC_SIGNAL_FILE_MAPPING_OFFSET = 0;
static size_t MEMCPY_SIGNAL_FILE_MAPPING_OFFSET = 0;
static size_t MALLOC_SIGNAL_FILE_SIZE = 1000;         // 1 KB default
static size_t MEMCPY_SIGNAL_FILE_SIZE = 1000;         // 1 KB default
static const size_t SIGNAL_FILE_MIN_SPACE_LEFT = 500; // 0.5 KB
static const int SIGNAL_FILE_FLAGS = O_RDWR | O_CREAT;
static const mode_t SIGNAL_FILE_MODE = S_IRUSR | S_IWUSR;
static const int SIGNAL_FILE_MMAP_PROT = PROT_WRITE;
static const int SIGNAL_FILE_MMAP_FLAGS = MAP_SHARED;

static void *LIBC_HANDLE = nullptr;
static void *(*MEMCPY)(void *, const void *, size_t) = nullptr;
static void *(*MEMMOVE)(void *, const void *, size_t) = nullptr;
static char *(*STRCPY)(void *, const void *) = nullptr;
static pid_t (*FORK)() = nullptr;

static thread_local bool SHOULD_RECORD = true; // avoid self-recursion
static thread_local uint32_t MALLOC_TRIGGERED = 0;
static thread_local uint32_t FREE_TRIGGERED = 0;
static thread_local uint32_t MEMCPY_TRIGGERED = 0;
static thread_local uint32_t PHP_ALLOCS = 0;
static thread_local uint32_t C_ALLOCS = 0;
static thread_local uint32_t MALLOC_SAMPLE = 0;
static thread_local uint32_t CALL_STACK_SAMPLE = 0;
static thread_local uint32_t FREE_SAMPLE = 0;
static thread_local uint32_t MEMCPY_SAMPLE = 0;

static void close_signal_files() {
  if (munmap(MALLOC_SIGNAL_FILE_MAPPING, MALLOC_SIGNAL_FILE_SIZE) == -1) {
    perror("munmap() failed");
    abort();
  }
  if (munmap(MEMCPY_SIGNAL_FILE_MAPPING, MEMCPY_SIGNAL_FILE_SIZE) == -1) {
    perror("munmap() failed");
    abort();
  }
  if (close(MALLOC_SIGNAL_FILE) == -1) {
    perror("close() failed");
    abort();
  }
  if (close(MEMCPY_SIGNAL_FILE) == -1) {
    perror("close() failed");
    abort();
  }

  // reset for child process (if any)
  MALLOC_SIGNAL_FILE = -1;
  MEMCPY_SIGNAL_FILE = -1;
  MALLOC_SIGNAL_FILE_MAPPING = nullptr;
  MEMCPY_SIGNAL_FILE_MAPPING = nullptr;
  MALLOC_SIGNAL_FILE_MAPPING_OFFSET = 0;
  MEMCPY_SIGNAL_FILE_MAPPING_OFFSET = 0;
  MALLOC_SIGNAL_FILE_SIZE = 1000;
  MEMCPY_SIGNAL_FILE_SIZE = 1000;
}

static void open_signal_files() {
  // close signal files if they are open (this can happen after fork)
  if (MALLOC_SIGNAL_FILE != -1) {
    close_signal_files();
  }

  // assemble signal file names
  int result = snprintf(MALLOC_SIGNAL_FILE_NAME, 256, "%s%d",
                        "/tmp/scalene-malloc-signal", getpid());
  if (result <= 0) {
    perror("snprintf() failed");
    abort();
  } else if (result >= 256) {
    fprintf(stderr, "MALLOC_SIGNAL_FILE_NAME overflow\n");
    abort();
  }

  result = snprintf(MEMCPY_SIGNAL_FILE_NAME, 256, "%s%d",
                    "/tmp/scalene-memcpy-signal", getpid());
  if (result <= 0) {
    perror("snprintf() failed");
    abort();
  } else if (result >= 256) {
    fprintf(stderr, "MEMCPY_SIGNAL_FILE_NAME overflow\n");
    abort();
  }

  // open signal files
  MALLOC_SIGNAL_FILE = open(MALLOC_SIGNAL_FILE_NAME, SIGNAL_FILE_FLAGS, SIGNAL_FILE_MODE);
  if (MALLOC_SIGNAL_FILE == -1) {
    perror("open() failed");
    abort();
  }

  MEMCPY_SIGNAL_FILE = open(MEMCPY_SIGNAL_FILE_NAME, SIGNAL_FILE_FLAGS, SIGNAL_FILE_MODE);
  if (MEMCPY_SIGNAL_FILE == -1) {
    perror("open() failed");
    abort();
  }

  // resize signal files
  if (ftruncate(MALLOC_SIGNAL_FILE, MALLOC_SIGNAL_FILE_SIZE) == -1) {
    perror("ftruncate() failed");
    abort();
  }
  if (ftruncate(MEMCPY_SIGNAL_FILE, MEMCPY_SIGNAL_FILE_SIZE) == -1) {
    perror("ftruncate() failed");
    abort();
  }

  // mmap signal files
  MALLOC_SIGNAL_FILE_MAPPING = mmap(nullptr, MALLOC_SIGNAL_FILE_SIZE,
                                    SIGNAL_FILE_MMAP_PROT, SIGNAL_FILE_MMAP_FLAGS,
                                    MALLOC_SIGNAL_FILE, 0);
  if (MALLOC_SIGNAL_FILE_MAPPING == MAP_FAILED) {
    perror("mmap() failed");
    abort();
  }

  MEMCPY_SIGNAL_FILE_MAPPING = mmap(nullptr, MEMCPY_SIGNAL_FILE_SIZE,
                                    SIGNAL_FILE_MMAP_PROT, SIGNAL_FILE_MMAP_FLAGS,
                                    MEMCPY_SIGNAL_FILE, 0);
  if (MEMCPY_SIGNAL_FILE_MAPPING == MAP_FAILED) {
    perror("mmap() failed");
    abort();
  }
}

[[gnu::constructor, gnu::unused]]
static void init() {
  // disable profiling
  SHOULD_RECORD = false;

  // set up signal files
  open_signal_files();

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

  FORK = (pid_t (*)()) dlsym(LIBC_HANDLE, "fork");
  if (FORK == nullptr) {
    fprintf(stderr, "dlsym(fork) failed: %s\n", dlerror());
    abort();
  }

  // enable profiling
  SHOULD_RECORD = true;
}

[[gnu::destructor, gnu::unused]]
static void fini() {
  // disable profiling
  SHOULD_RECORD = false;

  // release resources
  close_signal_files();
  if (unlink(MALLOC_SIGNAL_FILE_NAME) == -1) {
    perror("unlink() failed");
    abort();
  }
  if (unlink(MEMCPY_SIGNAL_FILE_NAME) == -1) {
    perror("unlink() failed");
    abort();
  }
  if (dlclose(LIBC_HANDLE)) {
    fprintf(stderr, "dlclose(libc) failed: %s\n", dlerror());
    abort();
  }
}

static void *backup_memmove(void *dest, const void *src, size_t n) {
  const char *from = reinterpret_cast<const char *>(src);
  char *to = reinterpret_cast<char *>(dest);

  if (from > to) {
    for (int i = 0; i < n; i++) {
      to[i] = from[i];
    }
  } else if (from < to) {
    for (int i = 1; i <= n; i++) {
      to[n - i] = from[n - i];
    }
  } // do nothing if from = to

  return dest;
}

static void update_malloc_signal_file(const uint8_t sig, const size_t size) {
  const std::lock_guard<std::mutex> lock(SIGNAL_FILES_LOCK);

  if (PHP_ALLOCS == 0) {
    PHP_ALLOCS = 1; // prevents 0/0
  }

  char *dest = reinterpret_cast<char *>(MALLOC_SIGNAL_FILE_MAPPING) +
               MALLOC_SIGNAL_FILE_MAPPING_OFFSET;
  size_t remaining_space =
      MALLOC_SIGNAL_FILE_SIZE - MALLOC_SIGNAL_FILE_MAPPING_OFFSET;

  // the extra \n serves as an end marker that will be overwritten the next time
  int result = snprintf(dest, remaining_space, "%ld,%s,%u,%ld,%lf\n\n",
                        pthread_self(),
                        (sig == MALLOC_SIGNAL) ? "M" : "F",
                        MALLOC_TRIGGERED + FREE_TRIGGERED,
                        size,
                        (double) PHP_ALLOCS / (PHP_ALLOCS + C_ALLOCS));
  if (result <= 0) {
    perror("snprintf() failed");
    abort();
  } else if (result >= remaining_space) {
    fprintf(stderr, "malloc signal file overflow\n");
    abort();
  }

  MALLOC_SIGNAL_FILE_MAPPING_OFFSET += (result - 1); // adjust for end marker
  if ((MALLOC_SIGNAL_FILE_MAPPING_OFFSET + SIGNAL_FILE_MIN_SPACE_LEFT) >
      MALLOC_SIGNAL_FILE_SIZE)
  {
    // enlarge signal file
    if (ftruncate(MALLOC_SIGNAL_FILE, MALLOC_SIGNAL_FILE_SIZE * 2) == -1) {
      perror("ftruncate() failed");
      abort();
    }

    // remap signal file
    MALLOC_SIGNAL_FILE_MAPPING = mremap(MALLOC_SIGNAL_FILE_MAPPING,
                                        MALLOC_SIGNAL_FILE_SIZE,
                                        MALLOC_SIGNAL_FILE_SIZE * 2,
                                        MREMAP_MAYMOVE);
    if (MALLOC_SIGNAL_FILE_MAPPING == MAP_FAILED) {
      perror("mremap() failed");
      abort();
    }

    MALLOC_SIGNAL_FILE_SIZE *= 2;
  }
}

static void update_memcpy_signal_file() {
  const std::lock_guard<std::mutex> lock(SIGNAL_FILES_LOCK);

  char *dest = reinterpret_cast<char *>(MEMCPY_SIGNAL_FILE_MAPPING) +
               MEMCPY_SIGNAL_FILE_MAPPING_OFFSET;
  size_t remaining_space =
      MEMCPY_SIGNAL_FILE_SIZE - MEMCPY_SIGNAL_FILE_MAPPING_OFFSET;

  // the extra \n serves as an end marker that will be overwritten the next time
  int result = snprintf(dest, remaining_space, "%ld,%u,%u\n\n",
                        pthread_self(), MEMCPY_TRIGGERED, MEMCPY_SAMPLE);
  if (result <= 0) {
    perror("snprintf() failed");
    abort();
  } else if (result >= remaining_space) {
    fprintf(stderr, "memcpy signal file overflow\n");
    abort();
  }

  MEMCPY_SIGNAL_FILE_MAPPING_OFFSET += (result - 1); // adjust for end marker
  if ((MEMCPY_SIGNAL_FILE_MAPPING_OFFSET + SIGNAL_FILE_MIN_SPACE_LEFT) >
      MEMCPY_SIGNAL_FILE_SIZE)
  {
    // enlarge signal file
    if (ftruncate(MEMCPY_SIGNAL_FILE, MEMCPY_SIGNAL_FILE_SIZE * 2) == -1) {
      perror("ftruncate() failed");
      abort();
    }

    // remap signal file
    MEMCPY_SIGNAL_FILE_MAPPING = mremap(MEMCPY_SIGNAL_FILE_MAPPING,
                                        MEMCPY_SIGNAL_FILE_SIZE,
                                        MEMCPY_SIGNAL_FILE_SIZE * 2,
                                        MREMAP_MAYMOVE);
    if (MEMCPY_SIGNAL_FILE_MAPPING == MAP_FAILED) {
      perror("mremap() failed");
      abort();
    }

    MEMCPY_SIGNAL_FILE_SIZE *= 2;
  }
}

static void record_call_stack(size_t size) {
  static void *frames[CALL_STACK_INSPECTION_DEPTH];
  static Dl_info dl_info;

  int n = backtrace(frames, CALL_STACK_INSPECTION_DEPTH);
  for (int i = 0; i < n; i++) {
    if (dladdr(frames[i], &dl_info)) {
      if (dl_info.dli_sname != nullptr) {
        if ((strcasestr(dl_info.dli_sname, "zif_ffi_trampoline") != nullptr))
        {
          C_ALLOCS += size;
          return;
        }
      }
    }
  }

  PHP_ALLOCS += size;
}

static void record_alloc(size_t size) {
  if (SHOULD_RECORD) {
    SHOULD_RECORD = false;
  } else {
    return; // avoid self-recursion
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

  SHOULD_RECORD = true;
}

static void record_free(size_t size) {
  if (SHOULD_RECORD) {
    SHOULD_RECORD = false;
  } else {
    return; // avoid self-recursion
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

  SHOULD_RECORD = true;
}

static void record_copy(size_t size) {
  if (SHOULD_RECORD) {
    SHOULD_RECORD = false;
  } else {
    return; // avoid self-recursion
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

  SHOULD_RECORD = true;
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

void *memmove(void *dest, const void *src, size_t n) {
//  fprintf(stderr, "memmove(%p, %p, %ld)\n", dest, src, n);
  if (MEMMOVE == nullptr) {
    return backup_memmove(dest, src, n);
  }

  record_copy(n);
  return (*MEMMOVE)(dest, src, n);
}

char *strcpy(char *dest, const char *src) {
//  fprintf(stderr, "strcpy(%p, %p)\n", dest, src);
  record_copy(strlen(src) + 1);
  return (*STRCPY)(dest, src);
}

pid_t fork() {
  pid_t result = (*FORK)();

  if (result == 0) { // child
    // set up signal files
    SHOULD_RECORD = false;
    open_signal_files();
    SHOULD_RECORD = true;

    // reset counters
    MALLOC_TRIGGERED = 0;
    FREE_TRIGGERED = 0;
    MEMCPY_TRIGGERED = 0;
    PHP_ALLOCS = 0;
    C_ALLOCS = 0;
    MALLOC_SAMPLE = 0;
    CALL_STACK_SAMPLE = 0;
    FREE_SAMPLE = 0;
    MEMCPY_SAMPLE = 0;
  }

  return result;
}
