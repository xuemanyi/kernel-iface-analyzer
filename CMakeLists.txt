cmake_minimum_required(VERSION 3.16)
project(kernel_interface_analyzer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using ClangConfig.cmake in: ${Clang_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CLANG_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_definitions(${LLVM_DEFINITIONS})

set(COMMON_SOURCES
    src/common/path_utils.cpp
    src/common/source_utils.cpp
)

set(COMMON_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Ubuntu/LLVM 安装里通常有 clang-cpp 聚合库；没有时回退到细分库
if (TARGET clang-cpp)
    set(CLANG_LIBS clang-cpp)
else()
    set(CLANG_LIBS
        clangTooling
        clangFrontend
        clangSerialization
        clangDriver
        clangParse
        clangSema
        clangAnalysis
        clangEdit
        clangAST
        clangASTMatchers
        clangLex
        clangBasic
    )
endif()

if (TARGET LLVM)
    set(LLVM_LIBS LLVM)
else()
    llvm_map_components_to_libnames(LLVM_LIBS Support Core Option)
endif()

add_executable(tu_indexer
    src/tu_indexer.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(tu_indexer PRIVATE ${CLANG_LIBS} ${LLVM_LIBS})

add_executable(function_extractor
    src/function_extractor.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(function_extractor PRIVATE ${CLANG_LIBS} ${LLVM_LIBS})

add_executable(call_extractor
    src/call_extractor.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(call_extractor PRIVATE ${CLANG_LIBS} ${LLVM_LIBS})

