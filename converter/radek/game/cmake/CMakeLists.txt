cmake_minimum_required(VERSION 3.22)
project(AnyiOSTranslatedGame LANGUAGES C)

# Host-driven NDK build of the existing portable-C translation and runner.
# This is not an iOS framework converter or proof of game compatibility.
if(NOT ANDROID)
    message(FATAL_ERROR "Use the Android NDK CMake toolchain and select an Android ABI")
endif()
foreach(required ANYIOS_TRANSLATION_DIR ANYIOS_RUNTIME_DIR ANYIOS_OUTPUT_DIR)
    if(NOT IS_ABSOLUTE "${${required}}")
        message(FATAL_ERROR "${required} must be an absolute path")
    endif()
endforeach()
foreach(input game_all.c rt_gen.c rt_gen.h)
    if(NOT EXISTS "${ANYIOS_TRANSLATION_DIR}/${input}")
        message(FATAL_ERROR "Missing translation input: ${input}")
    endif()
endforeach()

add_library(translated_game SHARED
    "${ANYIOS_TRANSLATION_DIR}/game_all.c"
    "${ANYIOS_TRANSLATION_DIR}/rt_gen.c"
    "${ANYIOS_RUNTIME_DIR}/rt_core.c"
    "${ANYIOS_RUNTIME_DIR}/rt_fs.c"
    "${ANYIOS_RUNTIME_DIR}/rt_entry.c"
    "${ANYIOS_RUNTIME_DIR}/rt_android_jni.c"
)
target_include_directories(translated_game PRIVATE "${ANYIOS_TRANSLATION_DIR}" "${ANYIOS_RUNTIME_DIR}")
target_compile_features(translated_game PRIVATE c_std_11)
target_compile_options(translated_game PRIVATE -fno-strict-aliasing -fvisibility=default)
target_link_options(translated_game PRIVATE
    -Wl,--no-undefined
    -Wl,--build-id=none
    -Wl,-z,max-page-size=16384
)
target_link_libraries(translated_game PRIVATE m)
set_target_properties(translated_game PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${ANYIOS_OUTPUT_DIR}"
    OUTPUT_NAME translated_game
)
