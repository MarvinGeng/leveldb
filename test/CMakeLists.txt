# Public API integration tests. Keep these independent from db/, table/, util/
# internals and third-party test frameworks so they also serve as standalone
# downstream-usage regression tests.
add_executable(leveldb_integration_test
  "leveldb_integration_test.cc"
)

target_link_libraries(leveldb_integration_test
  PRIVATE
    leveldb
)

add_test(NAME "leveldb_integration_test" COMMAND "leveldb_integration_test")
