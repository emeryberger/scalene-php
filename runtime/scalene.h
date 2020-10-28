#ifndef SCALENE_PHP_SCALENE_H
#define SCALENE_PHP_SCALENE_H

#include <csignal>

const size_t MALLOC_SAMPLING_RATE = 1048549; // ~= 1MiB
const size_t FREE_SAMPLING_RATE = 1048549;   // ~= 1MiB
const size_t CALL_STACK_SAMPLING_RATE = MALLOC_SAMPLING_RATE * 10;
const size_t CALL_STACK_INSPECTION_DEPTH = 10; // # of frames to check
const size_t MEMCPY_SAMPLING_RATE = 2097131; // next prime after MALLOC_SAMPLING_RATE * 2 + 1;

const uint8_t MALLOC_SIGNAL = SIGXCPU;
const uint8_t FREE_SIGNAL = SIGXFSZ;
const uint8_t MEMCPY_SIGNAL = SIGPROF;

extern "C" {
  void *malloc(size_t size);
  void free(void *ptr);
  void *realloc(void *ptr, size_t size);
  void *calloc(size_t num, size_t size);
  void *memcpy(void *dest, const void *src, size_t n);
  void *memmove(void *dest, const void *src, size_t n);
  char *strcpy(char *dest, const char *src);
}

#endif // SCALENE_PHP_SCALENE_H
