set(SOURCES
	"main.c"

	"shibe/basic.c"
	"shibe/str.c"
)

add_executable(doge-tests "${SOURCES}")
target_link_libraries(doge-tests blibs shibe shibe-aux)
set_target_properties(doge-tests PROPERTIES
    OUTPUT_NAME "tests"
)
