# Out-of-tree build of upstream's host unit-test target. The firmware source list is read from
# upstream's own test/CMakeLists.txt so it never drifts; only the gtest download is replaced by the
# prebuilt copy under /opt/toolchains/deps.
#   FUZZ_LIB=ON : static library of firmware + emulation layer (linked into the fuzz harnesses)
#   FUZZ_LIB=OFF: upstream's gtest binary (the behavioral oracle)
cmake_minimum_required(VERSION 3.20)
project(BatteryEmulatorMayhem CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(UP "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(GTEST_PREFIX "/opt/toolchains/deps/googletest-1.17.0" CACHE PATH "")
option(FUZZ_LIB "build the fuzz library instead of the gtest binary" OFF)

file(STRINGS "${UP}/test/CMakeLists.txt" FW_LINES REGEX "^ +\\.\\./Software/src/.*\\.cpp$")
set(FW_SOURCES "")
foreach(line IN LISTS FW_LINES)
  string(STRIP "${line}" rel)
  list(APPEND FW_SOURCES "${UP}/test/${rel}")
endforeach()
list(LENGTH FW_SOURCES FW_COUNT)
if(FW_COUNT LESS 50)
  message(FATAL_ERROR "failed to extract the firmware source list from upstream test/CMakeLists.txt")
endif()

add_compile_definitions(UNIT_TEST ESP32 HW_LILYGO COMMON_IMAGE)
include_directories("${UP}/test/emul" "${UP}/Software")

file(GLOB_RECURSE EMUL_SOURCES "${UP}/test/emul/*.cpp")

if(FUZZ_LIB)
  add_library(batteryfw STATIC ${FW_SOURCES} ${EMUL_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../harness/stubs.cpp")
  # One section per variable, so world_reset.ld can gather the firmware's mutable globals (every
  # file-scope static, function-local static and its init guard) into two ranges the harness resets
  # per input (world_reset.h).
  target_compile_options(batteryfw PRIVATE -fdata-sections)
else()
  include_directories("${GTEST_PREFIX}/include")
  file(GLOB_RECURSE TEST_SOURCES "${UP}/test/*.cpp")
  list(FILTER TEST_SOURCES EXCLUDE REGEX "/build/")
  add_executable(tests ${TEST_SOURCES} ${FW_SOURCES})
  target_link_libraries(tests "${GTEST_PREFIX}/lib/libgmock.a" "${GTEST_PREFIX}/lib/libgtest.a" pthread)
  target_compile_definitions(tests PRIVATE TEST_CAN_LOG_DIR="${UP}/test/can_log_based/can_logs")
endif()
