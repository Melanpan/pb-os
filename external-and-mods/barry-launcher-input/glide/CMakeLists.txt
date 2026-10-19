# barryglide: glide typing, suggestions and autocorrect (barry_glide.py's)
# as a plain C++20 static library, its words in a mapped read-only blob.
# Not part of Barry.Input's build yet; build.sh builds it and its tools.
cmake_minimum_required(VERSION 3.21)
project(barryglide LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(barryglide STATIC
  unicode.cpp unicode.h unicode_tables.inc
  json.cpp json.h
  blob.cpp blob.h
  glide.cpp glide.h
  text.h
)
target_include_directories(barryglide PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Each operation rounded as Python rounds it: no fused multiply-adds.
target_compile_options(barryglide PRIVATE -Wall -Wextra -Werror -ffp-contract=off)

add_executable(glide-cli tools/glide_cli.cpp)
target_link_libraries(glide-cli PRIVATE barryglide)
target_compile_options(glide-cli PRIVATE -Wall -Wextra -Werror)

add_executable(glide-blob tools/glide_blob.cpp)
target_link_libraries(glide-blob PRIVATE barryglide)
target_compile_options(glide-blob PRIVATE -Wall -Wextra -Werror)
