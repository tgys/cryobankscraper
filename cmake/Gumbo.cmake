# Builds google/gumbo-parser v0.10.1 as a static library (upstream has no CMake).
include(FetchContent)

FetchContent_Declare(
  gumbo_parser
  GIT_REPOSITORY https://github.com/google/gumbo-parser.git
  GIT_TAG v0.10.1
)

FetchContent_GetProperties(gumbo_parser)
if(NOT gumbo_parser_POPULATED)
  FetchContent_Populate(gumbo_parser)
endif()

set(_gumbo_src "${gumbo_parser_SOURCE_DIR}/src")
add_library(scrapeff_gumbo STATIC
  "${_gumbo_src}/attribute.c"
  "${_gumbo_src}/char_ref.c"
  "${_gumbo_src}/error.c"
  "${_gumbo_src}/parser.c"
  "${_gumbo_src}/string_buffer.c"
  "${_gumbo_src}/string_piece.c"
  "${_gumbo_src}/tag.c"
  "${_gumbo_src}/tokenizer.c"
  "${_gumbo_src}/utf8.c"
  "${_gumbo_src}/util.c"
  "${_gumbo_src}/vector.c"
)
target_include_directories(scrapeff_gumbo PUBLIC "${_gumbo_src}")
