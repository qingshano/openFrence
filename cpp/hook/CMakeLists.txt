cmake_minimum_required(VERSION 3.20)
project(openfences_hook LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
add_definitions(-DUNICODE -D_UNICODE)

add_library(openfences_hook SHARED
    dllmain.cpp
    exports.def
)

target_link_libraries(openfences_hook PRIVATE user32 shlwapi)
