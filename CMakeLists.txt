cmake_minimum_required(VERSION 3.20)
project(Kopru LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg shared dev paketi (include/lib/bin)")
set(SCRCPY_SERVER_DIR "${CMAKE_SOURCE_DIR}/third_party/scrcpy-server")
file(READ "${SCRCPY_SERVER_DIR}/VERSION" SCRCPY_SERVER_VERSION)
string(STRIP "${SCRCPY_SERVER_VERSION}" SCRCPY_SERVER_VERSION)

if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg bulunamadi: ${FFMPEG_DIR}. README'deki kurulum adimlarina bak.")
endif()

add_executable(kopru
    pc/src/main.cpp
    pc/src/adb.cpp
    pc/src/session.cpp
    pc/src/control_msg.cpp
    pc/src/decoder.cpp
    pc/src/renderer.cpp
)

target_include_directories(kopru SYSTEM PRIVATE "${FFMPEG_DIR}/include")
target_link_libraries(kopru PRIVATE
    "${FFMPEG_DIR}/lib/avcodec.lib"
    "${FFMPEG_DIR}/lib/avutil.lib"
    d3d11 dxgi dxguid d3dcompiler ws2_32
)
target_compile_definitions(kopru PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    SCRCPY_SERVER_VERSION="${SCRCPY_SERVER_VERSION}"
)
if(MSVC)
    target_compile_options(kopru PRIVATE /utf-8 /W4 /permissive-)
endif()

# Calisma zamani dosyalarini exe'nin yanina kopyala
file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
add_custom_command(TARGET kopru POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:kopru>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCRCPY_SERVER_DIR}/scrcpy-server" "$<TARGET_FILE_DIR:kopru>/scrcpy-server"
)
