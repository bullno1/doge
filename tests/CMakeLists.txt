set(SOURCES
	"main.c"

	"shibe/basic.c"
)

add_executable(doge-tests "${SOURCES}")
target_link_libraries(doge-tests shibe blibs)
set_target_properties(doge-tests PROPERTIES
    OUTPUT_NAME "tests"
)
