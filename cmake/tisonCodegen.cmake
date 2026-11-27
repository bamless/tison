# tison_typeinfo_codegen(<header> [<metaprogram args>...])
# Generates <header base>_typeinfo.{c,h} next to the header. Extra arguments, such as -I paths
# or -no-builtin-types, are forwarded to the metaprogram.
#
# Shared by the source tree and the installed package. Both set TISON_METAPROGRAM (a target),
# TISON_TYPEINFO_INCLUDE_DIR and TISON_CLANG_INCLUDE_DIR before including this file.
function(tison_typeinfo_codegen header)
    get_filename_component(header ${header} ABSOLUTE)
    get_filename_component(dir ${header} DIRECTORY)
    get_filename_component(name ${header} NAME_WE)
    add_custom_command(
        OUTPUT ${dir}/${name}_typeinfo.c ${dir}/${name}_typeinfo.h
        COMMAND ${TISON_METAPROGRAM}
            -I${TISON_TYPEINFO_INCLUDE_DIR}
            -I${TISON_CLANG_INCLUDE_DIR}
            ${ARGN}
            ${header}
            -o ${dir}/${name}_typeinfo
        DEPENDS ${TISON_METAPROGRAM} ${header}
        COMMENT "Generating typeinfo for ${name}"
        VERBATIM
    )
endfunction()
