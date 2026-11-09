cmake_minimum_required(VERSION 3.16)

# C: the Wayland protocol glue qtwaylandscanner generates
project(lingmo-screenshot LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

option(BUILD_TESTING "Build the unit tests" ON)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core DBus Gui Widgets Quick LinguistTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-xinput xcb-xtest)
# Wayland: the capture overlay is a layer-shell surface
find_package(LayerShellQt REQUIRED)
# Wayland: screen recording through KWin's screencast protocol
find_package(Qt6 REQUIRED COMPONENTS WaylandClient)

# Image logic without any window: shared by the app and the unit tests.
add_library(screenshot-core STATIC
    src/annotation.cpp
    src/annotationmodel.cpp
    src/annotationrenderer.cpp
    src/beautify.cpp
    src/commandline.cpp
    src/desktopapps.cpp
    src/filenamepattern.cpp
    src/imageanalysis.cpp
    src/ocr.cpp
    src/screenaskcore.cpp
)
target_include_directories(screenshot-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(screenshot-core PUBLIC Qt6::Core Qt6::Gui)

set(PROJECT_SOURCES
    src/main.cpp
    src/annotationcanvas.cpp
    src/application.cpp
    src/assistantengine.cpp
    src/capturecontroller.cpp
    src/capturesession.cpp
    src/config.cpp
    src/historymodel.cpp
    src/itemwindow.cpp
    src/kwinscreenshot.cpp
    src/kwinscreencast.cpp
    src/notifier.cpp
    src/pinwindow.cpp
    src/screenask.cpp
    src/screenrecorder.cpp
    src/scrollcapture.cpp
    src/x11windows.cpp
    qml.qrc
)

add_executable(lingmo-screenshot
  ${PROJECT_SOURCES}
)

# zkde_screencast_unstable_v1: from plasma-wayland-protocols when installed, else
# the copy kept here (LGPL-2.1-or-later)
find_package(PlasmaWaylandProtocols QUIET)
if (PlasmaWaylandProtocols_FOUND)
    set(SCREENCAST_XML "${PLASMA_WAYLAND_PROTOCOLS_DIR}/zkde-screencast-unstable-v1.xml")
else()
    set(SCREENCAST_XML "${CMAKE_CURRENT_SOURCE_DIR}/src/protocols/zkde-screencast-unstable-v1.xml")
endif()
qt6_generate_wayland_protocol_client_sources(lingmo-screenshot FILES "${SCREENCAST_XML}")

target_link_libraries(lingmo-screenshot
  PRIVATE
  screenshot-core
  Qt6::Core
  Qt6::DBus
  Qt6::Gui
  Qt6::Widgets
  Qt6::Quick
  PkgConfig::XCB
  LayerShellQt::Interface
  Qt6::WaylandClient
)

# Lingmo Intents: screenshot.capture, screen.record.start/stop, screenshot.ocr
# on com.lingmo.Screenshot /Intents (D-Bus activation: lingmo-screenshot --intents)
find_package(LingmoIntents 1.0 QUIET)
if(LingmoIntents_FOUND)
  target_sources(lingmo-screenshot PRIVATE src/screenshotintents.cpp src/screenshotintents.h)
  target_compile_definitions(lingmo-screenshot PRIVATE HAVE_LINGMO_INTENTS)
  target_link_libraries(lingmo-screenshot PRIVATE LingmoIntents::LingmoIntents)
  lingmo_install_intents(MANIFEST data/intents/lingmo-screenshot.json SERVICE data/com.lingmo.Screenshot.service)
else()
  message(WARNING "LingmoIntents not found: screenshots are built without app actions (intents)")
endif()

file(GLOB TS_FILES translations/*.ts)
foreach(filepath ${TS_FILES})
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" filename ${filepath})
    list(APPEND ts_files_replaced ${filename})
endforeach()
qt_add_translation(QM_FILES ${ts_files_replaced})
add_custom_target(translations ALL DEPENDS ${QM_FILES} SOURCES ${TS_FILES})

install(FILES ${QM_FILES} DESTINATION /usr/share/lingmo-screenshot/translations)

install(TARGETS lingmo-screenshot RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# OCR language packs: list, install and remove (shared with lingmo-settings,
# Idioma › Idiomas de leitura)
install(PROGRAMS data/lingmo-ocr-languages DESTINATION /usr/lib/lingmo-screenshot)
# The package is called lingmo-screenshots: accept that name on the command line too.
install(CODE "file(CREATE_LINK lingmo-screenshot
    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/lingmo-screenshots\" SYMBOLIC)")

install(FILES
    lingmo-screenshot.desktop
    DESTINATION /usr/share/applications/
    COMPONENT Runtime
)

if(BUILD_TESTING)
    enable_testing()
    find_package(Qt6 CONFIG REQUIRED COMPONENTS Test)
    add_executable(tst_core tests/tst_core.cpp)
    target_link_libraries(tst_core PRIVATE screenshot-core Qt6::Test)
    add_test(NAME tst_core COMMAND tst_core)
    set_tests_properties(tst_core PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
