cmake_minimum_required(VERSION 3.16)
project(flashfs VERSION 0.1.0 DESCRIPTION "Read-only reader for embedded flash filesystems: JFFS2, cramfs and UBI" LANGUAGES C)

option(FLASHFS_BUILD_TESTS "Build tests" OFF)
option(FLASHFS_BUILD_EXAMPLES "Build examples" OFF)
option(FLASHFS_BUILD_FUZZ "Build libFuzzer targets" OFF)
option(FLASHFS_WITH_ZLIB "Decode zlib (JFFS2, cramfs) when zlib is found" ON)
option(FLASHFS_WITH_LZMA "Decode JFFS2 lzma when liblzma is found" ON)
option(FLASHFS_WITH_LZO "Decode JFFS2 lzo when liblzo2 is found" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(FLASHFS_SOURCES src/flashfs.c src/source.c src/tree.c src/decode.c src/jffs2.c src/cramfs.c src/ubi.c)
add_library(flashfs ${FLASHFS_SOURCES})
add_library(flashfs::flashfs ALIAS flashfs)

set(FLASHFS_CODECS "")
set(PC_REQUIRES "")
set(PC_LIBS "")
if(FLASHFS_WITH_ZLIB)
    find_package(ZLIB)
    if(ZLIB_FOUND)
        target_link_libraries(flashfs PRIVATE ZLIB::ZLIB)
        target_compile_definitions(flashfs PRIVATE MTDF_HAVE_ZLIB)
        list(APPEND FLASHFS_CODECS zlib)
        string(APPEND PC_REQUIRES " zlib")
    endif()
endif()
if(FLASHFS_WITH_LZMA)
    find_package(LibLZMA)
    if(LIBLZMA_FOUND)
        target_link_libraries(flashfs PRIVATE LibLZMA::LibLZMA)
        target_compile_definitions(flashfs PRIVATE MTDF_HAVE_LZMA)
        list(APPEND FLASHFS_CODECS lzma)
        string(APPEND PC_REQUIRES " liblzma")
    endif()
endif()
if(FLASHFS_WITH_LZO)
    find_path(LZO2_INCLUDE_DIR lzo/lzo1x.h)
    find_library(LZO2_LIBRARY NAMES lzo2)
    if(LZO2_INCLUDE_DIR AND LZO2_LIBRARY)
        target_include_directories(flashfs PRIVATE ${LZO2_INCLUDE_DIR})
        target_link_libraries(flashfs PRIVATE ${LZO2_LIBRARY})
        target_compile_definitions(flashfs PRIVATE MTDF_HAVE_LZO)
        list(APPEND FLASHFS_CODECS lzo)
        string(APPEND PC_LIBS " -llzo2")
    endif()
endif()
message(STATUS "flashfs codecs: ${FLASHFS_CODECS}")
target_include_directories(flashfs PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(flashfs PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
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
    add_library(flashfs_header_check OBJECT tests/header_check.c)
    target_include_directories(flashfs_header_check PRIVATE include)

    add_executable(test_flashfs tests/test_flashfs.c)
    target_link_libraries(test_flashfs PRIVATE flashfs)
    add_test(NAME flashfs COMMAND test_flashfs ${PROJECT_SOURCE_DIR}/tests/data)
    add_executable(test_robust tests/test_robust.c)
    target_link_libraries(test_robust PRIVATE flashfs)
    add_test(NAME robust COMMAND test_robust ${PROJECT_SOURCE_DIR}/tests/data)

    file(GLOB FLASHFS_PARITY_IMAGES ${PROJECT_SOURCE_DIR}/tests/parity/data/*.img)
    list(JOIN FLASHFS_PARITY_IMAGES "\n" FLASHFS_PARITY_LIST)
    file(WRITE ${PROJECT_BINARY_DIR}/parity.list "${FLASHFS_PARITY_LIST}\n")
    add_executable(test_parity tests/parity/test_parity.c)
    target_link_libraries(test_parity PRIVATE flashfs)
    add_test(NAME parity COMMAND test_parity ${PROJECT_BINARY_DIR}/parity.list)
endif()

if(FLASHFS_BUILD_EXAMPLES)
    foreach(example mtdf-ls mtdf-extract)
        add_executable(${example} examples/${example}.c)
        target_link_libraries(${example} PRIVATE flashfs)
    endforeach()
endif()

if(FLASHFS_BUILD_FUZZ)
    add_executable(fuzz_split fuzz/fuzz_split.c)
    target_link_libraries(fuzz_split PRIVATE flashfs)
    target_link_options(fuzz_split PRIVATE -fsanitize=fuzzer)
endif()
