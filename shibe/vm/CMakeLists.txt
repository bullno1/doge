set(SOURCES
	"src/shibe.c"
)
add_library(shibe STATIC "${SOURCES}")
target_include_directories(shibe PUBLIC "include")
target_link_libraries(shibe PRIVATE blibs)

# Pins the interpreter's dispatch strategy, see shibe/vm/src/exec.h for what the
# numbers mean. Left unset the header picks one per compiler, which is what a
# normal build wants; the ci matrix sets it to cover all three.
# Tested with DEFINED because computed goto is 0, which cmake reads as false.
if (DEFINED SHIBE_DISPATCH_METHOD AND NOT SHIBE_DISPATCH_METHOD STREQUAL "")
	target_compile_definitions(shibe
		PRIVATE SHIBE_DISPATCH_METHOD=${SHIBE_DISPATCH_METHOD}
	)
endif ()

# SHIBE_OP_FMOD is fmodf. On most toolchains libm is folded into libc, so only
# link it where it is a library of its own.
include(CheckLibraryExists)
check_library_exists(m fmodf "" SHIBE_NEEDS_LIBM)
if (SHIBE_NEEDS_LIBM)
	target_link_libraries(shibe PUBLIC m)
endif ()
