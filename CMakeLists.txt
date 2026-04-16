cmake_minimum_required(VERSION 3.16)
project(danwebsocket VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Library ---

add_library(danwebsocket STATIC
    src/protocol/serializer.cpp
    src/protocol/codec.cpp
    src/protocol/stream_parser.cpp
    src/state/key_registry.cpp
    src/connection/heartbeat_manager.cpp
    src/connection/reconnect_engine.cpp
    src/api/topic_client_handle.cpp
    src/api/client.cpp
)

target_include_directories(danwebsocket PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Threading support (for heartbeat/reconnect timers)
find_package(Threads REQUIRED)
target_link_libraries(danwebsocket PUBLIC Threads::Threads)

# --- Tests ---

option(DANWS_BUILD_TESTS "Build tests" ON)

if(DANWS_BUILD_TESTS)
    enable_testing()

    add_executable(test_codec tests/test_codec.cpp)
    target_link_libraries(test_codec PRIVATE danwebsocket)
    add_test(NAME codec COMMAND test_codec)

    add_executable(test_stream_parser tests/test_stream_parser.cpp)
    target_link_libraries(test_stream_parser PRIVATE danwebsocket)
    add_test(NAME stream_parser COMMAND test_stream_parser)

    add_executable(test_key_registry tests/test_key_registry.cpp)
    target_link_libraries(test_key_registry PRIVATE danwebsocket)
    add_test(NAME key_registry COMMAND test_key_registry)
endif()

# --- Install ---

install(TARGETS danwebsocket
    EXPORT danwebsocket-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/danws
    DESTINATION include
)

install(EXPORT danwebsocket-targets
    FILE danwebsocket-config.cmake
    NAMESPACE danws::
    DESTINATION lib/cmake/danwebsocket
)
