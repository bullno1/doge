set(SOURCES
	"blibs.c"
	"fib.c"
)

add_executable(doge-bench-fib "${SOURCES}")
target_link_libraries(doge-bench-fib blibs shibe shibe-aux)
set_target_properties(doge-bench-fib PROPERTIES
    OUTPUT_NAME "fib"
)
