cmake_minimum_required(VERSION 3.24)
project(swml LANGUAGES C CXX)

# The launcher UI (Dear ImGui + SDL3) builds on Windows (MSVC), Linux and macOS, and cross-builds
# with mingw. The in-game core and the startup bootstrap are Windows-only and need generated
# per-game-build headers (SWML_GAME_DATA_DIR); without them only the launcher is built.
option(SWML_STARTUP_LOADER "Use in-process startup loading (disable for Proton)" ON)
option(SWML_BUILD_CORE "Build swml_core.dll / bootstrap (needs SWML_GAME_DATA_DIR)" ON)
option(SWML_STATIC_LINUX "Link the Linux launcher statically (single-file release)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build x64 only. Use: cmake -A x64")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libdeflate GIT_REPOSITORY https://github.com/ebiggers/libdeflate.git GIT_TAG v1.24)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.5)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.2.30 GIT_SHALLOW TRUE)
if(SWML_BUILD_CORE AND WIN32 AND MSVC)
  FetchContent_MakeAvailable(minhook libdeflate imgui SDL3)
else()
  FetchContent_MakeAvailable(libdeflate imgui SDL3)
endif()

add_subdirectory(sdk)

# --- Dear ImGui (core + SDL3 backends) ---------------------------------------------------------------
add_library(imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(imgui PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
if(MSVC)
  target_compile_options(imgui PRIVATE /utf-8)
endif()

# --- platform layer ------------------------------------------------------------------------------------
# Core subset: text/INI/files/locks, also linked into swml_core.dll (no shell/network imports).
if(WIN32)
  set(SWML_PLATFORM_CORE launcher/platform_win.cpp)
  set(SWML_PLATFORM_DESKTOP launcher/platform_desktop_win.cpp)
else()
  set(SWML_PLATFORM_CORE launcher/platform_posix.cpp)
  set(SWML_PLATFORM_DESKTOP launcher/platform_desktop_posix.cpp)
endif()
add_library(swml_platform_core STATIC ${SWML_PLATFORM_CORE})
target_include_directories(swml_platform_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(swml_platform_core PUBLIC cxx_std_20)
if(MSVC)
  target_compile_definitions(swml_platform_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_compile_options(swml_platform_core PRIVATE /W4 /permissive- /utf-8)
endif()

# --- launcher -------------------------------------------------------------------------------------------
set(SWML_LAUNCHER_SOURCES
  launcher/main.cpp
  launcher/ui.cpp
  launcher/launcher_model.cpp
  launcher/zip.cpp
  launcher/management.cpp
  launcher/updates.cpp
  ${SWML_PLATFORM_DESKTOP})
if(WIN32)
  list(APPEND SWML_LAUNCHER_SOURCES launcher/runner_win.cpp)
endif()
if(WIN32)
  add_executable(swml WIN32 ${SWML_LAUNCHER_SOURCES})
  if(MSVC)
    # main() entry with the GUI subsystem (no console window).
    target_link_options(swml PRIVATE /ENTRY:mainCRTStartup)
  endif()
else()
  add_executable(swml ${SWML_LAUNCHER_SOURCES})
endif()
target_include_directories(swml PRIVATE sdk launcher)
target_link_libraries(swml PRIVATE swml_platform_core imgui libdeflate_static)
if(WIN32)
  target_compile_definitions(swml PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  if(MSVC)
    target_compile_options(swml PRIVATE /W4 /permissive- /utf-8)
  endif()
  target_link_libraries(swml PRIVATE winhttp bcrypt ole32 shell32)
  if(MINGW)
    target_link_options(swml PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
else()
  if(SWML_STATIC_LINUX)
    # Single-file release: static libcurl/OpenSSL/libstdc++ so the launcher runs on any distro
    # (including SteamOS) without installing anything. Distro curl often pulls krb5/ldap/ssh2
    # which ship no static archives; for a fully static build use a minimal curl (see
    # linux/build-release.sh: HTTPS-only, OpenSSL, no optional protocols).
    target_link_options(swml PRIVATE -static-libgcc -static-libstdc++)
    # Link the archives explicitly: find_package(CURL) would hand back the system .so.
    find_library(SWML_CURL_A NAMES libcurl.a curl REQUIRED)
    find_library(SWML_SSL_A NAMES libssl.a ssl REQUIRED)
    find_library(SWML_CRYPTO_A NAMES libcrypto.a crypto REQUIRED)
    find_package(PkgConfig REQUIRED)
    # Resolve curl's full static dependency chain via pkg-config, minus the libraries we
    # already link explicitly (they must come from the archives above).
    execute_process(COMMAND ${PKG_CONFIG_EXECUTABLE} --static --libs libcurl
                    OUTPUT_VARIABLE SWML_CURL_STATIC_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(SWML_CURL_STATIC_LIBS)
    list(REMOVE_ITEM SWML_CURL_STATIC_LIBS -lcurl -lssl -lcrypto)
    # Everything except libc/libm/libdl is linked statically (single-file release).
    # SDL's own dependency scan may pull libz/libzstd dynamically; force archives for them.
    find_library(SWML_Z_A NAMES libz.a z REQUIRED)
    find_library(SWML_ZSTD_A NAMES libzstd.a zstd)
    set(SWML_LINUX_NET_LIBS "${SWML_CURL_A}" -Wl,-Bstatic ${SWML_CURL_STATIC_LIBS} "${SWML_SSL_A}" "${SWML_CRYPTO_A}" "${SWML_Z_A}")
    if(SWML_ZSTD_A)
      list(APPEND SWML_LINUX_NET_LIBS "${SWML_ZSTD_A}")
    endif()
    list(APPEND SWML_LINUX_NET_LIBS -Wl,-Bdynamic)
    target_link_libraries(swml PRIVATE ${SWML_LINUX_NET_LIBS})
    message(STATUS "SWML static curl: ${SWML_CURL_A}")
  else()
    find_package(CURL REQUIRED)
    find_package(OpenSSL REQUIRED)
    set(SWML_LINUX_NET_LIBS CURL::libcurl OpenSSL::Crypto)
    target_link_libraries(swml PRIVATE ${SWML_LINUX_NET_LIBS})
  endif()
endif()

# --- in-game core + startup bootstrap (Windows, generated headers required) ------------------------------
if(SWML_BUILD_CORE AND WIN32 AND MSVC)
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
    core/status_display.cpp
    core/multiplayer_gate.cpp
    core/log_sessions.cpp)
  target_include_directories(swml_core PRIVATE "${SWML_GAME_DATA_DIR}")
  target_link_libraries(swml_core PRIVATE swml_lookup swml_platform_core minhook libdeflate_static bcrypt)
  target_compile_definitions(swml_core PRIVATE ${SWML_DEFS})
  target_compile_options(swml_core PRIVATE ${SWML_OPTS})

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
    target_link_libraries(swml_startup_setup PRIVATE swml_platform_core)
    target_compile_definitions(swml_startup_setup PRIVATE ${SWML_DEFS})
    target_compile_options(swml_startup_setup PRIVATE ${SWML_OPTS})
    target_compile_definitions(swml PRIVATE SWML_STARTUP_LOADER=1)
    target_compile_definitions(swml_core PRIVATE SWML_STARTUP_LOADER=1)
  endif()
elseif(SWML_BUILD_CORE)
  message(STATUS "SWML: core/bootstrap skipped (needs MSVC + generated headers); building the launcher only")
endif()

# --- tests -----------------------------------------------------------------------------------------------
enable_testing()
add_executable(swml_tests tests/tests.cpp launcher/zip.cpp launcher/management.cpp launcher/updates.cpp
  launcher/launcher_model.cpp ${SWML_PLATFORM_DESKTOP})
target_include_directories(swml_tests PRIVATE sdk launcher)
target_link_libraries(swml_tests PRIVATE swml_platform_core libdeflate_static)
if(WIN32)
  target_compile_definitions(swml_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(swml_tests PRIVATE winhttp bcrypt ole32 shell32)
  if(MINGW)
    target_link_options(swml_tests PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
else()
  target_link_libraries(swml_tests PRIVATE ${SWML_LINUX_NET_LIBS})
endif()
add_test(NAME swml_tests COMMAND swml_tests)

add_executable(swml_multiplayer_protocol_test tests/multiplayer_protocol_test.cpp)
target_include_directories(swml_multiplayer_protocol_test PRIVATE core)
add_test(NAME swml_multiplayer_protocol_test COMMAND swml_multiplayer_protocol_test)

add_executable(swml_multiplayer_simulation_test tests/multiplayer_simulation_test.cpp)
target_include_directories(swml_multiplayer_simulation_test PRIVATE core)
add_test(NAME swml_multiplayer_simulation_test COMMAND swml_multiplayer_simulation_test)

if(WIN32)
  add_executable(swml_multiplayer_transport_test tests/multiplayer_transport_test.cpp)
  target_include_directories(swml_multiplayer_transport_test PRIVATE sdk core)
  target_compile_definitions(swml_multiplayer_transport_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(swml_multiplayer_transport_test PRIVATE bcrypt)
  add_test(NAME swml_multiplayer_transport_test COMMAND swml_multiplayer_transport_test)
  add_executable(swml_status_test tests/status_test.cpp core/status_display.cpp)
  target_include_directories(swml_status_test PRIVATE sdk core)
  target_compile_definitions(swml_status_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  add_test(NAME swml_status_test COMMAND swml_status_test)
endif()
