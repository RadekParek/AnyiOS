cmake_minimum_required(VERSION 3.22)
project(PodNativeRuntimeTests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DYNARMIC_ROOT)
    message(FATAL_ERROR "Supply the license-reviewed pinned DYNARMIC_ROOT; see scripts/prepare-cpu.sh")
endif()
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS A32 CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
# NDK's root-path-only include search must not hide these host-stored, target-neutral headers.
# This is deliberately narrower than disabling the NDK's cross-compilation search rules.
if(BOOST_ROOT)
    if(NOT EXISTS "${BOOST_ROOT}/boost/version.hpp")
        message(FATAL_ERROR "Prepared pinned Boost headers missing at BOOST_ROOT")
    endif()
    set(Boost_INCLUDE_DIR "${BOOST_ROOT}" CACHE PATH "Pinned Boost headers" FORCE)
endif()
add_subdirectory("${DYNARMIC_ROOT}" dynarmic)
if(ANDROID)
    # Only the validated single-step path is exposed. Do not re-enable RWX fallbacks.
    target_compile_definitions(dynarmic PRIVATE OAKNUT_USE_WX=1)
endif()
set(RUNTIME "${CMAKE_CURRENT_LIST_DIR}/../app/src/main/cpp")
add_library(podnative_cpu STATIC "${RUNTIME}/runtime/cpu.cpp" "${RUNTIME}/runtime/memory.cpp" "${RUNTIME}/runtime/cpu_probe.cpp")
target_include_directories(podnative_cpu PUBLIC "${RUNTIME}")
target_link_libraries(podnative_cpu PRIVATE dynarmic)
target_compile_options(podnative_cpu PRIVATE -Wall -Wextra -Werror)
add_library(podnative_hle STATIC
    "${RUNTIME}/runtime/exports.cpp"
    "${RUNTIME}/runtime/dyld.cpp"
    # Stop reports name the guest function through LoadedImage::nearestSymbol, so the
    # loader (and the header inspection it validates with) belongs to the HLE library
    # the CPU tests link.
    "${RUNTIME}/runtime/loader.cpp"
    "${RUNTIME}/macho.cpp"
    "${RUNTIME}/runtime/services.cpp"
    "${RUNTIME}/runtime/libc.cpp"
    "${RUNTIME}/runtime/cxx.cpp"
    "${RUNTIME}/runtime/fs.cpp"
    "${RUNTIME}/runtime/stdio.cpp"
    "${RUNTIME}/runtime/audio.cpp"
    "${RUNTIME}/runtime/posix.cpp"
    "${RUNTIME}/runtime/openal.cpp"
    "${RUNTIME}/runtime/format.cpp"
    "${RUNTIME}/runtime/heap.cpp"
    "${RUNTIME}/runtime/execution.cpp"
    "${RUNTIME}/runtime/objc.cpp"
    "${RUNTIME}/runtime/uikit.cpp"
    # EAGL routes into the GL bridge, so the HLE library the CPU tests link needs it too.
    "${RUNTIME}/runtime/gles.cpp"
    "${RUNTIME}/runtime/frameworks.cpp"
    "${RUNTIME}/runtime/coregraphics.cpp")
target_include_directories(podnative_hle PUBLIC "${RUNTIME}")
target_link_libraries(podnative_hle PUBLIC podnative_cpu)
target_compile_options(podnative_hle PRIVATE -Wall -Wextra -Werror)
add_executable(cpu_test cpu_test.cpp)
target_link_libraries(cpu_test PRIVATE podnative_cpu podnative_hle)
target_compile_options(cpu_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
enable_testing()
add_test(NAME cpu_correctness COMMAND cpu_test)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
    add_executable(oaknut_wx_test oaknut_wx_test.cpp)
    target_include_directories(oaknut_wx_test PRIVATE "${DYNARMIC_ROOT}/externals/oaknut/include")
    target_compile_definitions(oaknut_wx_test PRIVATE OAKNUT_USE_WX=1)
    target_compile_options(oaknut_wx_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME allocator_wx_permissions COMMAND oaknut_wx_test)
endif()
add_executable(uikit_test uikit_test.cpp)
target_link_libraries(uikit_test PRIVATE podnative_hle)
target_compile_options(uikit_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME uikit_bootstrap COMMAND uikit_test)

add_executable(audio_test audio_test.cpp)
target_link_libraries(audio_test PRIVATE podnative_hle)
target_compile_options(audio_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME audio_session COMMAND audio_test)

add_executable(cxx_test cxx_test.cpp)
target_link_libraries(cxx_test PRIVATE podnative_hle)
target_compile_options(cxx_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME cxx_runtime COMMAND cxx_test)

# Host runner for a real .app bundle: the same entry point the Android app calls, so a
# launch report (and the API the guest stopped at) can be reproduced without a device.
# launch.cpp is compiled again here with PODNATIVE_CPU_IN_APP so the runner gets the real
# guest path; the archive's copy stays unbound and is not pulled in.
add_executable(guest_runner "${CMAKE_CURRENT_LIST_DIR}/../tools/guest_runner.cpp"
    "${RUNTIME}/runtime/launch.cpp")
target_link_libraries(guest_runner PRIVATE podnative_hle podnative_cpu)
target_compile_definitions(guest_runner PRIVATE PODNATIVE_CPU_IN_APP=1)
target_compile_options(guest_runner PRIVATE -Wall -Wextra -Werror)
