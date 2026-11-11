cmake_minimum_required(VERSION 3.24)
project(swml LANGUAGES C CXX)
option(SWML_STARTUP_LOADER "Use in-process startup loading (disable for Proton)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build x64 only. Use: cmake -A x64")
endif()
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

include(FetchContent)
FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537)
FetchContent_MakeAvailable(minhook)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libdeflate GIT_REPOSITORY https://github.com/ebiggers/libdeflate.git GIT_TAG v1.24)
FetchContent_MakeAvailable(libdeflate)

add_subdirectory(sdk)
if(NOT EXISTS "${SWML_GAME_DATA_DIR}/native_ui_signatures.h")
  message(FATAL_ERROR "SWML_GAME_DATA_DIR also needs native_ui_signatures.h")
endif()

set(SWML_DEFS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set(SWML_OPTS /W4 /permissive- /utf-8)

add_library(swml_core SHARED
  core/core_main.cpp
  core/core_events.cpp
  core/core_crash.cpp
  core/core_v2.cpp
  core/core_v3.cpp
  core/core_v4.cpp
  core/world_context.cpp
  core/input_runtime.cpp
  core/native_input.cpp
  core/ui_runtime.cpp
  core/native_ui.cpp
  core/log_sessions.cpp)
target_include_directories(swml_core PRIVATE "${SWML_GAME_DATA_DIR}")
target_link_libraries(swml_core PRIVATE swml_lookup minhook libdeflate_static)
target_compile_definitions(swml_core PRIVATE ${SWML_DEFS})
target_compile_options(swml_core PRIVATE ${SWML_OPTS})

add_executable(swml WIN32
  launcher/main.cpp
  launcher/zip.cpp
  launcher/management.cpp
  launcher/updates.cpp
  launcher/field_dialog.cpp
  launcher/settings_editor.cpp
  launcher/settings_panel.cpp)
target_include_directories(swml PRIVATE sdk)
target_link_libraries(swml PRIVATE libdeflate_static)
target_compile_definitions(swml PRIVATE ${SWML_DEFS})
target_compile_options(swml PRIVATE ${SWML_OPTS})
if(SWML_STARTUP_LOADER)
  enable_language(ASM_MASM)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(BOOTSTRAP_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/bootstrap_generated")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bootstrap/generate_exports.py"
    "$ENV{SystemRoot}/System32/opengl32.dll" "${BOOTSTRAP_GENERATED}" COMMAND_ERROR_IS_FATAL ANY)
  add_library(swml_bootstrap SHARED bootstrap/bootstrap.cpp "${BOOTSTRAP_GENERATED}/forward.asm" "${BOOTSTRAP_GENERATED}/exports.def")
  target_include_directories(swml_bootstrap PRIVATE "${BOOTSTRAP_GENERATED}")
  target_compile_definitions(swml_bootstrap PRIVATE ${SWML_DEFS})
  target_compile_options(swml_bootstrap PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>")
  set_target_properties(swml_bootstrap PROPERTIES OUTPUT_NAME "opengl32")
  add_executable(swml_startup_setup WIN32 bootstrap/setup.cpp)
  target_include_directories(swml_startup_setup PRIVATE sdk)
  target_compile_definitions(swml_startup_setup PRIVATE ${SWML_DEFS})
  target_compile_options(swml_startup_setup PRIVATE ${SWML_OPTS})
  target_compile_definitions(swml PRIVATE SWML_STARTUP_LOADER=1)
  target_compile_definitions(swml_core PRIVATE SWML_STARTUP_LOADER=1)
endif()
