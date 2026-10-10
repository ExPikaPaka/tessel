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

# ----------------------------------------------------------------- toolchain
# Optimisation flags and warning switches are compiler specific. The GCC and
# Clang spellings are kept exactly as they were; MSVC gets its own set so the
# Windows build is not fed flags it cannot parse.
if(MSVC)
  # CMake's own MSVC release flags (/O2 /Ob2 /DNDEBUG) already match the intent
  # of -O3, so they are left alone.
  add_compile_options(
    /utf-8            # source and execution charset are UTF-8, not the ANSI codepage
    /permissive-      # standard conformance, which Qt 6 expects
    /Zc:__cplusplus   # report a real __cplusplus value instead of 199711L
    /bigobj           # the larger translation units overflow the default section limit
  )
  add_compile_definitions(
    _USE_MATH_DEFINES # M_PI and friends, used throughout src/effects and src/tools
    NOMINMAX          # keep windows.h from defining min/max macros over std::min/max
    WIN32_LEAN_AND_MEAN
  )
  set(TESSEL_WARNING_FLAGS /W4 /wd4100)  # /wd4100 == -Wno-unused-parameter
else()
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
  set(TESSEL_WARNING_FLAGS -Wall -Wextra -Wno-unused-parameter)
endif()

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
  src/core/Palette.cpp
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
target_compile_options(tessel_core PRIVATE ${TESSEL_WARNING_FLAGS})

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
  src/tools/PlaceTool.cpp
  src/tools/TransformTool.cpp
  src/tools/MiscTools.cpp
)
target_link_libraries(tessel_ui PUBLIC tessel_core Qt6::Widgets Qt6::PrintSupport)
target_compile_options(tessel_ui PRIVATE ${TESSEL_WARNING_FLAGS})

# ----------------------------------------------------------------- app
qt_add_executable(tessel src/main.cpp)
target_link_libraries(tessel PRIVATE tessel_ui)
target_compile_options(tessel PRIVATE ${TESSEL_WARNING_FLAGS})
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
