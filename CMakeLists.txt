cmake_minimum_required(VERSION 3.16)
project(obs-raw-tcp-udp-output LANGUAGES C)

# 查找 OBS Studio
find_package(libobs REQUIRED)

add_library(raw-tcp-udp-output MODULE
    src/raw-tcp-udp-output.c
)

target_link_libraries(raw-tcp-udp-output PRIVATE libobs::libobs)

# Windows 需要链接 ws2_32
if(WIN32)
    target_link_libraries(raw-tcp-udp-output PRIVATE ws2_32)
endif()

# 安装到 OBS 插件目录
install(TARGETS raw-tcp-udp-output
    LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
    RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
)