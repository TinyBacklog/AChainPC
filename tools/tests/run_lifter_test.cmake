# Runs one lifter test: generator -> C source -> native binary -> exit code.
# Inputs: GEN (generator executable), CC, OUT_DIR, NAME, LIBS (;-list)
execute_process(COMMAND "${GEN}" OUTPUT_FILE "${OUT_DIR}/${NAME}.c"
                RESULT_VARIABLE r ERROR_VARIABLE e)
if (NOT r EQUAL 0)
    message(FATAL_ERROR "${NAME}: generator failed (${r}): ${e}")
endif()
execute_process(COMMAND "${CC}" -std=c11 -O2 -fsanitize=undefined -fsanitize-trap=all
                        "${OUT_DIR}/${NAME}.c" -o "${OUT_DIR}/${NAME}.bin" ${LIBS}
                RESULT_VARIABLE r ERROR_VARIABLE e)
if (NOT r EQUAL 0)
    message(FATAL_ERROR "${NAME}: generated C failed to compile: ${e}")
endif()
execute_process(COMMAND "${OUT_DIR}/${NAME}.bin" RESULT_VARIABLE r
                OUTPUT_VARIABLE o ERROR_VARIABLE e TIMEOUT 300)
message(STATUS "${NAME}: ${o}")
if (NOT r EQUAL 0)
    message(FATAL_ERROR "${NAME}: test failed (${r}): ${o}${e}")
endif()
