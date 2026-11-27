foreach(example config inventory reviews)
    tison_typeinfo_codegen(${example}_types.h -I${PROJECT_SOURCE_DIR}/extern)
    add_executable(${example} ${example}.c ${example}_types_typeinfo.c)
    target_include_directories(${example} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${example} PRIVATE ${TISON_WARNINGS})
    target_link_libraries(${example} PRIVATE tison)
endforeach()

target_link_libraries(reviews PRIVATE tison_extlib)

add_custom_target(examples DEPENDS config inventory reviews)
add_custom_target(run-examples
    COMMAND config ${CMAKE_CURRENT_BINARY_DIR}/config.out.json
    COMMAND inventory
    COMMAND reviews
    DEPENDS examples
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
