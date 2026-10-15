cmake_minimum_required(VERSION 3.16)
project(kasin_httpd LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # needs GNU extensions (memmem, accept4, ...)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The compiler checks the C rules of CLAUDE.md it can: no information lost by an implicit
# conversion, no hidden shadowing, every function declared before use and const kept.
set(KASIN_WARNINGS
  -Wall -Wextra -Wpedantic
  -Wconversion -Wsign-conversion -Wshadow
  -Wmissing-prototypes -Wstrict-prototypes -Wcast-qual)

# Memory is reserved once at startup from these values:
#   total ~= threads * (conns_per_worker * (req_buf + resp_buf + sizeof(Conn))
#                       + big_bufs_per_worker * big_buf) + db_heap
set(KASIN_MAX_CONNS_PER_WORKER 512      CACHE STRING "Connection slots per worker thread")
set(KASIN_REQ_BUF_SIZE         8192     CACHE STRING "Request buffer bytes per connection (head + body)")
set(KASIN_RESP_BUF_SIZE        32768    CACHE STRING "Response buffer bytes per connection")
set(KASIN_BIG_BUF_SIZE         2097152  CACHE STRING "Large buffer bytes, lent for uploads and long recipes")
set(KASIN_BIG_BUFS_PER_WORKER  2        CACHE STRING "Large buffers per worker thread")
set(KASIN_DB_HEAP_SIZE         33554432 CACHE STRING "SQLite heap bytes (64 times a power of two)")

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

add_executable(kasin-httpd
  src/main.c
  src/arena.c
  src/buddy.c
  src/db.c
  src/geoip.c
  src/ip.c
  src/jwt.c
  src/login_guard.c
  src/migrate.c
  src/password.c
  src/sha256.c
  src/sql.c
  src/str.c)
target_compile_options(kasin-httpd PRIVATE ${KASIN_WARNINGS})
target_compile_definitions(kasin-httpd PRIVATE
  MAX_CONNS_PER_WORKER=${KASIN_MAX_CONNS_PER_WORKER}
  REQ_BUF_SIZE=${KASIN_REQ_BUF_SIZE}
  RESP_BUF_SIZE=${KASIN_RESP_BUF_SIZE}
  BIG_BUF_SIZE=${KASIN_BIG_BUF_SIZE}
  BIG_BUFS_PER_WORKER=${KASIN_BIG_BUFS_PER_WORKER}
  DB_HEAP_SIZE=${KASIN_DB_HEAP_SIZE})
target_link_libraries(kasin-httpd PRIVATE Threads::Threads SQLite::SQLite3)

# Makes the admin password hash for KASIN_ADMIN_PASSWORD_HASH.
add_executable(kasin-passwd
  src/passwd.c
  src/password.c
  src/sha256.c
  src/str.c)
target_compile_options(kasin-passwd PRIVATE ${KASIN_WARNINGS})

# Makes the server's country file from DB-IP's CSV (see scripts/fetch-geoip).
add_executable(kasin-geoip
  src/geoip_tool.c
  src/geoip_build.c
  src/str.c)
target_compile_options(kasin-geoip PRIVATE ${KASIN_WARNINGS})

# Build the bundles in public/js and public/css from the sources in js/ and css/
# (see js/bundles.txt and css/bundles.txt). bundler.c is what they share.
add_executable(kasin-jsbundle
  src/jsbundle.c
  src/jsmin.c
  src/bundler.c
  src/arena.c
  src/str.c)
target_compile_options(kasin-jsbundle PRIVATE ${KASIN_WARNINGS})

add_executable(kasin-cssbundle
  src/cssbundle.c
  src/cssmin.c
  src/bundler.c
  src/arena.c
  src/str.c)
target_compile_options(kasin-cssbundle PRIVATE ${KASIN_WARNINGS})

enable_testing()
add_executable(jsmin-test
  tests/jsmin_test.c
  src/jsmin.c
  src/str.c)
target_compile_options(jsmin-test PRIVATE ${KASIN_WARNINGS})
add_test(NAME jsmin COMMAND jsmin-test)

add_executable(cssmin-test
  tests/cssmin_test.c
  src/cssmin.c
  src/str.c)
target_compile_options(cssmin-test PRIVATE ${KASIN_WARNINGS})
add_test(NAME cssmin COMMAND cssmin-test)

add_executable(password-test
  tests/password_test.c
  src/password.c
  src/sha256.c
  src/str.c)
target_compile_options(password-test PRIVATE ${KASIN_WARNINGS})
add_test(NAME password COMMAND password-test)

add_executable(login-guard-test
  tests/login_guard_test.c
  src/login_guard.c
  src/ip.c
  src/str.c)
target_compile_options(login-guard-test PRIVATE ${KASIN_WARNINGS})
target_link_libraries(login-guard-test PRIVATE Threads::Threads)
add_test(NAME login-guard COMMAND login-guard-test)

add_executable(geoip-test
  tests/geoip_test.c
  src/geoip.c
  src/geoip_build.c
  src/ip.c
  src/str.c)
target_compile_options(geoip-test PRIVATE ${KASIN_WARNINGS})
add_test(NAME geoip COMMAND geoip-test)
