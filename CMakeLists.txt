set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
cmake_minimum_required(VERSION 3.22)

project(GLFrontier C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

##########################################################################################
# Options
##########################################################################################

option(USE_SDL2 "Use SDL2" ON)
option(USE_PHYSFS "Use PhysFS" ON)
option(USE_GLAD "Load OpenGL with glad (desktop only)" ON)
option(BUILD_SDL_SHARED_LIB "Build SDL as a shared library" OFF)
# ON: the game code with this fork's mods (fe2/fe2_modded.s). OFF: the
# unmodified game (fe2/fe2.s); host code for the mods is compiled out.
# Use a separate build directory for each, e.g. build / build-original.
option(GLF_MODDED_FE2 "Build the modded game code (OFF: the original game)" ON)

if(NOT DEFINED PLATFORM)
    set(PLATFORM "default") # "Web" for emscripten builds
endif()

# Android and the web use GLES3 / WebGL2 directly
if(ANDROID OR PLATFORM STREQUAL "Web")
    set(USE_GLAD OFF CACHE BOOL "" FORCE)
endif()

# MSVC: link the runtime statically
if(MSVC)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebug")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
endif()

# Libraries linked into the game, filled in below
set(LIBRARIES "")

include(FetchContent)
function(add_git_dependency name url tag)
    FetchContent_Declare(${name}
        GIT_REPOSITORY ${url}
        GIT_TAG ${tag}
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(${name})
endfunction()

##########################################################################################
# SDL2
##########################################################################################

if(USE_SDL2 AND NOT PLATFORM STREQUAL "Web")
    set(BUILD_SHARED_LIBS ${BUILD_SDL_SHARED_LIB})
    if(BUILD_SDL_SHARED_LIB)
        option(SDL_SHARED "Build SDL as a shared library" ON)
    endif()

    foreach(sub AUDIO VIDEO TIMERS THREADS RENDER EVENTS CPUINFO ATOMIC)
        set(SDL_${sub} ON CACHE BOOL "" FORCE)
    endforeach()
    foreach(sub FILE FILESYSTEM MISC TEST EXAMPLES DEMOS)
        set(SDL_${sub} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(SDL2MAIN OFF CACHE BOOL "" FORCE)

    if(ANDROID)
        foreach(sub JOYSTICK HAPTIC SENSOR POWER)
            set(SDL_${sub} ON CACHE BOOL "" FORCE)
        endforeach()
        # SDL's Java side (HIDDeviceManager) always calls into hidapi's
        # native code at startup: without it the app dies on launch
        set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
        set(SDL_HIDAPI_JOYSTICK OFF CACHE BOOL "" FORCE)
    else()
        # joystick + HIDAPI: game controllers (gamepad.c)
        foreach(sub JOYSTICK HIDAPI)
            set(SDL_${sub} ON CACHE BOOL "" FORCE)
        endforeach()
        foreach(sub HAPTIC SENSOR POWER)
            set(SDL_${sub} OFF CACHE BOOL "" FORCE)
        endforeach()
    endif()

    add_git_dependency(SDL2 https://github.com/libsdl-org/SDL.git SDL2)

    if(ANDROID)
        set(SDL_TARGET SDL2-static)
    elseif(BUILD_SDL_SHARED_LIB)
        set(SDL_TARGET SDL2)
        list(APPEND LIBRARIES SDL2main)
    else()
        set(SDL_TARGET SDL2-static)
        list(APPEND LIBRARIES SDL2main)
    endif()
    list(APPEND LIBRARIES ${SDL_TARGET})
endif()

if(USE_SDL2 AND PLATFORM STREQUAL "Web")
    # emscripten provides SDL2 (-s USE_SDL=2); only the headers are needed here
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG SDL2
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(SDL2)
    if(NOT sdl2_POPULATED)
        FetchContent_Populate(SDL2)
    endif()
    include_directories(${sdl2_SOURCE_DIR}/include)
endif()

##########################################################################################
# PhysFS (save games)
##########################################################################################

if(USE_PHYSFS)
    set(PHYSFS_STATIC_BUILD ON CACHE BOOL "")
    set(PHYSFS_BUILD_SHARED OFF CACHE BOOL "")
    set(PHYSFS_BUILD_TEST OFF CACHE BOOL "")
    set(PHYSFS_BUILD_DOCS OFF CACHE BOOL "")
    add_git_dependency(physfs https://github.com/icculus/physfs.git main)
    list(APPEND LIBRARIES PhysFS::PhysFS-static)
endif()

##########################################################################################
# OpenGL
##########################################################################################

if(USE_GLAD)
    # glad 1.x: master is glad 2, which has no glad/glad.h or gladLoadGLLoader
    set(GLAD_PROFILE "core" CACHE STRING "OpenGL profile")
    set(GLAD_API "gl=" CACHE STRING "API type/version pairs, no version means latest")
    set(GLAD_GENERATOR "c" CACHE STRING "Language to generate the binding for")
    find_package(OpenGL REQUIRED)
    add_git_dependency(glad https://github.com/Dav1dde/glad.git v0.1.36)
    list(APPEND LIBRARIES glad)
endif()

if(ANDROID)
    find_library(GLES3_LIB GLESv3)
    find_library(EGL_LIB EGL)
    find_library(ANDROID_LIB android)
    find_library(LOG_LIB log)
    list(APPEND LIBRARIES ${GLES3_LIB} ${EGL_LIB} ${ANDROID_LIB} ${LOG_LIB})
endif()

##########################################################################################
# OpenXR (VR headsets, src/vr.cpp): the loader is built in, linked statically
##########################################################################################

if(ANDROID OR PLATFORM STREQUAL "Web")
    set(GLF_VR_DEFAULT OFF)
else()
    set(GLF_VR_DEFAULT ON)
endif()
option(GLF_VR "VR headsets through OpenXR" ${GLF_VR_DEFAULT})

if(GLF_VR)
    set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE) # (its own, built in)
    FetchContent_Declare(OpenXR
        URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.60.tar.gz)
    FetchContent_MakeAvailable(OpenXR)
    if(MINGW)
        # MinGW's headers trip the loader's -Werror=undef
        target_compile_options(openxr_loader PRIVATE -Wno-undef -Wno-error=undef)
    endif()
    list(APPEND LIBRARIES openxr_loader)
    add_compile_definitions(GLF_VR)
    if(WIN32)
        list(APPEND LIBRARIES d3d11 dxgi opengl32)
    else()
        find_package(X11 REQUIRED)
        list(APPEND LIBRARIES ${X11_LIBRARIES} OpenGL::GLX ${CMAKE_DL_LIBS})
    endif()
endif()

##########################################################################################
# Dear ImGui (emulator menu)
##########################################################################################

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
)
target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
if(ANDROID OR PLATFORM STREQUAL "Web")
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
endif()
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
if(SDL_TARGET)
    target_link_libraries(imgui PUBLIC ${SDL_TARGET})
endif()
list(APPEND LIBRARIES imgui)

##########################################################################################
# glutess (polygon tessellation for the GL renderer)
##########################################################################################

# (priorityq-heap.c is #included by priorityq.c, so no glob here)
set(GLUTESS_DIR ${CMAKE_CURRENT_LIST_DIR}/vendor/glutess)
add_library(glutess OBJECT
    ${GLUTESS_DIR}/dict.c ${GLUTESS_DIR}/geom.c ${GLUTESS_DIR}/memalloc.c
    ${GLUTESS_DIR}/mesh.c ${GLUTESS_DIR}/normal.c ${GLUTESS_DIR}/priorityq.c
    ${GLUTESS_DIR}/render.c ${GLUTESS_DIR}/sweep.c ${GLUTESS_DIR}/tess.c
    ${GLUTESS_DIR}/tessellate.c ${GLUTESS_DIR}/tessmono.c)
list(APPEND LIBRARIES glutess)

##########################################################################################
# The emulated game (68k code translated to C)
##########################################################################################

add_compile_definitions(OGG_MUSIC LITTLE_ENDIAN)

# Both are generated by tools/build_fe2.py (--variant modded / original)
if(GLF_MODDED_FE2)
    set(FE2_SOURCE fe2/fe2_modded.s.c)
    add_compile_definitions(FE2_USE_MODDED=1)
else()
    set(FE2_SOURCE fe2/fe2.s.c)
    add_compile_definitions(FE2_USE_MODDED=0)
endif()
message(STATUS "Game code: ${FE2_SOURCE}")

include_directories(
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/fe2
    ${CMAKE_CURRENT_LIST_DIR}/fe2/audio
    ${CMAKE_CURRENT_LIST_DIR}/vendor/glutess
    ${CMAKE_CURRENT_LIST_DIR}/vendor/minivorbis
)

# The translated 68k code works out the overflow flag V from signed adds
# that overflow: -fwrapv makes that defined. Without it clang (-O2) decides
# V is always 0 and loops like "add.l d0,d0 / bvc" never end.
#
# FE2_FLAT: Start680x0_at, the whole game in one giant switch, is cut into
# ~7500 small functions by tools/flatten_fe2.py (at build time, from
# FE2_SOURCE). Same code, same runtime; emscripten needs it: on the giant
# function its wasm backend and ASYNCIFY take hours. Natively it's optional.
if(PLATFORM STREQUAL "Web")
    set(FE2_FLAT_DEFAULT ON)
else()
    set(FE2_FLAT_DEFAULT OFF)
endif()
option(FE2_FLAT "Split the game code into small functions (tools/flatten_fe2.py)" ${FE2_FLAT_DEFAULT})

if(FE2_FLAT)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(FE2_FLAT_DIR ${CMAKE_BINARY_DIR}/fe2_flat)
    set(FE2_FLAT_CHUNKS 8)
    set(FE2_FLAT_SOURCES ${FE2_FLAT_DIR}/fe2_flat_main.c)
    math(EXPR FE2_FLAT_LAST "${FE2_FLAT_CHUNKS} - 1")
    foreach(k RANGE ${FE2_FLAT_LAST})
        list(APPEND FE2_FLAT_SOURCES ${FE2_FLAT_DIR}/fe2_flat_${k}.c)
    endforeach()
    add_custom_command(
        OUTPUT ${FE2_FLAT_SOURCES} ${FE2_FLAT_DIR}/fe2_flat.h
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/flatten_fe2.py
                ${CMAKE_CURRENT_LIST_DIR}/${FE2_SOURCE} ${FE2_FLAT_DIR} ${FE2_FLAT_CHUNKS}
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/${FE2_SOURCE} ${CMAKE_CURRENT_LIST_DIR}/tools/flatten_fe2.py
        COMMENT "Splitting ${FE2_SOURCE} into small functions")

    # part one: the blocks and the dispatch loop; part two: the instruction
    # helpers, straight from the original file
    add_library(fe2_part_one OBJECT ${FE2_FLAT_SOURCES})
    target_include_directories(fe2_part_one PRIVATE ${FE2_FLAT_DIR})
    add_library(fe2_part_two OBJECT ${FE2_SOURCE})
    target_compile_definitions(fe2_part_two PRIVATE PART2)
    if(MSVC)
        target_compile_options(fe2_part_one PRIVATE /w)
        target_compile_options(fe2_part_two PRIVATE /w)
    elseif(EMSCRIPTEN)
        # (no -fcommon: wasm has no common symbols, clang crashes on it)
        target_compile_options(fe2_part_one PRIVATE -O2 -fwrapv -w)
        target_compile_options(fe2_part_two PRIVATE -O2 -fwrapv -w)
    else()
        target_compile_options(fe2_part_one PRIVATE -O1 -fwrapv -fomit-frame-pointer -fcommon -w)
        target_compile_options(fe2_part_two PRIVATE -O1 -fwrapv -fomit-frame-pointer -fcommon -w)
    endif()
    list(APPEND LIBRARIES fe2_part_one fe2_part_two)
elseif(PLATFORM STREQUAL "Web")
    message(FATAL_ERROR "The web build needs FE2_FLAT=ON")
else()
    # the game code is huge, so it is compiled in two halves
    add_library(fe2_part_one OBJECT ${FE2_SOURCE})
    add_library(fe2_part_two OBJECT ${FE2_SOURCE})
    target_compile_definitions(fe2_part_one PRIVATE PART1)
    target_compile_definitions(fe2_part_two PRIVATE PART2)
    if(NOT MSVC)
        target_compile_options(fe2_part_one PRIVATE -O1 -fwrapv -fomit-frame-pointer -fcommon -w)
        target_compile_options(fe2_part_two PRIVATE -O0 -fwrapv -fomit-frame-pointer -fcommon -w)
    endif()
    list(APPEND LIBRARIES fe2_part_one fe2_part_two)
endif()

##########################################################################################
# Game executable
##########################################################################################

file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")

# Custom ship models and ships (tools/fe2ShipBuilder, src/custom_ships.c): every
# tools/fe2ShipBuilder/custom_ships/NNN.fe2m is compiled into the executable, so the game stays one
# standalone file. Adding, changing or deleting a file there re-runs this on
# the next build. (.fe2m.off and .bak files are left out.) Modded builds only:
# the original game has no hook for them.
if(GLF_MODDED_FE2)
    set(FE2_SHIP_FILES_DIR "${CMAKE_CURRENT_LIST_DIR}/tools/fe2ShipBuilder/custom_ships")
    file(GLOB FE2_SHIP_FILES CONFIGURE_DEPENDS "${FE2_SHIP_FILES_DIR}/*.fe2m")
    list(SORT FE2_SHIP_FILES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FE2_SHIP_FILES})
    set(FE2_SHIPS_C "${CMAKE_CURRENT_BINARY_DIR}/generated/custom_ships_data.c")
    set(FE2_SHIPS_CONTENT "/* Generated by CMakeLists.txt from tools/fe2ShipBuilder/custom_ships - do not edit. */
#include \"custom_ships.h\"

")
    set(FE2_SHIPS_TABLE "")
    set(FE2_SHIP_I 0)
    foreach(FE2_SHIP_FILE ${FE2_SHIP_FILES})
        get_filename_component(FE2_SHIP_NAME "${FE2_SHIP_FILE}" NAME)
        file(READ "${FE2_SHIP_FILE}" FE2_SHIP_HEX HEX)
        string(LENGTH "${FE2_SHIP_HEX}" FE2_SHIP_HEXLEN)
        math(EXPR FE2_SHIP_LEN "${FE2_SHIP_HEXLEN} / 2")
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FE2_SHIP_BYTES "${FE2_SHIP_HEX}")
        string(APPEND FE2_SHIPS_CONTENT "static const unsigned char ship${FE2_SHIP_I}[] = {${FE2_SHIP_BYTES}};
")
        string(APPEND FE2_SHIPS_TABLE "    {\"${FE2_SHIP_NAME}\", ship${FE2_SHIP_I}, ${FE2_SHIP_LEN}},
")
        math(EXPR FE2_SHIP_I "${FE2_SHIP_I} + 1")
    endforeach()
    string(APPEND FE2_SHIPS_CONTENT "
const CustomShipFile custom_ships_embedded[] = {
${FE2_SHIPS_TABLE}    {0, 0, 0},
};
")
    file(CONFIGURE OUTPUT "${FE2_SHIPS_C}" CONTENT "${FE2_SHIPS_CONTENT}" @ONLY)
    list(APPEND PROJECT_SOURCES "${FE2_SHIPS_C}")
    message(STATUS "Custom ship models compiled in: ${FE2_SHIP_I} file(s) from tools/fe2ShipBuilder/custom_ships")
endif()

# Windows: the exe's icon (src/glfrontier.ico, from tools/icon/make_icon.py),
# and Winsock for head tracking (src/headtrack.c)
if(WIN32)
    list(APPEND LIBRARIES ws2_32)
    enable_language(RC)
    list(APPEND PROJECT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/glfrontier.rc")
endif()

if(ANDROID)
    add_library(${PROJECT_NAME} SHARED ${PROJECT_SOURCES})
else()
    add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})
endif()
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

if(NOT MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall)
endif()

##########################################################################################
# Linking
##########################################################################################

if(PLATFORM STREQUAL "Web" OR ANDROID)
    target_link_libraries(${PROJECT_NAME} ${LIBRARIES})
    if(ANDROID)
        # nothing in C calls SDL's hidapi JNI code, so the linker would leave
        # it out of the static SDL; its Java side needs it at startup
        target_link_options(${PROJECT_NAME} PRIVATE
            -Wl,-u,Java_org_libsdl_app_HIDDeviceManager_HIDDeviceRegisterCallback)
    endif()
elseif(MSVC)
    target_link_libraries(${PROJECT_NAME} ${LIBRARIES})
    # no console window in release builds
    set_target_properties(${PROJECT_NAME} PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /ZI") # edit and continue
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SAFESEH:NO")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /SAFESEH:NO")
elseif(CMAKE_C_COMPILER_ID STREQUAL "Intel")
    target_link_libraries(${PROJECT_NAME} ${LIBRARIES} -static-intel -lpthread)
else()
    # GCC / Clang: fully static executable on Windows. Elsewhere libc stays
    # dynamic: a static glibc binary crashes when SDL dlopens the GL driver.
    target_link_libraries(${PROJECT_NAME} ${LIBRARIES}
        -static-libgcc -static-libstdc++ -pthread)
    if(WIN32)
        target_link_options(${PROJECT_NAME} PRIVATE -static)
    endif()
    if(WIN32 AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_link_options(${PROJECT_NAME} PRIVATE -mwindows) # no console window
    endif()
endif()

##########################################################################################
# Web (emscripten)
##########################################################################################

# the game loop never returns to the browser: Call_Idle's SDL_Delay yields
# through ASYNCIFY, so the web build always needs it
if(ASYNCIFY OR PLATFORM STREQUAL "Web")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} \
        -s ASYNCIFY \
        -s TOTAL_STACK=8MB \
        -s ASYNCIFY_STACK_SIZE=131072 \
        -s INITIAL_MEMORY=128MB \
        -s MAXIMUM_MEMORY=512MB \
        -s ALLOW_MEMORY_GROWTH=1")
endif()

if(PLATFORM STREQUAL "Web")
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "index" SUFFIX ".html")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    if(USE_SDL2)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} \
            -s USE_SDL=2 \
            -s USE_SDL_IMAGE=2 \
            -s MAX_WEBGL_VERSION=2 \
            -s FULL_ES3=1 \
            --use-preload-plugins \
            -s FORCE_FILESYSTEM=1 \
            -lidbfs.js \
            -s GL_UNSAFE_OPTS=0 \
            --shell-file ../vendor/minshell.html")
    endif()
endif()
