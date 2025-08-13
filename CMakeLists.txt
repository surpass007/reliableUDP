cmake_minimum_required(VERSION 3.10)
project(my_uv_server C CXX)

set(CMAKE_CXX_STANDARD 11)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -g")

include_directories(
    ${CMAKE_SOURCE_DIR}/../libev/build/include
)

add_executable(
    tcp_server
    
    ${CMAKE_SOURCE_DIR}/tcp_echo_server.cpp
)

target_link_libraries(
    tcp_server

    ${CMAKE_SOURCE_DIR}/../libev/build/lib/libev.a
)

add_executable(
    udp_server
    
    ${CMAKE_SOURCE_DIR}/udp_server.cpp
)

target_link_libraries(
    udp_server

    ${CMAKE_SOURCE_DIR}/../libev/build/lib/libev.a
)

add_executable(
    udp_client
    
    ${CMAKE_SOURCE_DIR}/udp_client.cpp
)

target_link_libraries(
    udp_client

    ${CMAKE_SOURCE_DIR}/../libev/build/lib/libev.a
)

add_executable(
    udp_client_sr
    
    ${CMAKE_SOURCE_DIR}/udp_client_sr.cpp
)

target_link_libraries(
    udp_client_sr

    ${CMAKE_SOURCE_DIR}/../libev/build/lib/libev.a
)

add_executable(
    udp_server_sr
    
    ${CMAKE_SOURCE_DIR}/udp_server_sr.cpp
)

target_link_libraries(
    udp_server_sr

    ${CMAKE_SOURCE_DIR}/../libev/build/lib/libev.a
)