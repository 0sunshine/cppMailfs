cmake_minimum_required(VERSION 3.22.1)

project(mailfs_android LANGUAGES C CXX)

set(MAILFS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../..")
set(MAILFS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MAILFS_BUILD_CLI OFF CACHE BOOL "" FORCE)

add_subdirectory("${MAILFS_REPO_ROOT}" "${CMAKE_BINARY_DIR}/mailfs_core")

add_library(mailfs_jni SHARED
  mailfs_jni.cpp
)

target_compile_features(mailfs_jni PRIVATE cxx_std_17)
target_link_libraries(mailfs_jni
  PRIVATE
    mailfs_core
    android
    log
)
