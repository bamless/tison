tison_typeinfo_codegen(test_types.h -I${PROJECT_SOURCE_DIR}/extern)

add_executable(tison_test
    extlib_impl.c
    test_types_typeinfo.c
    test_main.c
    test_scanner.c
    test_deserialize.c
    test_serialize.c
    test_extlib.c
)
if(TISON_SANITIZE)
    set_source_files_properties(test_main.c PROPERTIES COMPILE_OPTIONS -fno-sanitize=address)
endif()
target_include_directories(tison_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tison_test PRIVATE tison_extlib)
target_compile_options(tison_test PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Werror=switch -Wno-attributes -Wno-pragmas -Wno-unused-function>
)

add_custom_target(test
    COMMAND tison_test
    DEPENDS tison_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)
