cmake_minimum_required(VERSION 3.16)
project(flashfs VERSION 0.1.0 DESCRIPTION "Read-only reader for embedded flash filesystems: JFFS2, cramfs and UBI" LANGUAGES C)

option(FLASHFS_BUILD_TESTS "Build tests" OFF)
option(FLASHFS_BUILD_EXAMPLES "Build examples" OFF)
option(FLASHFS_BUILD_FUZZ "Build libFuzzer targets" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(flashfs src/flashfs.c)
add_library(flashfs::flashfs ALIAS flashfs)
target_include_directories(flashfs PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(flashfs PROPERTIES
    C_STANDARD 99
    C_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

set(PC_CFLAGS "")
if(BUILD_SHARED_LIBS)
    target_compile_definitions(flashfs PRIVATE MTDF_BUILD_SHARED INTERFACE MTDF_SHARED)
    set(PC_CFLAGS "-DMTDF_SHARED")
endif()

install(TARGETS flashfs EXPORT flashfsTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT flashfsTargets
    NAMESPACE flashfs::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flashfs)

configure_package_config_file(cmake/flashfsConfig.cmake.in
    ${PROJECT_BINARY_DIR}/flashfsConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flashfs)
write_basic_package_version_file(${PROJECT_BINARY_DIR}/flashfsConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${PROJECT_BINARY_DIR}/flashfsConfig.cmake
    ${PROJECT_BINARY_DIR}/flashfsConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flashfs)

configure_file(flashfs.pc.in ${PROJECT_BINARY_DIR}/flashfs.pc @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/flashfs.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(FLASHFS_BUILD_TESTS)
    enable_testing()
    add_executable(test_flashfs tests/test_flashfs.c)
    target_link_libraries(test_flashfs PRIVATE flashfs)
    add_test(NAME flashfs COMMAND test_flashfs)
    add_library(flashfs_header_check OBJECT tests/header_check.c)
    target_include_directories(flashfs_header_check PRIVATE include)
endif()

if(FLASHFS_BUILD_EXAMPLES)
    add_executable(flashfs_basic examples/basic.c)
    target_link_libraries(flashfs_basic PRIVATE flashfs)
endif()

if(FLASHFS_BUILD_FUZZ)
    add_executable(fuzz_split fuzz/fuzz_split.c)
    target_link_libraries(fuzz_split PRIVATE flashfs)
    target_link_options(fuzz_split PRIVATE -fsanitize=fuzzer)
endif()
