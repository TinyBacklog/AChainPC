# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Credits to Samantas5855 and others for this function.
function(create_resource file output filename)
    # Read hex data from file
    file(READ ${file} filedata HEX)
    # Convert hex data for C compatibility
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," filedata ${filedata})
    # Write data to output file
    set(RESOURCES_DIR "${PROJECT_BINARY_DIR}/dist" PARENT_SCOPE)
    file(WRITE "${PROJECT_BINARY_DIR}/dist/${output}" "const unsigned char ${filename}[] = {${filedata}};\nconst unsigned ${filename}_size = sizeof(${filename});\n")
endfunction()

if (NOT ASTRALCHAIN_RECOMP_DIR AND SUYU_CMD_RECOMP_DIR)
    message(WARNING "SUYU_CMD_RECOMP_DIR is deprecated, use ASTRALCHAIN_RECOMP_DIR")
    set(ASTRALCHAIN_RECOMP_DIR "${SUYU_CMD_RECOMP_DIR}")
endif()

if (SUYU_CMD OR YUZU_CMD)
    # In-game overlay: Dear ImGui layout and its conversion to video_core draw data.
    add_library(acre_overlay STATIC
        overlay/chord.cpp
        overlay/chord.h
        overlay/frame_stats.cpp
        overlay/frame_stats.h
        overlay/overlay_ui.cpp
        overlay/overlay_ui.h
    )
    target_include_directories(acre_overlay PUBLIC ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/runtime)
    target_link_libraries(acre_overlay PUBLIC common imgui SDL3::SDL3)

    set(SUYU_CMD_SOURCES
        emu_window/emu_window_sdl2.cpp
        emu_window/emu_window_sdl2.h
        emu_window/emu_window_headless.cpp
        emu_window/emu_window_headless.h
        emu_window/emu_window_sdl2_gl.cpp
        emu_window/emu_window_sdl2_gl.h
        emu_window/emu_window_sdl2_null.cpp
        emu_window/emu_window_sdl2_null.h
        emu_window/emu_window_sdl2_vk.cpp
        emu_window/emu_window_sdl2_vk.h
        precompiled_headers.h
        acre_ini.cpp
        acre_ini.h
        acre_ini_apply.cpp
        acre_ini_apply.h
        acre_tas.cpp
        acre_tas.h
        sdl_config.cpp
        sdl_config.h
        suyu.cpp
        suyu.rc
    )

    add_executable(astralchain ${SUYU_CMD_SOURCES})
    set(SUYU_CMD_TARGETS astralchain)

    # ---------------------------------------------------------------------
    # Per-game statically recompiled variant.
    #
    # ASTRALCHAIN_RECOMP_DIR points at one export's exefs/ directory, which holds
    # a generated recomp_registration.c plus one subdirectory per NSO module
    # with a generated CMakeLists.txt. Each of those contributes a static
    # library (recomp_static_<module>) whose C symbols have been renamed per
    # module, so they can all live in one binary. The result is a single
    # self-contained executable: no recompiled_*.dll to ship beside it.
    #
    # This is an *additional* target - plain astralchain stays game-agnostic.
    # ---------------------------------------------------------------------
    if (ASTRALCHAIN_RECOMP_DIR AND EXISTS "${ASTRALCHAIN_RECOMP_DIR}/recomp_registration.c")
        enable_language(C)
        # Tells the generated module projects to emit only the static library,
        # skipping their portable standalone exe (whose target name would
        # collide between modules) and their loadable shared image.
        set(RECOMP_STATIC_ONLY ON)
        set(_recomp_libs "")
        # Must match Core::RecompAbiVersion; the generated header asserts it.
        set(_recomp_abi 7)
        set(_recomp_identities "#pragma once\n#include \"core/arm/recomp/recomp_identity.h\"\ninline constexpr Core::RecompModuleIdentity kRecompIdentities[] = {\n")
        # file(GLOB) treats [ ] * ? in the WHOLE path as glob syntax, not just
        # the trailing "/*" - a title whose exported folder name carries its
        # title ID in brackets (e.g. "Pokemon Sword[0100ABF008968000][US][v0]",
        # produced whenever a ROM has no NACP title to name the folder from)
        # silently matches nothing, so every module looks missing even though
        # recomp_export.c/CMakeLists.txt are sitting right there on disk.
        # Escaping each special character to its bracket-literal form before
        # globbing keeps this working for any export path, not just ones that
        # happen to avoid those characters.
        # Order matters: each replacement's own output must not be re-mangled
        # by a later replacement, so "[" goes through a placeholder that
        # contains no bracket characters until every other substitution has
        # already run.
        set(_recomp_dir_escaped "${ASTRALCHAIN_RECOMP_DIR}")
        string(REPLACE "[" "@LBRACKET@" _recomp_dir_escaped "${_recomp_dir_escaped}")
        string(REPLACE "]" "[]]" _recomp_dir_escaped "${_recomp_dir_escaped}")
        string(REPLACE "*" "[*]" _recomp_dir_escaped "${_recomp_dir_escaped}")
        string(REPLACE "?" "[?]" _recomp_dir_escaped "${_recomp_dir_escaped}")
        string(REPLACE "@LBRACKET@" "[[]" _recomp_dir_escaped "${_recomp_dir_escaped}")
        file(GLOB _recomp_children RELATIVE "${ASTRALCHAIN_RECOMP_DIR}"
             "${_recomp_dir_escaped}/*")
        foreach(_m ${_recomp_children})
            if (EXISTS "${ASTRALCHAIN_RECOMP_DIR}/${_m}/CMakeLists.txt" AND
                EXISTS "${ASTRALCHAIN_RECOMP_DIR}/${_m}/recomp_export.c")
                # Each generated module carries its own frozen copy of the
                # GuestContext layout. A cache produced before TPIDRRO_EL0 was
                # split out of TPIDR_EL0 still folds the two registers into one
                # field, which silently breaks IPC (the guest emits its message
                # header outside the TLS region the kernel parses it from).
                # Refuse to build against such a cache instead of shipping a
                # binary that black-screens.
                if (EXISTS "${ASTRALCHAIN_RECOMP_DIR}/${_m}/recomp_runtime.h")
                    file(READ "${ASTRALCHAIN_RECOMP_DIR}/${_m}/recomp_runtime.h" _rt_h)
                    if (NOT _rt_h MATCHES "tpidrro_el0")
                        message(FATAL_ERROR
                            "Recompiled module '${_m}' in ${ASTRALCHAIN_RECOMP_DIR} predates the "
                            "TPIDRRO_EL0/TPIDR_EL0 split and would mis-handle IPC. "
                            "Re-export the game to regenerate its AOT cache.")
                    endif()
                endif()
                set(_identity_path "${ASTRALCHAIN_RECOMP_DIR}/${_m}/recomp_identity.json")
                if (NOT EXISTS "${_identity_path}")
                    message(FATAL_ERROR "Module ${_m} lacks identity metadata. Re-export with the current recompiler.")
                endif()
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_identity_path}")
                file(READ "${_identity_path}" _identity)
                string(JSON _abi GET "${_identity}" abi_version)
                string(JSON _identity_name GET "${_identity}" module)
                if (NOT _abi EQUAL _recomp_abi OR NOT _identity_name STREQUAL _m)
                    message(FATAL_ERROR "Incompatible AOT identity for ${_m}")
                endif()
                string(APPEND _recomp_identities "    {\"${_m}\", {{")
                set(_segment_index 0)
                foreach(_segment text rodata data)
                    string(JSON _hash GET "${_identity}" sha256 ${_segment_index})
                    set(_segment_path "${ASTRALCHAIN_RECOMP_DIR}/${_m}/data/${_segment}.bin")
                    if (EXISTS "${_segment_path}")
                        file(SHA256 "${_segment_path}" _actual_hash)
                        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_segment_path}")
                    else()
                        set(_actual_hash "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
                    endif()
                    if (NOT _hash STREQUAL _actual_hash)
                        message(FATAL_ERROR "AOT segment modified after export: ${_segment_path}")
                    endif()
                    string(APPEND _recomp_identities "\"${_hash}\",")
                    math(EXPR _segment_index "${_segment_index} + 1")
                endforeach()
                string(APPEND _recomp_identities "}}},\n")
                add_subdirectory("${ASTRALCHAIN_RECOMP_DIR}/${_m}"
                                 "${CMAKE_CURRENT_BINARY_DIR}/recomp/${_m}")
                target_compile_options(recomp_static_${_m} PRIVATE ${ACRE_RECOMP_C_FLAGS})
                if (ACRE_FASTMEM)
                    target_compile_definitions(recomp_static_${_m} PRIVATE RECOMP_FASTMEM=1)
                    target_include_directories(recomp_static_${_m} PRIVATE ${PROJECT_SOURCE_DIR}/runtime)
                endif()
                list(APPEND _recomp_libs "recomp_static_${_m}")
            endif()
        endforeach()
        if (_recomp_libs)
            string(APPEND _recomp_identities "};\nstatic_assert(Core::RecompAbiVersion == ${_recomp_abi});\n")
            file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/recomp_identities.h" CONTENT "${_recomp_identities}")
            add_executable(astralchain-static ${SUYU_CMD_SOURCES}
                           "${ASTRALCHAIN_RECOMP_DIR}/recomp_registration.c")
            target_compile_definitions(astralchain-static PRIVATE SUYU_CMD_STATIC_RECOMP=1)
            if (ACRE_RECOMP_ARCH MATCHES "^x86-64-v[234]$")
                target_compile_definitions(astralchain-static PRIVATE ACRE_RECOMP_CPU_LEVEL="${ACRE_RECOMP_ARCH}")
            endif()
            target_include_directories(astralchain-static PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
            target_link_libraries(astralchain-static PRIVATE ${_recomp_libs})
            # Game patch hooks (C) called from the lifted main module; they need
            # that module's generated runtime header.
            if (TARGET recomp_static_main)
                add_library(acre_game_hooks STATIC ${PROJECT_SOURCE_DIR}/game/patches/fps60.c)
                target_link_libraries(acre_game_hooks PRIVATE recomp_static_main m)
                target_compile_options(acre_game_hooks PRIVATE ${ACRE_RECOMP_C_FLAGS})
                target_link_libraries(astralchain-static PRIVATE acre_game_hooks)
            endif()
            list(APPEND SUYU_CMD_TARGETS astralchain-static)
        else()
            message(WARNING "ASTRALCHAIN_RECOMP_DIR set but no recompiled modules found in it")
        endif()
    endif()

    create_resource("${PROJECT_SOURCE_DIR}/dist/suyu.bmp" "suyu_cmd/suyu_icon.h" "suyu_icon")

    foreach(_t ${SUYU_CMD_TARGETS})
        target_include_directories(${_t} PRIVATE ${PROJECT_SOURCE_DIR})
        if (APPLE)
            target_sources(${_t} PRIVATE
                emu_window/emu_window_sdl2_mtl.cpp
                emu_window/emu_window_sdl2_mtl.h
            )
        endif()

        target_link_libraries(${_t} PRIVATE common core input_common frontend_common acre_overlay)
        target_link_libraries(${_t} PRIVATE glad)
        if (MSVC)
            target_link_libraries(${_t} PRIVATE getopt)
        endif()
        target_link_libraries(${_t} PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)

        target_include_directories(${_t} PRIVATE ${RESOURCES_DIR})

        target_link_libraries(${_t} PRIVATE SDL3::SDL3 Vulkan::Headers GPUOpen::VulkanMemoryAllocator)
        # FFmpeg headers are not transitively exposed by core; add them explicitly
        target_include_directories(${_t} PRIVATE ${FFmpeg_INCLUDE_DIR})

        if(UNIX)
            if (APPLE)
                target_link_libraries(${_t} PRIVATE metal-cpp)  # Metal support for macOS
            endif()
        endif()

        if(WIN32)
            # compile as a win32 gui application instead of a console application
            if(MSVC)
                set_target_properties(${_t} PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
            elseif(MINGW)
                set_target_properties(${_t} PROPERTIES LINK_FLAGS_RELEASE "-Wl,--subsystem,windows")
            endif()
            target_link_libraries(${_t} PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)  # Fixed typo: yuzu-cmd -> astralchain
        endif()

        if (MSVC AND SUYU_USE_BUNDLED_SDL3)
            include(CopySuyuSDLDeps)
            copy_suyu_SDL_deps(${_t})
        endif()

        if (SUYU_USE_PRECOMPILED_HEADERS)
            target_precompile_headers(${_t} PRIVATE precompiled_headers.h)
        endif()

        create_target_directory_groups(${_t})
    endforeach()

    if(UNIX AND NOT APPLE)
        install(TARGETS astralchain)
    endif()
endif()