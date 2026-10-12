cmake_minimum_required(VERSION 3.16)
project(kasin_httpd LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # needs GNU extensions (memmem, accept4, ...)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Memory is reserved once at startup from these values:
#   total ~= threads * conns_per_worker * (req_buf + resp_buf + sizeof(Conn))
set(KASIN_MAX_CONNS_PER_WORKER 512   CACHE STRING "Connection slots per worker thread")
set(KASIN_REQ_BUF_SIZE         8192  CACHE STRING "Request buffer bytes per connection")
set(KASIN_RESP_BUF_SIZE        32768 CACHE STRING "Response buffer bytes per connection")

find_package(Threads REQUIRED)

add_executable(kasin-httpd src/main.c src/arena.c src/str.c)
target_compile_options(kasin-httpd PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(kasin-httpd PRIVATE
  MAX_CONNS_PER_WORKER=${KASIN_MAX_CONNS_PER_WORKER}
  REQ_BUF_SIZE=${KASIN_REQ_BUF_SIZE}
  RESP_BUF_SIZE=${KASIN_RESP_BUF_SIZE})
target_link_libraries(kasin-httpd PRIVATE Threads::Threads)
