# Brand data for the launcher: the Chakra Petch font and the 64x64 window icon, as C++ arrays.
#
#   include(launcher/brand/embed.cmake)
#   swml_embed_brand(swml)        # adds <build>/brand_generated/swml_brand_data.h to the target
#
# The same file runs in script mode (cmake -P) to write the header.
if(CMAKE_SCRIPT_MODE_FILE)
  file(WRITE "${OUT}.tmp" "// Generated from launcher/brand by embed.cmake. Do not edit.\n#pragma once\n#include <cstddef>\n\nnamespace swml_brand {\n")
  foreach(pair IN ITEMS "font:ChakraPetch-BoldItalic.ttf" "icon64:icon64.rgba")
    string(REPLACE ":" ";" pair "${pair}")
    list(GET pair 0 name)
    list(GET pair 1 file)
    file(READ "${DIR}/${file}" hex HEX)
    string(LENGTH "${hex}" length)
    math(EXPR size "${length} / 2")
    string(REGEX REPLACE "(................................................................)" "\\1\n" hex "${hex}")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    file(APPEND "${OUT}.tmp" "alignas(4) inline constexpr unsigned char ${name}[${size}] = {\n${hex}\n};\n")
  endforeach()
  file(APPEND "${OUT}.tmp" "constexpr int icon64_size = 64;\n} // namespace swml_brand\n")
  file(RENAME "${OUT}.tmp" "${OUT}")
  return()
endif()

set(SWML_BRAND_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(swml_embed_brand target)
  set(gen "${CMAKE_CURRENT_BINARY_DIR}/brand_generated")
  set(out "${gen}/swml_brand_data.h")
  add_custom_command(OUTPUT "${out}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${out}" "-DDIR=${SWML_BRAND_DIR}" -P "${SWML_BRAND_DIR}/embed.cmake"
    DEPENDS "${SWML_BRAND_DIR}/embed.cmake" "${SWML_BRAND_DIR}/ChakraPetch-BoldItalic.ttf" "${SWML_BRAND_DIR}/icon64.rgba"
    COMMENT "Embedding launcher brand data"
    VERBATIM)
  target_sources(${target} PRIVATE "${out}")
  target_include_directories(${target} PRIVATE "${gen}")
  swml_brand_icon(${target})
endfunction()

# Just the S icon as the exe's icon (the installer uses this; the launcher gets it via swml_embed_brand).
function(swml_brand_icon target)
  if(WIN32)
    # ID 1 so Explorer lists it first; SDL also takes the first icon resource for the window and
    # taskbar. Absolute path: windres and rc.exe resolve relative ones differently.
    set(gen "${CMAKE_CURRENT_BINARY_DIR}/brand_generated")
    file(TO_CMAKE_PATH "${SWML_BRAND_DIR}/swml.ico" ico)
    file(CONFIGURE OUTPUT "${gen}/swml_icon.rc" CONTENT "1 ICON \"${ico}\"\n")
    target_sources(${target} PRIVATE "${gen}/swml_icon.rc")
  endif()
endfunction()
