cmake_minimum_required(VERSION 3.20)

project(ClipBank VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- third party
set(CLIPBANK_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

add_library(sqlite3 STATIC "${CLIPBANK_THIRD_PARTY_DIR}/sqlite/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${CLIPBANK_THIRD_PARTY_DIR}/sqlite")
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_DQS=0
  SQLITE_DEFAULT_FOREIGN_KEYS=1
)
target_compile_options(sqlite3 PRIVATE -w)
target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

# --------------------------------------------------------------------- core
add_library(clipbank_core STATIC
  src/util.cpp
  src/sha256.cpp
  src/subprocess.cpp
  src/probe.cpp
  src/profile.cpp
  src/db.cpp
  src/library.cpp
  src/pipeline.cpp
  src/manifest.cpp
  src/verify.cpp
)

target_include_directories(clipbank_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CLIPBANK_THIRD_PARTY_DIR}"
  "${CLIPBANK_THIRD_PARTY_DIR}/sqlite"
)

target_link_libraries(clipbank_core
  PUBLIC sqlite3 Threads::Threads ${CMAKE_DL_LIBS} m
)

target_compile_options(clipbank_core PRIVATE -Wall -Wextra -Wpedantic)

target_compile_definitions(clipbank_core PUBLIC
  CLIPBANK_VERSION="${PROJECT_VERSION}"
)

# ------------------------------------------------------------------- binary
add_executable(clipbank src/main.cpp)
target_link_libraries(clipbank PRIVATE clipbank_core)
target_compile_options(clipbank PRIVATE -Wall -Wextra -Wpedantic)

# -------------------------------------------------------------------- tests
option(CLIPBANK_BUILD_TESTS "Build unit and integration tests" ON)

if(CLIPBANK_BUILD_TESTS)
  enable_testing()

  add_executable(clipbank_tests
    tests/test_main.cpp
    tests/test_units.cpp
  )
  target_link_libraries(clipbank_tests PRIVATE clipbank_core)
  target_include_directories(clipbank_tests PRIVATE
    "${CLIPBANK_THIRD_PARTY_DIR}"
  )
  target_compile_options(clipbank_tests PRIVATE -Wall -Wextra -Wpedantic)

  add_test(NAME unit COMMAND clipbank_tests)
  set_tests_properties(unit PROPERTIES LABELS "unit" TIMEOUT 120)

  add_test(NAME integration
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh" "$<TARGET_FILE:clipbank>"
  )
  set_tests_properties(integration PROPERTIES
    LABELS "integration" TIMEOUT 900
    ENVIRONMENT "CLIPBANK_TEST_KEEP=0"
  )

  add_test(NAME kill_resume
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/kill_resume.sh" "$<TARGET_FILE:clipbank>"
  )
  set_tests_properties(kill_resume PROPERTIES
    LABELS "integration" TIMEOUT 600
  )
endif()

install(TARGETS clipbank RUNTIME DESTINATION bin)
