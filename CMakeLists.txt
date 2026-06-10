cmake_minimum_required(VERSION 3.16)
project(obs-raw-tcp-udp-output VERSION 0.1.0 LANGUAGES C)

set(PLUGIN_NAME raw-tcp-udp-output)

# libobs comes either from an obs-studio install tree exporting the CMake
# package (Windows/macOS CI builds libobs from source) or from a distro
# package via pkg-config (Linux: libobs-dev).
find_package(libobs CONFIG QUIET)
if(NOT libobs_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBOBS REQUIRED IMPORTED_TARGET libobs)
endif()

add_library(${PLUGIN_NAME} MODULE src/raw-tcp-udp-output.c)

if(libobs_FOUND)
  target_link_libraries(${PLUGIN_NAME} PRIVATE OBS::libobs)
else()
  target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::LIBOBS)
endif()

if(WIN32)
  target_link_libraries(${PLUGIN_NAME} PRIVATE ws2_32)
endif()

# OBS loads modules by their plain name; CMake's default "lib" prefix on
# Unix would break that.
set_target_properties(${PLUGIN_NAME} PROPERTIES PREFIX "" C_STANDARD 11)

if(APPLE)
  # OBS 28+ on macOS only loads plugins packaged as .plugin bundles.
  set_target_properties(
    ${PLUGIN_NAME}
    PROPERTIES BUNDLE TRUE
               BUNDLE_EXTENSION plugin
               MACOSX_BUNDLE_GUI_IDENTIFIER "com.snakexgc.${PLUGIN_NAME}"
               MACOSX_BUNDLE_BUNDLE_NAME "${PLUGIN_NAME}"
               MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
               MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
endif()

if(WIN32)
  # Portable layout: extract the archive into the OBS install directory.
  install(TARGETS ${PLUGIN_NAME} LIBRARY DESTINATION obs-plugins/64bit
                                 RUNTIME DESTINATION obs-plugins/64bit)
  install(DIRECTORY data/ DESTINATION data/obs-plugins/${PLUGIN_NAME})
elseif(NOT APPLE)
  install(TARGETS ${PLUGIN_NAME} LIBRARY DESTINATION lib/obs-plugins)
  install(DIRECTORY data/ DESTINATION share/obs/obs-plugins/${PLUGIN_NAME})
endif()
