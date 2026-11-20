cmake_minimum_required(VERSION 3.24)

# Buildable standalone (it has no Jolt/PhysX/sqlite dependency) or as a
# subdirectory of the top-level Homeworldz project.
if(NOT DEFINED PROJECT_NAME)
  project(homeworldz-script LANGUAGES CXX)
  enable_testing()
endif()

add_library(homeworldz-script
  src/lexer.cpp
  src/parser.cpp
  src/compiler.cpp
  src/library.cpp
  src/vm.cpp)
target_include_directories(homeworldz-script PUBLIC include)
target_compile_features(homeworldz-script PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(homeworldz-script PRIVATE /W4 /permissive-)
else()
  target_compile_options(homeworldz-script PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(homeworldz-script-poc-tests tests/script_poc_test.cpp)
target_link_libraries(homeworldz-script-poc-tests PRIVATE homeworldz-script)
add_test(NAME script-poc COMMAND homeworldz-script-poc-tests)

# How much of LSL Falcon runs: the semantics checks and Tailslide's corpus,
# compared with the record of what passed before (tests/compat/*.txt).
add_executable(homeworldz-script-compat-tests
  tests/compat/compat_test.cpp
  tests/compat/semantics.cpp)
target_link_libraries(homeworldz-script-compat-tests PRIVATE homeworldz-script)
if(MSVC)
  # The RSA test keys are long string literals.
  target_compile_options(homeworldz-script-compat-tests PRIVATE /W4 /permissive-)
endif()
add_test(NAME script-compat COMMAND homeworldz-script-compat-tests
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/tailslide-tests/scripts
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/compat)
