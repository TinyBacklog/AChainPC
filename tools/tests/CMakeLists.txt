# Lifter tests. Each test_<name>.cpp is a generator: it prints a C program that
# exercises the translation, which is then compiled and run (see
# run_lifter_test.cmake). test_exclusive runs directly. test_cli.py needs a
# built game and a ROM, so it is not part of ctest.

find_package(OpenSSL COMPONENTS Crypto)
if (NOT OpenSSL_Crypto_FOUND)
    message(FATAL_ERROR "Lifter tests need libcrypto (openssl-devel). "
                        "Install it or configure with -DACRE_TESTS=OFF.")
endif()

# The game build compiles the lifted C with clang (see tools/acre/build.py), so
# the tests should too; GCC works but is not what ships.
if (NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(WARNING "Lifter tests compile generated C with ${CMAKE_C_COMPILER_ID}; "
                    "the game build uses clang. Configure with -DCMAKE_C_COMPILER=clang.")
endif()

set(_lifter_tests adc bitfields extract fp fpcontract simd crc32 recip aes sha1 sha256 satsimd chain patch fmlite xfast)
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
                     -DGEN=$<TARGET_FILE:lifter_gen_${t}> -DCC=${CMAKE_C_COMPILER} "-DCFLAGS=${ACRE_RECOMP_C_FLAGS}"
                     -DOUT_DIR=${_lifter_out} -DNAME=${t}
                     "-DLIBS=-I${OPENSSL_INCLUDE_DIR};${OPENSSL_CRYPTO_LIBRARY};-lm"
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/run_lifter_test.cmake)
    set_tests_properties(lifter_${t} PROPERTIES LABELS lifter TIMEOUT 600)
endforeach()

# 60fps hooks (game/patches/fps60.c) against the generated runtime header.
acre_lifter_executable(lifter_gen_fps60 test_fps60.cpp)
add_test(NAME lifter_fps60
         COMMAND ${CMAKE_COMMAND}
                 -DGEN=$<TARGET_FILE:lifter_gen_fps60> -DCC=${CMAKE_C_COMPILER} "-DCFLAGS=${ACRE_RECOMP_C_FLAGS}"
                 -DOUT_DIR=${_lifter_out} -DNAME=fps60_hooks
                 "-DLIBS=-I${PROJECT_SOURCE_DIR}/game/patches;-lm"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_lifter_test.cmake)
set_tests_properties(lifter_fps60 PROPERTIES LABELS lifter TIMEOUT 600)

acre_lifter_executable(lifter_exclusive test_exclusive.cpp)
add_test(NAME lifter_exclusive COMMAND lifter_exclusive)
set_tests_properties(lifter_exclusive PROPERTIES LABELS lifter)

# astralchain.ini parser (frontend/acre_ini.cpp); pure, links only simpleini.
add_executable(test_acre_ini test_acre_ini.cpp ${PROJECT_SOURCE_DIR}/frontend/acre_ini.cpp
               ${PROJECT_SOURCE_DIR}/frontend/overlay/chord.cpp)
