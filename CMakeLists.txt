cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# sokaster lives as a dock inside the OBS window, so both template options are on.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

# Ours, not the template's — kept in its own directory so the two never get
# confused when the template is updated.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sokaster/onnxruntime.cmake")

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# libcurl comes from obs-deps and ships with OBS itself, so nothing extra
# travels with the plugin.
find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/plugin-config.cpp
    src/plugin-config.hpp
    src/dock/sokaster-dock.cpp
    src/dock/sokaster-dock.hpp
    src/net/backend-client.cpp
    src/net/backend-client.hpp
    src/vision/frame-grabber.cpp
    src/vision/frame-grabber.hpp
    src/vision/jpeg-encoder.cpp
    src/vision/jpeg-encoder.hpp
    src/vision/source-picker.cpp
    src/vision/source-picker.hpp
    src/vision/vision-loop.cpp
    src/vision/vision-loop.hpp
    src/audio/audio-loop.cpp
    src/audio/audio-loop.hpp
    src/audio/audio-tap.cpp
    src/audio/audio-tap.hpp
    src/audio/silero-vad.cpp
    src/audio/silero-vad.hpp
    src/audio/vad-segmenter.cpp
    src/audio/vad-segmenter.hpp
    src/audio/wav-encoder.cpp
    src/audio/wav-encoder.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Voice activity detection runs a small ONNX model, so the runtime travels with
# the plugin: neither OBS nor obs-deps carries it. After the plugin properties,
# because on macOS this copies into the bundle the line above creates.
target_add_onnxruntime(${CMAKE_PROJECT_NAME})
