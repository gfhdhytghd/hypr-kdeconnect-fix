if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

find_program(READELF_EXECUTABLE readelf)
if(NOT READELF_EXECUTABLE)
  message(FATAL_ERROR "readelf is required for hardening-readelf-test")
endif()

if(NOT EXISTS "${HKCF_EXECUTABLE}")
  message(FATAL_ERROR "missing executable: ${HKCF_EXECUTABLE}")
endif()

execute_process(
  COMMAND "${READELF_EXECUTABLE}" -hW "${HKCF_EXECUTABLE}"
  OUTPUT_VARIABLE elf_header
  ERROR_VARIABLE readelf_error
  RESULT_VARIABLE readelf_result
)
if(NOT readelf_result EQUAL 0)
  message(FATAL_ERROR "readelf -h failed: ${readelf_error}")
endif()
if(NOT elf_header MATCHES "Type:[ ]*DYN")
  message(FATAL_ERROR "executable is not PIE/ET_DYN")
endif()

execute_process(
  COMMAND "${READELF_EXECUTABLE}" -lW "${HKCF_EXECUTABLE}"
  OUTPUT_VARIABLE program_headers
  ERROR_VARIABLE readelf_error
  RESULT_VARIABLE readelf_result
)
if(NOT readelf_result EQUAL 0)
  message(FATAL_ERROR "readelf -l failed: ${readelf_error}")
endif()
if(NOT program_headers MATCHES "GNU_RELRO")
  message(FATAL_ERROR "GNU_RELRO segment is missing")
endif()
if(program_headers MATCHES "GNU_STACK[^\\n]*RWE")
  message(FATAL_ERROR "GNU_STACK is executable")
endif()

execute_process(
  COMMAND "${READELF_EXECUTABLE}" -dW "${HKCF_EXECUTABLE}"
  OUTPUT_VARIABLE dynamic_section
  ERROR_VARIABLE readelf_error
  RESULT_VARIABLE readelf_result
)
if(NOT readelf_result EQUAL 0)
  message(FATAL_ERROR "readelf -d failed: ${readelf_error}")
endif()
if(NOT dynamic_section MATCHES "BIND_NOW|Flags:[^\\n]*NOW")
  message(FATAL_ERROR "BIND_NOW is missing")
endif()
