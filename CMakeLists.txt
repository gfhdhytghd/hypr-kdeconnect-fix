cmake_minimum_required(VERSION 3.20)
project(hypr-kdeconnect-fix VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(CTest)
include(GNUInstallDirs)

add_compile_options(
  -Wall
  -Wextra
  -Wpedantic
)

find_package(PkgConfig REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core DBus)

pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)

set(PROTOCOL_SOURCES)
foreach(PROTOCOL_NAME
  virtual-keyboard-unstable-v1
  wlr-virtual-pointer-unstable-v1
)
  set(PROTOCOL_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL_NAME}.xml)
  set(PROTOCOL_HEADER ${CMAKE_CURRENT_BINARY_DIR}/protocols/${PROTOCOL_NAME}-client-protocol.h)
  set(PROTOCOL_CODE ${CMAKE_CURRENT_BINARY_DIR}/protocols/${PROTOCOL_NAME}-protocol.c)

  add_custom_command(
    OUTPUT ${PROTOCOL_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/protocols
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${PROTOCOL_XML} ${PROTOCOL_HEADER}
    DEPENDS ${PROTOCOL_XML}
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${PROTOCOL_CODE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/protocols
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${PROTOCOL_XML} ${PROTOCOL_CODE}
    DEPENDS ${PROTOCOL_XML} ${PROTOCOL_HEADER}
    VERBATIM
  )
  list(APPEND PROTOCOL_SOURCES ${PROTOCOL_HEADER} ${PROTOCOL_CODE})
endforeach()

add_executable(hypr-kdeconnect-portal
  ${PROTOCOL_SOURCES}
  src/key_resolver.cpp
  src/main.cpp
  src/portal_backend.cpp
  src/wayland_input.cpp
)

target_include_directories(hypr-kdeconnect-portal PRIVATE
  src
  ${CMAKE_CURRENT_BINARY_DIR}/protocols
)
target_link_libraries(hypr-kdeconnect-portal PRIVATE
  Qt6::Core
  Qt6::DBus
  PkgConfig::WAYLAND_CLIENT
  PkgConfig::XKBCOMMON
)

if(BUILD_TESTING)
  add_executable(key-resolver-test
    src/key_resolver.cpp
    tests/key_resolver_test.cpp
  )
  target_include_directories(key-resolver-test PRIVATE src)
  target_link_libraries(key-resolver-test PRIVATE
    Qt6::Core
    PkgConfig::XKBCOMMON
  )
  add_test(NAME key-resolver-test COMMAND key-resolver-test)
endif()

configure_file(data/hypr-kdeconnect.portal.in hypr-kdeconnect.portal @ONLY)
configure_file(data/org.freedesktop.impl.portal.desktop.hypr_kdeconnect.service.in
  org.freedesktop.impl.portal.desktop.hypr_kdeconnect.service
  @ONLY
)

install(TARGETS hypr-kdeconnect-portal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hypr-kdeconnect.portal
  DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.hypr_kdeconnect.service
  DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services
)
