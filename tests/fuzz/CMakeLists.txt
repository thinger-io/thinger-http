function(add_fuzz_target name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE thinger::http)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

add_fuzz_target(fuzz_http_request_parser fuzz_http_request_parser.cpp)
add_fuzz_target(fuzz_http_response_parser fuzz_http_response_parser.cpp)
add_fuzz_target(fuzz_chunked_decoder fuzz_chunked_decoder.cpp)
# Differential against Boost.Beast (header-only, used only by the fuzz targets as a reference)
add_fuzz_target(fuzz_http_differential fuzz_http_differential.cpp)
add_fuzz_target(fuzz_url_decode fuzz_url_decode.cpp)
add_fuzz_target(fuzz_utf8_validate fuzz_utf8_validate.cpp)
add_fuzz_target(fuzz_body_decoders fuzz_body_decoders.cpp)

# llhttp (Node.js's HTTP parser, MIT), the reference of the three-way differential target. It
# is fetched and built only here, never linked into the library. Its release tags carry the
# generated C sources (src/ and include/), so no code generation is needed; it is built as a
# static library with the fuzzing flags (instrumented, so the fuzzer explores its states too)
# instead of with its own CMakeLists.txt.
include(FetchContent)
FetchContent_Declare(
    llhttp
    URL https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.4.3.tar.gz
    URL_HASH SHA256=1eb813c7437b31a87496a1cd3ed79f00746720f5e7e29c79b42c02cb69f36c39
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _none
)
FetchContent_MakeAvailable(llhttp)
add_library(fuzz_llhttp STATIC
    ${llhttp_SOURCE_DIR}/src/api.c
    ${llhttp_SOURCE_DIR}/src/http.c
    ${llhttp_SOURCE_DIR}/src/llhttp.c
)
target_include_directories(fuzz_llhttp SYSTEM PUBLIC ${llhttp_SOURCE_DIR}/include)

# Three-way differentials: llhttp as the reference, Beast as a third opinion (requests, and
# responses as the client reads them)
add_fuzz_target(fuzz_http_differential3 fuzz_http_differential3.cpp)
target_link_libraries(fuzz_http_differential3 PRIVATE fuzz_llhttp)
add_fuzz_target(fuzz_http_response_differential fuzz_http_response_differential.cpp)
target_link_libraries(fuzz_http_response_differential PRIVATE fuzz_llhttp)
