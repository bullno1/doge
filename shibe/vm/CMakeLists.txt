set(SOURCES
	"src/shibe.c"
)
add_library(shibe STATIC "${SOURCES}")
target_include_directories(shibe PUBLIC "include")
target_link_libraries(shibe PRIVATE blibs)
