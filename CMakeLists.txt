cmake_minimum_required(VERSION 3.16)
project(obs-raw-tcp-udp-output VERSION 0.1.0 LANGUAGES C)

set(PLUGIN_NAME raw-tcp-udp-output)

# libobs from an obs-studio build/install tree exporting the CMake package
# (CI builds libobs from source and points CMAKE_PREFIX_PATH at it).
find_package(libobs REQUIRED CONFIG)

add_library(${PLUGIN_NAME} MODULE src/raw-tcp-udp-output.c)

target_link_libraries(${PLUGIN_NAME} PRIVATE OBS::libobs)

if(WIN32)
  target_link_libraries(${PLUGIN_NAME} PRIVATE ws2_32)
endif()

# OBS loads modules by their plain name, never with a "lib" prefix.
set_target_properties(${PLUGIN_NAME} PROPERTIES PREFIX "" C_STANDARD 11)

# Portable layout: extract the archive into the OBS install directory.
install(TARGETS ${PLUGIN_NAME} LIBRARY DESTINATION obs-plugins/64bit
                               RUNTIME DESTINATION obs-plugins/64bit)
install(DIRECTORY data/ DESTINATION data/obs-plugins/${PLUGIN_NAME})
