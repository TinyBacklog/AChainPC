# Lifter tests. Each test_<name>.cpp is a generator: it prints a C program that
# exercises the translation, which is then compiled and run (see
# run_lifter_test.cmake). test_exclusive runs directly. test_cli.py needs a
# built game and a ROM, so it is not part of ctest.

find_package(OpenSSL COMPONENTS Crypto)
if (NOT OpenSSL_Crypto_FOUND)
    message(FATAL_ERROR "Lifter tests need libcrypto (openssl-devel). "
                        "Install it or configure with -DACRE_TESTS=OFF.")
endif()

set(_lifter_tests adc bitfields extract fp simd crc32 recip aes sha1 sha256 satsimd chain patch)
set(_lifter_out "${CMAKE_CURRENT_BINARY_DIR}/out")
file(MAKE_DIRECTORY "${_lifter_out}")

# The generators check their own invariants with assert(), so NDEBUG from a
# Release build must not reach them. -UNDEBUG is GCC/clang syntax; fine while
# the project targets Linux only.
function(acre_lifter_executable target source)
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/runtime)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -UNDEBUG)
endfunction()

foreach(t IN LISTS _lifter_tests)
    acre_lifter_executable(lifter_gen_${t} test_${t}.cpp)
    add_test(NAME lifter_${t}
             COMMAND ${CMAKE_COMMAND}
                     -DGEN=$<TARGET_FILE:lifter_gen_${t}> -DCC=${CMAKE_C_COMPILER}
                     -DOUT_DIR=${_lifter_out} -DNAME=${t}
                     "-DLIBS=-I${OPENSSL_INCLUDE_DIR};${OPENSSL_CRYPTO_LIBRARY};-lm"
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/run_lifter_test.cmake)
    set_tests_properties(lifter_${t} PROPERTIES LABELS lifter TIMEOUT 600)
endforeach()

# 60fps hooks (game/patches/fps60.c) against the generated runtime header.
acre_lifter_executable(lifter_gen_fps60 test_fps60.cpp)
add_test(NAME lifter_fps60
         COMMAND ${CMAKE_COMMAND}
                 -DGEN=$<TARGET_FILE:lifter_gen_fps60> -DCC=${CMAKE_C_COMPILER}
                 -DOUT_DIR=${_lifter_out} -DNAME=fps60_hooks
                 "-DLIBS=-I${PROJECT_SOURCE_DIR}/game/patches;-lm"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_lifter_test.cmake)
set_tests_properties(lifter_fps60 PROPERTIES LABELS lifter TIMEOUT 600)

acre_lifter_executable(lifter_exclusive test_exclusive.cpp)
add_test(NAME lifter_exclusive COMMAND lifter_exclusive)
set_tests_properties(lifter_exclusive PROPERTIES LABELS lifter)

# astralchain.ini parser (frontend/acre_ini.cpp); pure, links only simpleini.
add_executable(test_acre_ini test_acre_ini.cpp ${PROJECT_SOURCE_DIR}/frontend/acre_ini.cpp)
target_include_directories(test_acre_ini PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_features(test_acre_ini PRIVATE cxx_std_20)
target_link_libraries(test_acre_ini PRIVATE SimpleIni::SimpleIni)
target_compile_definitions(test_acre_ini PRIVATE ACRE_INI_TEMPLATE="${PROJECT_SOURCE_DIR}/frontend/astralchain.ini")
add_test(NAME frontend_acre_ini COMMAND test_acre_ini)
set_tests_properties(frontend_acre_ini PROPERTIES LABELS frontend)
