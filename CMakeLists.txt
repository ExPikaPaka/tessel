cmake_minimum_required(VERSION 3.21)
project(Tessel VERSION 1.0.0 LANGUAGES CXX
        DESCRIPTION "Layered raster image editor")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

qt_standard_project_setup()

# ----------------------------------------------------------------- core
add_library(tessel_core STATIC
  src/core/Image.cpp
  src/core/Blend.cpp
  src/core/ImageOps.cpp
  src/core/Document.cpp
  src/core/Selection.cpp
  src/core/History.cpp
  src/core/Zip.cpp
  src/core/Io.cpp
  src/core/Units.cpp
  src/effects/Effect.cpp
  src/effects/Adjustments.cpp
  src/effects/Blurs.cpp
  src/effects/ColorAndObject.cpp
  src/effects/Distort.cpp
  src/effects/Stylize.cpp
  src/effects/Noise.cpp
  src/effects/Render.cpp
  src/effects/Photo.cpp
)
target_include_directories(tessel_core PUBLIC src)
target_link_libraries(tessel_core PUBLIC Qt6::Core Qt6::Gui ZLIB::ZLIB Threads::Threads)
target_compile_options(tessel_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ----------------------------------------------------------------- ui library
# The interface lives in a library so it can be exercised by the headless
# tests as well as by the application itself.
qt_add_library(tessel_ui STATIC
  src/ui/MainWindow.cpp
  src/ui/MainWindow.h
  src/ui/CanvasView.cpp
  src/ui/CanvasView.h
  src/ui/Docks.cpp
  src/ui/Docks.h
  src/ui/ColorPanel.cpp
  src/ui/ColorPanel.h
  src/ui/Dialogs.cpp
  src/ui/Dialogs.h
  src/ui/EffectDialog.cpp
  src/ui/EffectDialog.h
  src/ui/CurveWidget.cpp
  src/ui/CurveWidget.h
  src/ui/Icons.cpp
  src/ui/Icons.h
  src/ui/Theme.cpp
  src/ui/Theme.h
  src/tools/Tool.cpp
  src/tools/Tool.h
  src/tools/PaintTools.cpp
  src/tools/SelectTools.cpp
  src/tools/ShapeTools.cpp
  src/tools/MiscTools.cpp
)
target_link_libraries(tessel_ui PUBLIC tessel_core Qt6::Widgets Qt6::PrintSupport)
target_compile_options(tessel_ui PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ----------------------------------------------------------------- app
qt_add_executable(tessel src/main.cpp)
target_link_libraries(tessel PRIVATE tessel_ui)
target_compile_options(tessel PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(tessel PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)

# ----------------------------------------------------------------- docs tool
# Renders the interface to PNG files for the README. Off by default because it
# is only needed when the documentation images are refreshed.
option(TESSEL_BUILD_SCREENSHOT_TOOL "Build the documentation screenshot tool" OFF)
if(TESSEL_BUILD_SCREENSHOT_TOOL)
  qt_add_executable(tessel_screenshots tools/screenshot_tool.cpp)
  target_link_libraries(tessel_screenshots PRIVATE tessel_ui)
endif()

# ----------------------------------------------------------------- tests
enable_testing()
add_executable(tessel_tests tests/tests.cpp)
target_link_libraries(tessel_tests PRIVATE tessel_core)
add_test(NAME core COMMAND tessel_tests)

qt_add_executable(tessel_gui_tests tests/gui_tests.cpp)
target_link_libraries(tessel_gui_tests PRIVATE tessel_ui)
add_test(NAME gui COMMAND tessel_gui_tests)
set_tests_properties(gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ----------------------------------------------------------------- install
include(GNUInstallDirs)
install(TARGETS tessel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES resources/org.tessel.Tessel.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES resources/org.tessel.Tessel.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
