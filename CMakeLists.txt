cmake_minimum_required(VERSION 3.16)
project(RpfStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(RPFSTUDIO_BUILD_GUI "Build the Qt GUI" ON)
option(RPFSTUDIO_BUILD_TESTS "Build tests" ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Xml)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(rpfcore STATIC
  src/core/Crypto.cpp
  src/core/Compression.cpp
  src/core/Rpf.cpp
  src/core/RpfWriter.cpp
  src/core/Resource.cpp
  src/core/Texture.cpp
  src/core/Ytd.cpp
  src/core/Drawable.cpp
  src/core/Gxt2.cpp
  src/core/Zip.cpp
  src/core/GameFS.cpp
  src/core/Oiv.cpp
  src/core/XPathLite.cpp
)
target_include_directories(rpfcore PUBLIC src/core)
target_link_libraries(rpfcore PUBLIC Qt6::Core Qt6::Gui Qt6::Xml ZLIB::ZLIB OpenSSL::Crypto Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rpfcore PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(rpfstudio-cli src/cli/main.cpp)
target_link_libraries(rpfstudio-cli PRIVATE rpfcore)

if(RPFSTUDIO_BUILD_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
  add_executable(rpfstudio
    src/gui/main.cpp
    src/gui/MainWindow.cpp
    src/gui/ArchiveModel.cpp
    src/gui/HexView.cpp
    src/gui/TextViewer.cpp
    src/gui/TextureViewer.cpp
    src/gui/ModelViewer.cpp
    src/gui/Gxt2Viewer.cpp
    src/gui/SettingsDialog.cpp
    src/gui/OivDialog.cpp
    src/gui/Viewers.cpp
  )
  target_link_libraries(rpfstudio PRIVATE rpfcore Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
  install(TARGETS rpfstudio RUNTIME DESTINATION bin)
  install(FILES packaging/rpfstudio.desktop DESTINATION share/applications)
endif()

install(TARGETS rpfstudio-cli RUNTIME DESTINATION bin)

if(RPFSTUDIO_BUILD_TESTS)
  enable_testing()
  add_executable(rpfcore_tests tests/core_tests.cpp)
  target_link_libraries(rpfcore_tests PRIVATE rpfcore)
  add_test(NAME rpfcore_tests COMMAND rpfcore_tests)
endif()
