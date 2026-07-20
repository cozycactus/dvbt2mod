cmake_minimum_required(VERSION 3.20)

project(dvbt2mod VERSION 0.1.0 LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The executable uses only the C++ API.  Avoid making Python a configure-time
# dependency through GnuradioConfig.cmake, then restore the caller's cache.
if(DEFINED CACHE{ENABLE_PYTHON})
  set(DVBT2MOD_HAD_ENABLE_PYTHON_CACHE TRUE)
  set(DVBT2MOD_SAVED_ENABLE_PYTHON "$CACHE{ENABLE_PYTHON}")
endif()
set(ENABLE_PYTHON OFF CACHE BOOL "Enable GNU Radio Python bindings" FORCE)
find_package(
  Gnuradio 3.10...<3.11 REQUIRED
  COMPONENTS blocks fec fft filter analog digital dtv
)
if(DVBT2MOD_HAD_ENABLE_PYTHON_CACHE)
  set(
    ENABLE_PYTHON "${DVBT2MOD_SAVED_ENABLE_PYTHON}"
    CACHE BOOL "Enable GNU Radio Python bindings" FORCE
  )
else()
  unset(ENABLE_PYTHON CACHE)
endif()
unset(DVBT2MOD_HAD_ENABLE_PYTHON_CACHE)
unset(DVBT2MOD_SAVED_ENABLE_PYTHON)

add_executable(dvbt2mod src/main.cpp)
if(WIN32)
  target_compile_definitions(
    dvbt2mod
    PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601
  )
endif()
if(MINGW)
  # Select MinGW's wide-character console startup for wmain().
  target_link_options(dvbt2mod PRIVATE -municode)
endif()
target_link_libraries(
  dvbt2mod
  PRIVATE
    gnuradio::gnuradio-runtime
    gnuradio::gnuradio-blocks
    gnuradio::gnuradio-digital
    gnuradio::gnuradio-dtv
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(dvbt2mod PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # GNU Radio 3.10 and GMP headers trigger these warnings under newer Clang.
  target_compile_options(
    dvbt2mod PRIVATE -Wno-unused-parameter -Wno-deprecated-literal-operator
  )
endif()

include(GNUInstallDirs)
install(TARGETS dvbt2mod RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
  FILES README.md LICENSE
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)

include(CTest)
if(BUILD_TESTING)
  if(WIN32)
    # A tiny native control used by the Windows workflow to prove that its
    # Process/BaseStream harness observes anonymous-pipe bytes without text
    # conversion. It is never installed or included in release archives.
    add_executable(
      dvbt2mod_windows_pipe_probe
      tests/windows_pipe_probe.cpp
    )
    target_compile_definitions(
      dvbt2mod_windows_pipe_probe
      PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601
    )
  endif()

  add_test(NAME dvbt2mod_help COMMAND dvbt2mod --help)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME dvbt2mod_cli
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.py"
        "$<TARGET_FILE:dvbt2mod>"
    )
    set_tests_properties(dvbt2mod_cli PROPERTIES TIMEOUT 30)
  endif()

  option(
    DVBT2MOD_ENABLE_NETWORK_TESTS
    "Download and run GNU Radio's official DVB-T2 golden vector"
    OFF
  )
  if(DVBT2MOD_ENABLE_NETWORK_TESTS)
    if(NOT Python3_Interpreter_FOUND)
      find_package(Python3 REQUIRED COMPONENTS Interpreter)
    endif()
    add_test(
      NAME dvbt2mod_gnuradio_golden
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden_test.py"
        "$<TARGET_FILE:dvbt2mod>"
    )
    set_tests_properties(dvbt2mod_gnuradio_golden PROPERTIES TIMEOUT 120)
  endif()
endif()
