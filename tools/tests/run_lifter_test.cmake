# Runs one lifter test: generator -> C source -> native binary -> exit code.
# Inputs: GEN (generator executable), CC, CFLAGS (;-list, the game build's flags
# for lifted code), OUT_DIR, NAME, LIBS (;-list of extra compiler arguments:
# include dirs and libraries)
execute_process(COMMAND "${GEN}" OUTPUT_FILE "${OUT_DIR}/${NAME}.c"
                RESULT_VARIABLE r ERROR_VARIABLE e)
if (NOT r EQUAL 0)
    message(FATAL_ERROR "${NAME}: generator failed (${r}): ${e}")
endif()
# gnu11, not c11: the game build's CMAKE_C_STANDARD 11 keeps extensions on, and
# in GNU mode floating-point contraction defaults to "fast" where ISO mode turns
# it off. Testing in ISO mode would hide a missing -ffp-contract=off.
execute_process(COMMAND "${CC}" -std=gnu11 -O2 -fsanitize=undefined -fsanitize-trap=all ${CFLAGS}
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
