cmake_minimum_required(VERSION 3.16)
project(sidp_host_tests LANGUAGES CXX)

enable_testing()
add_executable(test_sidp_session
    test_sidp_session.cpp
    ../sidp_session.cpp
    ../sidp_transport.cpp
)
target_include_directories(test_sidp_session PRIVATE host_stubs ../include)
set_target_properties(test_sidp_session PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS YES
)
target_compile_options(test_sidp_session PRIVATE -Wall -Wextra)
add_test(NAME sidp_session COMMAND test_sidp_session)

find_package(Threads REQUIRED)
add_executable(test_sidp_transport
    test_sidp_transport.cpp
    ../sidp_transport.cpp
    ../sidp_transport_queue.cpp
    ../sidp_transport_websocket.cpp
    ../sidp_transport_cdc.cpp
)
target_include_directories(test_sidp_transport PRIVATE host_stubs ../include)
set_target_properties(test_sidp_transport PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS YES)
target_compile_options(test_sidp_transport PRIVATE -Wall -Wextra -UNDEBUG)
target_link_libraries(test_sidp_transport PRIVATE Threads::Threads)
add_test(NAME sidp_transport COMMAND test_sidp_transport)
