function(add_fuzz_target name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE thinger::http)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

add_fuzz_target(fuzz_http_request_parser fuzz_http_request_parser.cpp)
add_fuzz_target(fuzz_url_decode fuzz_url_decode.cpp)
