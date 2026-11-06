cmake_minimum_required(VERSION 3.18)
project(rustle VERSION 0.1.0 DESCRIPTION "Rustle: a soundboard and voice changer for Linux, Windows and macOS built with Fizmo, Grove and Walnut" LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RUSTLE_WITH_WALNUT "Build Walnut alongside Grove so Rustle's Walnut script works" ON)
option(RUSTLE_BUILD_APP "Build the Grove app (turn off to build only the rustle library and its tests)" ON)
option(RUSTLE_BUILD_TESTS "Build the Rustle tests" ON)
option(RUSTLE_PACKAGE "Make release packages: rustle with its window and compiled script built in, linked so it runs without a compiler" OFF)
option(RUSTLE_BUNDLE "Run rustle-host --bundle as part of the build to make rustle with its window built in (turn off when cross-compiling, then bundle on the target)" ON)
set(RUSTLE_FIZMO_TAG "v0.4.1" CACHE STRING "Fizmo git tag to fetch (needs the audio component)")
set(RUSTLE_GROVE_TAG "v0.3.1" CACHE STRING "Grove git tag to fetch")
set(RUSTLE_WALNUT_TAG "v0.3.0" CACHE STRING "Walnut git tag Grove fetches")

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

include(FetchContent)
set(RUSTLE_FETCH_EXTRA "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
    list(APPEND RUSTLE_FETCH_EXTRA SYSTEM)
endif()

FetchContent_Declare(fizmo
    GIT_REPOSITORY https://github.com/ThatNewCoderJSCpp/Fizmo.git
    GIT_TAG        ${RUSTLE_FIZMO_TAG}
    GIT_SHALLOW    TRUE
    ${RUSTLE_FETCH_EXTRA})

if(RUSTLE_BUILD_APP)
    set(GROVE_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
    set(GROVE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GROVE_WITH_WALNUT ${RUSTLE_WITH_WALNUT} CACHE BOOL "" FORCE)
    set(GROVE_WALNUT_TAG ${RUSTLE_WALNUT_TAG} CACHE STRING "" FORCE)
    set(GROVE_FIZMO_TAG ${RUSTLE_FIZMO_TAG} CACHE STRING "" FORCE)
    FetchContent_Declare(grove
        GIT_REPOSITORY https://github.com/ThatNewCoderJSCpp/Grove.git
        GIT_TAG        ${RUSTLE_GROVE_TAG}
        GIT_SHALLOW    TRUE
        ${RUSTLE_FETCH_EXTRA})
    FetchContent_MakeAvailable(fizmo grove)
else()
    FetchContent_MakeAvailable(fizmo)
endif()

if(TARGET fizmo::audio)
    set(RUSTLE_FIZMO_AUDIO fizmo::audio)
else()
    set(RUSTLE_FIZMO_AUDIO fizmo::fizmo)
endif()

function(rustle_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

if(RUSTLE_BUILD_TESTS)
    enable_testing()
endif()

add_library(rustle_core STATIC
    Source/rustle/json.cpp
    Source/rustle/settings.cpp
    Source/rustle/library.cpp
    Source/rustle/files.cpp
    Source/rustle/voice.cpp
    Source/rustle/clip.cpp
    Source/rustle/engine.cpp
    Source/rustle/virtual_mic.cpp
    Source/rustle/hotkeys.cpp
    Source/rustle/control.cpp
    Source/rustle/session.cpp)
add_library(rustle::core ALIAS rustle_core)
target_compile_features(rustle_core PUBLIC cxx_std_17)
target_include_directories(rustle_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Include")
target_link_libraries(rustle_core PUBLIC ${RUSTLE_FIZMO_AUDIO} ${CMAKE_DL_LIBS})
target_compile_definitions(rustle_core PUBLIC "RUSTLE_VERSION_STRING=\"${PROJECT_VERSION}\"")
find_package(Threads REQUIRED)
target_link_libraries(rustle_core PUBLIC Threads::Threads)
rustle_warnings(rustle_core)

if(RUSTLE_BUILD_TESTS)
    add_executable(rustle_tests Tests/rustle_tests.cpp)
    target_link_libraries(rustle_tests PRIVATE rustle_core)
    rustle_warnings(rustle_tests)
    if(MINGW)
        target_link_options(rustle_tests PRIVATE -static)
    endif()
    add_test(NAME rustle_tests COMMAND rustle_tests)
    set_tests_properties(rustle_tests PROPERTIES ENVIRONMENT "FIZMO_AUDIO_BACKEND=null")
endif()

if(NOT RUSTLE_BUILD_APP)
    return()
endif()

set(RUSTLE_BUNDLE_FLAGS "")
if(RUSTLE_PACKAGE)
    set(RUSTLE_BUNDLE_FLAGS --portable)
endif()

add_executable(rustle-host Tools/rustle.cpp)
target_link_libraries(rustle-host PRIVATE rustle_core grove::core grove::runtime)
target_compile_definitions(rustle-host PRIVATE
    "RUSTLE_SOURCE_UI_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/UI\"")
rustle_warnings(rustle-host)
if(WIN32)
    set_target_properties(rustle-host PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
endif()

add_custom_command(TARGET rustle-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/UI" "$<TARGET_FILE_DIR:rustle-host>/ui")

file(GLOB_RECURSE RUSTLE_UI_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/UI/*.grove" "${CMAKE_CURRENT_SOURCE_DIR}/UI/*.bark" "${CMAKE_CURRENT_SOURCE_DIR}/UI/*.wal")
set(RUSTLE_BUNDLED "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rustle${CMAKE_EXECUTABLE_SUFFIX}")
set(RUSTLE_BUNDLE_DEPENDS rustle-host)
if(TARGET walnut)
    list(APPEND RUSTLE_BUNDLE_DEPENDS walnut)
endif()
if(RUSTLE_BUNDLE)
    add_custom_command(OUTPUT "${RUSTLE_BUNDLED}"
        COMMAND ${CMAKE_COMMAND} -E env "RUSTLE_UI_DIR=${CMAKE_CURRENT_SOURCE_DIR}/UI" $<TARGET_FILE:rustle-host> --bundle "${RUSTLE_BUNDLED}" ${RUSTLE_BUNDLE_FLAGS}
        DEPENDS ${RUSTLE_BUNDLE_DEPENDS} ${RUSTLE_UI_FILES}
        COMMENT "Building rustle with its window and compiled script built in"
        VERBATIM)
    add_custom_target(rustle ALL DEPENDS "${RUSTLE_BUNDLED}")
endif()

if(RUSTLE_PACKAGE)
    if(MSVC)
        message(FATAL_ERROR "RUSTLE_PACKAGE needs a GCC or Clang toolchain, like Grove and Walnut")
    endif()

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _rustle_cpu)
    if(_rustle_cpu MATCHES "^(x86_64|amd64|x64)$")
        if(CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(_rustle_arch i686)
        else()
            set(_rustle_arch x86_64)
        endif()
    elseif(_rustle_cpu MATCHES "^(aarch64|arm64)$")
        set(_rustle_arch arm64)
    elseif(_rustle_cpu MATCHES "^(i[3-6]86|x86)$")
        set(_rustle_arch i686)
    else()
        set(_rustle_arch ${_rustle_cpu})
    endif()

    if(MINGW)
        target_link_options(rustle-host PRIVATE -static)
        include(CheckCXXSourceCompiles)
        check_cxx_source_compiles("#include <cstdlib>\n#ifndef _UCRT\n#error msvcrt\n#endif\nint main() { return 0; }" RUSTLE_MINGW_UCRT)
        if(_rustle_arch STREQUAL "arm64")
            set(_rustle_platform windows-clangarm64)
        elseif(_rustle_arch STREQUAL "i686")
            set(_rustle_platform windows-i686)
        elseif(RUSTLE_MINGW_UCRT)
            set(_rustle_platform windows-ucrt64)
        else()
            set(_rustle_platform windows-mingw64)
        endif()
        set(CPACK_GENERATOR ZIP)
    elseif(APPLE)
        set(_rustle_platform macos-${_rustle_arch})
        set(CPACK_GENERATOR TGZ)
        set(_rustle_mac_libs "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lib")
        foreach(t rustle-host)
            add_custom_command(TARGET ${t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} "-DFILES=$<TARGET_FILE:${t}>" "-DLIB_DIR=${_rustle_mac_libs}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos_bundle_libs.cmake"
                COMMENT "Copying the libraries ${t} needs into ${_rustle_mac_libs}"
                VERBATIM)
        endforeach()
        install(DIRECTORY "${_rustle_mac_libs}/" DESTINATION lib COMPONENT rustle USE_SOURCE_PERMISSIONS)
    else()
        target_link_options(rustle-host PRIVATE -static-libstdc++ -static-libgcc)
        set(_rustle_platform linux-${_rustle_arch})
        set(CPACK_GENERATOR TGZ)
    endif()
    set(RUSTLE_PACKAGE_PLATFORM ${_rustle_platform} CACHE STRING "Platform name used in the package file name")

    if(RUSTLE_BUNDLE)
        install(PROGRAMS "${RUSTLE_BUNDLED}" DESTINATION . COMPONENT rustle)
    endif()
    install(FILES README.md DESTINATION . COMPONENT rustle)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
        install(FILES LICENSE DESTINATION . COMPONENT rustle)
    endif()
    set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};Rustle;rustle;/")

    set(CPACK_PACKAGE_NAME "Rustle")
    set(CPACK_PACKAGE_VENDOR "ThatNewCoderJSCpp")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "Rustle-${PROJECT_VERSION}-${RUSTLE_PACKAGE_PLATFORM}")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Rustle")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RustlePackaging.cmake")
    include(CPack)
else()
    if(RUSTLE_BUNDLE)
        install(PROGRAMS "${RUSTLE_BUNDLED}" DESTINATION bin)
    endif()
endif()
