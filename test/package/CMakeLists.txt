# Smoke test for the installed package, built against an install prefix rather than this tree:
#   cmake -S test/package -B build-pkg -DCMAKE_PREFIX_PATH="<prefix>;<llvm prefix>"
cmake_minimum_required(VERSION 3.16)
project(tison_package_test C)

find_package(tison 0.1 REQUIRED)

tison_typeinfo_codegen(player.h)

add_executable(player main.c player_typeinfo.c)
target_include_directories(player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(player PRIVATE tison::tison)
