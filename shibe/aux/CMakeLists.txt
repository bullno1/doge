set(SOURCES
	"src/alloc.c"
)
add_library(shibe-aux STATIC "${SOURCES}")
target_include_directories(shibe-aux PUBLIC "include")
target_link_libraries(shibe-aux PUBLIC blibs shibe)
