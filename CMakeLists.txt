cmake_minimum_required(VERSION 3.21)

project(cppMailfs VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MAILFS_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
)
FetchContent_MakeAvailable(mbedtls)

FetchContent_Declare(
  sqlite_amalgamation
  URL https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip
)
FetchContent_MakeAvailable(sqlite_amalgamation)

add_library(sqlite3_vendor STATIC
  ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c
)

target_include_directories(sqlite3_vendor PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
target_compile_definitions(sqlite3_vendor PUBLIC SQLITE_ENABLE_JSON1 SQLITE_THREADSAFE=1)

add_library(mailfs_core
  src/application/mailfs_service.cpp
  src/core/app_config.cpp
  src/core/hash.cpp
  src/core/mail_block_metadata.cpp
  src/core/mime_message.cpp
  src/core/xor_codec.cpp
  src/infra/config/json_config_loader.cpp
  src/infra/imap/imap_client.cpp
  src/infra/imap/imap_response_parser.cpp
  src/infra/net/secure_socket.cpp
  src/infra/platform/utf8.cpp
  src/infra/storage/sqlite_cache_repository.cpp
)

target_include_directories(mailfs_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(mailfs_core
  PUBLIC
    nlohmann_json::nlohmann_json
    mbedtls
    mbedx509
    mbedcrypto
    sqlite3_vendor
)

if(MSVC)
  target_compile_options(mailfs_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(mailfs_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(mailfs_cli
  src/app/cli/main.cpp
)

target_link_libraries(mailfs_cli PRIVATE mailfs_core)

if(MAILFS_BUILD_TESTS)
  include(CTest)
  enable_testing()

  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(mailfs_tests
    tests/config_loader_test.cpp
    tests/imap_response_parser_test.cpp
    tests/mail_block_metadata_test.cpp
    tests/mailfs_service_test.cpp
    tests/mime_message_test.cpp
    tests/sqlite_cache_repository_test.cpp
    tests/utf8_test.cpp
  )

  target_link_libraries(mailfs_tests
    PRIVATE
      GTest::gtest_main
      mailfs_core
  )

  include(GoogleTest)
  gtest_discover_tests(mailfs_tests)
endif()
