set(SOURCES
	"main.c"

	"shibe/asm.c"
	"shibe/basic.c"
	"shibe/exec.c"
	"shibe/str.c"
)

add_executable(doge-tests "${SOURCES}")
target_link_libraries(doge-tests blibs shibe shibe-aux)
set_target_properties(doge-tests PROPERTIES
    OUTPUT_NAME "tests"
)

if (EMSCRIPTEN)
	target_link_options(doge-tests PRIVATE
		# Without this the runtime never shuts down, so main's exit code never
		# reaches node and a failing suite still reports success
		"-sEXIT_RUNTIME=1"
		# The default 64KB will not hold the re-entrancy tests, which nest
		# shibe_execute a few hundred deep before the aux stack runs out
		"-sSTACK_SIZE=4194304"
	)
endif ()
