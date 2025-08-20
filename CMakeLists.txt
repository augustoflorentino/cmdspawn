cmake_minimum_required(VERSION 3.16)
project(cmdspawn VERSION 0.2.0 DESCRIPTION "Run programs portably on POSIX and Windows: no shell, captured output, time limits" LANGUAGES C)

option(CMDSPAWN_BUILD_TESTS "Build tests" OFF)
option(CMDSPAWN_BUILD_EXAMPLES "Build examples" OFF)
option(CMDSPAWN_BUILD_FUZZ "Build libFuzzer targets" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(cmdspawn src/spawn.c src/spawn_posix.c src/spawn_win.c)
add_library(cmdspawn::cmdspawn ALIAS cmdspawn)
target_include_directories(cmdspawn PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(cmdspawn PROPERTIES
    C_STANDARD 99
    C_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

set(PC_CFLAGS "")
if(BUILD_SHARED_LIBS)
    target_compile_definitions(cmdspawn PRIVATE SPW_BUILD_SHARED INTERFACE SPW_SHARED)
    set(PC_CFLAGS "-DSPW_SHARED")
endif()

install(TARGETS cmdspawn EXPORT cmdspawnTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT cmdspawnTargets
    NAMESPACE cmdspawn::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cmdspawn)

configure_package_config_file(cmake/cmdspawnConfig.cmake.in
    ${PROJECT_BINARY_DIR}/cmdspawnConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cmdspawn)
write_basic_package_version_file(${PROJECT_BINARY_DIR}/cmdspawnConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${PROJECT_BINARY_DIR}/cmdspawnConfig.cmake
    ${PROJECT_BINARY_DIR}/cmdspawnConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cmdspawn)

configure_file(cmdspawn.pc.in ${PROJECT_BINARY_DIR}/cmdspawn.pc @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/cmdspawn.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(CMDSPAWN_BUILD_TESTS)
    enable_testing()
    add_executable(spw_helper tests/helper.c)
    add_executable(test_spawn tests/test_spawn.c)
    target_link_libraries(test_spawn PRIVATE cmdspawn)
    if(WIN32)
        target_link_libraries(spw_helper PRIVATE shell32)
        target_link_options(spw_helper PRIVATE -municode)
        target_link_libraries(test_spawn PRIVATE shell32)
    endif()
    add_test(NAME spawn COMMAND test_spawn $<TARGET_FILE:spw_helper>)
    add_library(spawn_header_check OBJECT tests/header_check.c)
    target_include_directories(spawn_header_check PRIVATE include)
endif()

if(CMDSPAWN_BUILD_EXAMPLES)
    foreach(example basic hook poll_loop)
        add_executable(cmdspawn_${example} examples/${example}.c)
        target_link_libraries(cmdspawn_${example} PRIVATE cmdspawn)
    endforeach()
endif()

if(CMDSPAWN_BUILD_FUZZ)
    add_executable(fuzz_win_quote fuzz/fuzz_win_quote.c)
    target_link_libraries(fuzz_win_quote PRIVATE cmdspawn)
    target_include_directories(fuzz_win_quote PRIVATE tests)
    target_link_options(fuzz_win_quote PRIVATE -fsanitize=fuzzer)
endif()
