cmake_minimum_required(VERSION 3.16)
project(tison VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(TISON_IS_TOP_LEVEL ON)
else()
    set(TISON_IS_TOP_LEVEL OFF)
endif()

option(TISON_TYPEINFO_CODEGEN
       "Build typeinfo_metaprogram and define tison_typeinfo_codegen() (requires libclang)" ON)
option(TISON_BUILD_TESTS "Build tison tests (requires TISON_TYPEINFO_CODEGEN)" ${TISON_IS_TOP_LEVEL})
option(TISON_BUILD_EXAMPLES "Build tison examples (requires TISON_TYPEINFO_CODEGEN)"
       ${TISON_IS_TOP_LEVEL})
option(TISON_SANITIZE "Build the library and its consumers with ASan and UBSan" OFF)
option(TISON_INSTALL "Generate install rules and the CMake package" ${TISON_IS_TOP_LEVEL})

set(TISON_WARNINGS $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Werror=switch>)

# TODO: support BUILD_SHARED_LIBS. Windows needs symbol export annotations first.
add_library(tison STATIC src/tison.c)
add_library(tison::tison ALIAS tison)

target_compile_features(tison PUBLIC c_std_99)
target_include_directories(tison PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/extern/typeinfo>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/tison>
)
target_compile_options(tison PRIVATE ${TISON_WARNINGS})
if(UNIX)
    target_link_libraries(tison PUBLIC m)
endif()

if(TISON_SANITIZE)
    set(TISON_SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    # Build-only, so sanitizer flags never leak into the installed package.
    target_compile_options(tison PUBLIC $<BUILD_INTERFACE:-O1 ${TISON_SANITIZE_FLAGS}>)
    target_link_options(tison PUBLIC $<BUILD_INTERFACE:${TISON_SANITIZE_FLAGS}>)
endif()

add_library(tison_extlib STATIC src/tison_extlib.c)
add_library(tison::extlib ALIAS tison_extlib)
set_target_properties(tison_extlib PROPERTIES EXPORT_NAME extlib)
# extlib.h is not installed: installed consumers add their own copy's include path.
target_include_directories(tison_extlib PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/extern>)
target_link_libraries(tison_extlib PUBLIC tison)
target_compile_options(tison_extlib PRIVATE ${TISON_WARNINGS})

if((TISON_BUILD_TESTS OR TISON_BUILD_EXAMPLES) AND NOT TISON_TYPEINFO_CODEGEN)
    message(FATAL_ERROR "TISON_BUILD_TESTS and TISON_BUILD_EXAMPLES require TISON_TYPEINFO_CODEGEN")
endif()

if(TISON_TYPEINFO_CODEGEN)
    find_library(LIBCLANG_LIBRARY NAMES clang libclang REQUIRED)
    find_path(LIBCLANG_INCLUDE_DIR NAMES clang-c/Index.h REQUIRED)
    execute_process(
        COMMAND clang -print-resource-dir
        OUTPUT_VARIABLE CLANG_RESOURCE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _clang_result
    )
    if(NOT _clang_result EQUAL 0)
        message(FATAL_ERROR "Could not determine clang resource directory. Is clang installed?")
    endif()

    set(TISON_CLANG_INCLUDE_DIR ${CLANG_RESOURCE_DIR}/include CACHE INTERNAL "")
    set(TISON_TYPEINFO_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/extern/typeinfo CACHE INTERNAL "")
    set(TISON_METAPROGRAM typeinfo_metaprogram CACHE INTERNAL "")

    add_executable(typeinfo_metaprogram extern/typeinfo/typeinfo_metaprogram.c)
    add_executable(tison::typeinfo_metaprogram ALIAS typeinfo_metaprogram)
    target_include_directories(typeinfo_metaprogram PRIVATE extern ${LIBCLANG_INCLUDE_DIR})
    target_link_libraries(typeinfo_metaprogram PRIVATE ${LIBCLANG_LIBRARY})
    # libclang often lives outside the default linker path (e.g. /usr/lib/llvm-18/lib), and
    # install strips the build RPATH.
    set_target_properties(typeinfo_metaprogram PROPERTIES INSTALL_RPATH_USE_LINK_PATH ON)

    include(cmake/tisonCodegen.cmake)
endif()

if(TISON_BUILD_TESTS)
    add_subdirectory(test)

    if(NOT TISON_SANITIZE)
        set(TISON_ASAN_CC clang CACHE STRING "Compiler used by the test-asan target")
        set(TISON_ASAN_DIR ${CMAKE_BINARY_DIR}/asan)
        add_custom_target(test-asan
            COMMAND ${CMAKE_COMMAND} -S ${PROJECT_SOURCE_DIR} -B ${TISON_ASAN_DIR}
                -G ${CMAKE_GENERATOR}
                -DCMAKE_C_COMPILER=${TISON_ASAN_CC}
                -DCMAKE_BUILD_TYPE=Debug
                -DTISON_SANITIZE=ON
                -DTISON_BUILD_EXAMPLES=OFF
            COMMAND ${CMAKE_COMMAND} --build ${TISON_ASAN_DIR} --target test
            USES_TERMINAL
            VERBATIM
        )
    endif()
endif()

if(TISON_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(TISON_INSTALL)
    set(TISON_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/tison)

    set(TISON_INSTALL_TARGETS tison tison_extlib)
    if(TISON_TYPEINFO_CODEGEN)
        list(APPEND TISON_INSTALL_TARGETS typeinfo_metaprogram)
    endif()
    install(TARGETS ${TISON_INSTALL_TARGETS} EXPORT tisonTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    # typeinfo.h goes next to tison.h, which includes it with quotes.
    install(FILES include/tison.h include/tison_extlib.h extern/typeinfo/typeinfo.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tison
    )
    install(EXPORT tisonTargets NAMESPACE tison:: DESTINATION ${TISON_CMAKE_DIR})

    configure_package_config_file(cmake/tisonConfig.cmake.in
        ${PROJECT_BINARY_DIR}/tisonConfig.cmake
        INSTALL_DESTINATION ${TISON_CMAKE_DIR}
        PATH_VARS CMAKE_INSTALL_INCLUDEDIR
    )
    # Below 1.0 a minor bump may break the API, so only patch releases are compatible.
    write_basic_package_version_file(${PROJECT_BINARY_DIR}/tisonConfigVersion.cmake
        COMPATIBILITY SameMinorVersion
    )
    install(FILES
        ${PROJECT_BINARY_DIR}/tisonConfig.cmake
        ${PROJECT_BINARY_DIR}/tisonConfigVersion.cmake
        cmake/tisonCodegen.cmake
        DESTINATION ${TISON_CMAKE_DIR}
    )

    # The .pc files locate the prefix relative to themselves, so `cmake --install --prefix`
    # works without reconfiguring.
    file(RELATIVE_PATH TISON_PC_PREFIX /${CMAKE_INSTALL_LIBDIR}/pkgconfig /)
    string(REGEX REPLACE "/$" "" TISON_PC_PREFIX "${TISON_PC_PREFIX}")
    configure_file(cmake/tison.pc.in ${PROJECT_BINARY_DIR}/tison.pc @ONLY)
    configure_file(cmake/tison-extlib.pc.in ${PROJECT_BINARY_DIR}/tison-extlib.pc @ONLY)
    install(FILES ${PROJECT_BINARY_DIR}/tison.pc ${PROJECT_BINARY_DIR}/tison-extlib.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
