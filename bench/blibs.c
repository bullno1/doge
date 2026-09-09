// The single translation unit that instantiates the header-only libs, the way
// tests/main.c is for the test runner. barena.h emits its implementation on
// every include once BLIB_IMPLEMENTATION is set, so it cannot share a file with
// anything that pulls the header in again.
#define BLIB_IMPLEMENTATION
#include <blog.h>
#define BARENA_USE_MALLOC
#include <barena.h>
