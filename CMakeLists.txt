cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0083)
  cmake_policy(SET CMP0083 NEW)
endif()
project(hypr-kdeconnect-fix VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(CTest)
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
include(CheckPIESupported)
include(GNUInstallDirs)

option(HKCF_ENABLE_HARDENING "Enable compiler and linker hardening flags" ON)
set(HKCF_PORTAL_USE_IN "wlroots;Hyprland;sway;Wayfire;river;phosh;niri;labwc"
  CACHE STRING "Semicolon-separated XDG_CURRENT_DESKTOP values where this portal backend should be visible"
)

function(hkcf_flag_id out flag)
  string(MAKE_C_IDENTIFIER "${flag}" flag_id)
  string(TOUPPER "${flag_id}" flag_id)
  set(${out} "${flag_id}" PARENT_SCOPE)
endfunction()

function(hkcf_add_compile_flag_if_supported target flag)
  hkcf_flag_id(flag_id "${flag}")
  check_c_compiler_flag("${flag}" "HKCF_C_HAS_${flag_id}")
  check_cxx_compiler_flag("${flag}" "HKCF_CXX_HAS_${flag_id}")
  if(HKCF_C_HAS_${flag_id})
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:${flag}>)
  endif()
  if(HKCF_CXX_HAS_${flag_id})
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${flag}>)
  endif()
endfunction()

function(hkcf_add_link_flag_if_supported target flag)
  hkcf_flag_id(flag_id "${flag}")
  check_linker_flag(CXX "${flag}" "HKCF_LINK_HAS_${flag_id}")
  if(HKCF_LINK_HAS_${flag_id})
    target_link_options(${target} PRIVATE "${flag}")
  endif()
endfunction()

function(hkcf_apply_target_defaults target)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )

  if(HKCF_ENABLE_HARDENING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_property(TARGET ${target} PROPERTY POSITION_INDEPENDENT_CODE TRUE)
    target_compile_definitions(${target} PRIVATE
      $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:_FORTIFY_SOURCE=3>
    )
    foreach(flag
      -fPIE
      -fstack-protector-strong
      -fstack-clash-protection
      -fcf-protection=full
      -Wformat=2
    )
      hkcf_add_compile_flag_if_supported(${target} "${flag}")
    endforeach()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Werror=format-security>)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror=format-security>)
    endif()
    foreach(flag
      -pie
      LINKER:-z,relro
      LINKER:-z,now
      LINKER:-z,noexecstack
    )
      hkcf_add_link_flag_if_supported(${target} "${flag}")
    endforeach()
  endif()
endfunction()

if(HKCF_ENABLE_HARDENING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  check_pie_supported()
endif()
set(HKCF_EXPECT_PIE OFF)
if(HKCF_ENABLE_HARDENING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_LINK_PIE_SUPPORTED)
  set(HKCF_EXPECT_PIE ON)
endif()

find_package(PkgConfig REQUIRED)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus)

pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client>=1.20)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon>=1.5)
pkg_check_modules(LIBEIS REQUIRED IMPORTED_TARGET libeis-1.0>=1.4)

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
  src/eis_input_bridge.cpp
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
  PkgConfig::LIBEIS
  PkgConfig::WAYLAND_CLIENT
  PkgConfig::XKBCOMMON
)
hkcf_apply_target_defaults(hypr-kdeconnect-portal)

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
  hkcf_apply_target_defaults(key-resolver-test)
  add_test(NAME key-resolver-test COMMAND key-resolver-test)

  add_executable(security-policy-test
    tests/security_policy_test.cpp
  )
  target_include_directories(security-policy-test PRIVATE src)
  target_link_libraries(security-policy-test PRIVATE
    Qt6::Core
    PkgConfig::XKBCOMMON
  )
  hkcf_apply_target_defaults(security-policy-test)
  add_test(NAME security-policy-test COMMAND security-policy-test)
endif()

configure_file(data/hypr-kdeconnect.portal.in hypr-kdeconnect.portal @ONLY)
configure_file(data/org.freedesktop.impl.portal.desktop.hypr_kdeconnect.service.in
  org.freedesktop.impl.portal.desktop.hypr_kdeconnect.service
  @ONLY
)
configure_file(data/hypr-kdeconnect-portal.service.in hypr-kdeconnect-portal.service @ONLY)

install(TARGETS hypr-kdeconnect-portal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hypr-kdeconnect.portal
  DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.hypr_kdeconnect.service
  DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hypr-kdeconnect-portal.service
  DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user
)

if(BUILD_TESTING)
  add_test(NAME portal-metadata-test
    COMMAND ${CMAKE_COMMAND}
      -DHKCF_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DHKCF_INSTALL_FULL_BINDIR=${CMAKE_INSTALL_FULL_BINDIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/portal_metadata_test.cmake
  )
  add_test(NAME hardening-readelf-test
    COMMAND ${CMAKE_COMMAND}
      -DHKCF_EXECUTABLE=$<TARGET_FILE:hypr-kdeconnect-portal>
      -DHKCF_EXPECT_PIE=${HKCF_EXPECT_PIE}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/hardening_readelf_test.cmake
  )
endif()
