#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  void *p = malloc(512);
  if (p == NULL) {
    fprintf(stderr, "malloc() failed\n");
  } else {
    free(p);
  }

  p = calloc(8, 64);
  if (p == NULL) {
    fprintf(stderr, "calloc() failed\n");
  }

  p = realloc(p, 1024);
  if (p == NULL) {
    fprintf(stderr, "realloc() failed\n");
  } else {
    free(p);
  }

  /* ================================================= */

  void *p1 = malloc(1024 * 1024);
  if (p1 == NULL) {
    fprintf(stderr, "malloc() failed\n");
  }
  void *p2 = malloc(1024 * 1024);
  if (p2 == NULL) {
    fprintf(stderr, "malloc() failed\n");
  }

  memcpy(p2, p1, 1024 * 1024);
  memmove(p2, p1, 1024 * 1024);
  strcpy(p2, p1);

  free(p1);
  free(p2);

  return 0;
}
