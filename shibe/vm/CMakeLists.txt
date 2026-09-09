set(SOURCES
	"src/shibe.c"
)
add_library(shibe STATIC "${SOURCES}")
target_include_directories(shibe PUBLIC "include")
target_link_libraries(shibe PRIVATE blibs)

# SHIBE_OP_FMOD is fmodf. On most toolchains libm is folded into libc, so only
# link it where it is a library of its own.
include(CheckLibraryExists)
check_library_exists(m fmodf "" SHIBE_NEEDS_LIBM)
if (SHIBE_NEEDS_LIBM)
	target_link_libraries(shibe PUBLIC m)
endif ()