target_include_directories(test_acre_ini PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_features(test_acre_ini PRIVATE cxx_std_20)
target_link_libraries(test_acre_ini PRIVATE SimpleIni::SimpleIni SDL3::SDL3)
target_compile_definitions(test_acre_ini PRIVATE ACRE_INI_TEMPLATE="${PROJECT_SOURCE_DIR}/frontend/astralchain.ini")
add_test(NAME frontend_acre_ini COMMAND test_acre_ini)
set_tests_properties(frontend_acre_ini PROPERTIES LABELS frontend)

# Dynarmic::SpinLock (exclusive monitor lock): mutual exclusion under contention.
add_executable(test_spinlock test_spinlock.cpp)
target_link_libraries(test_spinlock PRIVATE dynarmic)
add_test(NAME runtime_spinlock COMMAND test_spinlock)
set_tests_properties(runtime_spinlock PROPERTIES LABELS lifter)

# Fastmem arena (FM2): inline accesses (recomp_fastmem_access.h, C for clang,
# like the lifted code) and their runtime (recomp_fastmem.cpp), with dynarmic's
# fault handler installed below ours.
add_executable(test_fastmem test_fastmem.c test_fastmem_dynarmic.cpp
               ${PROJECT_SOURCE_DIR}/runtime/core/arm/recomp/recomp_fastmem.cpp)
target_include_directories(test_fastmem PRIVATE ${PROJECT_SOURCE_DIR}/runtime)
target_compile_definitions(test_fastmem PRIVATE RECOMP_FM_LIMIT_INIT=0)
target_compile_options(test_fastmem PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -march=x86-64-v3 -UNDEBUG>)
target_link_libraries(test_fastmem PRIVATE dynarmic Threads::Threads)
add_test(NAME runtime_fastmem COMMAND test_fastmem)
set_tests_properties(runtime_fastmem PROPERTIES LABELS lifter TIMEOUT 120)

# Per-core cache of rasterizer read areas (core/rasterizer_read_cache.h).
add_executable(test_read_area_cache test_read_area_cache.cpp)
target_include_directories(test_read_area_cache PRIVATE ${PROJECT_SOURCE_DIR}/runtime)
target_compile_features(test_read_area_cache PRIVATE cxx_std_20)
add_test(NAME runtime_read_area_cache COMMAND test_read_area_cache)
set_tests_properties(runtime_read_area_cache PROPERTIES LABELS lifter)

# Overlay UI (frontend/overlay): Dear ImGui frames converted to plain draw data.
add_executable(test_overlay_ui test_overlay_ui.cpp)
target_link_libraries(test_overlay_ui PRIVATE acre_overlay)
target_compile_features(test_overlay_ui PRIVATE cxx_std_20)
add_test(NAME frontend_overlay_ui COMMAND test_overlay_ui)
set_tests_properties(frontend_overlay_ui PROPERTIES LABELS frontend)

add_executable(test_frame_stats test_frame_stats.cpp)
target_link_libraries(test_frame_stats PRIVATE acre_overlay)
target_compile_features(test_frame_stats PRIVATE cxx_std_20)
add_test(NAME frontend_frame_stats COMMAND test_frame_stats)
set_tests_properties(frontend_frame_stats PROPERTIES LABELS frontend)

add_executable(test_chord test_chord.cpp)
target_link_libraries(test_chord PRIVATE acre_overlay)
target_compile_features(test_chord PRIVATE cxx_std_20)
add_test(NAME frontend_chord COMMAND test_chord)
set_tests_properties(frontend_chord PROPERTIES LABELS frontend)

# Saving astralchain.ini without losing comments (frontend/ini_rewrite.cpp).
add_executable(test_ini_rewrite test_ini_rewrite.cpp ${PROJECT_SOURCE_DIR}/frontend/ini_rewrite.cpp)
target_include_directories(test_ini_rewrite PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_features(test_ini_rewrite PRIVATE cxx_std_20)
add_test(NAME frontend_ini_rewrite COMMAND test_ini_rewrite)
set_tests_properties(frontend_ini_rewrite PROPERTIES LABELS frontend)

add_executable(test_thread_cpu test_thread_cpu.cpp)
target_link_libraries(test_thread_cpu PRIVATE acre_overlay Threads::Threads)
target_compile_features(test_thread_cpu PRIVATE cxx_std_20)
add_test(NAME frontend_thread_cpu COMMAND test_thread_cpu)
set_tests_properties(frontend_thread_cpu PROPERTIES LABELS frontend)

# Live recomp diagnostics and recent log lines for the overlay.
add_executable(test_recomp_snapshot test_recomp_snapshot.cpp)
target_link_libraries(test_recomp_snapshot PRIVATE core frontend_common)
target_include_directories(test_recomp_snapshot PRIVATE ${PROJECT_SOURCE_DIR}/runtime)
target_compile_features(test_recomp_snapshot PRIVATE cxx_std_20)
add_test(NAME runtime_recomp_snapshot COMMAND test_recomp_snapshot)
set_tests_properties(runtime_recomp_snapshot PROPERTIES LABELS lifter)

# Copying a VFS directory to disk for the game folder.
add_executable(test_game_folder test_game_folder.cpp ${PROJECT_SOURCE_DIR}/frontend/game_folder.cpp)
target_link_libraries(test_game_folder PRIVATE core)
target_include_directories(test_game_folder PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/runtime)
target_compile_features(test_game_folder PRIVATE cxx_std_20)
add_test(NAME frontend_game_folder COMMAND test_game_folder)
set_tests_properties(frontend_game_folder PROPERTIES LABELS frontend)

add_executable(test_overlay_input test_overlay_input.cpp)
target_link_libraries(test_overlay_input PRIVATE acre_overlay)
target_compile_features(test_overlay_input PRIVATE cxx_std_20)
add_test(NAME frontend_overlay_input COMMAND test_overlay_input)
set_tests_properties(frontend_overlay_input PROPERTIES LABELS frontend)

add_executable(test_live_settings test_live_settings.cpp)
target_link_libraries(test_live_settings PRIVATE acre_overlay)
target_compile_features(test_live_settings PRIVATE cxx_std_20)
add_test(NAME frontend_live_settings COMMAND test_live_settings)
set_tests_properties(frontend_live_settings PROPERTIES LABELS frontend)
