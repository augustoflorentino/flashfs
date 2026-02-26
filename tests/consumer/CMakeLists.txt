cmake_minimum_required(VERSION 3.16)
project(flashfs_consumer LANGUAGES C)

find_package(flashfs REQUIRED)

add_executable(consumer ${CMAKE_CURRENT_SOURCE_DIR}/../../examples/basic.c)
target_link_libraries(consumer PRIVATE flashfs::flashfs)
