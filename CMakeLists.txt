cmake_minimum_required(VERSION 3.10)
project(hermes_flight_comm C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

include_directories(include third_party)

set(CORE_SOURCES
    third_party/aes.c
    third_party/sha256.c
    src/audio.c
    src/crypto.c
    src/tmr_vote.c
    src/mesh_net.c
    src/node.c
)

add_library(hermes_core ${CORE_SOURCES})
target_link_libraries(hermes_core m)

add_executable(hermes_demo src/main.c)
target_link_libraries(hermes_demo hermes_core m)

enable_testing()
add_executable(test_hermes tests/test_hermes.c)
target_link_libraries(test_hermes hermes_core m)
add_test(NAME unit_tests COMMAND test_hermes)