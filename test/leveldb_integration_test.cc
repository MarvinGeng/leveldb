// Copyright 2026 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/write_batch.h"

namespace leveldb {
namespace {

class TestContext {
 public:
  explicit TestContext(const char* name) : name_(name) {}

  bool Check(bool condition, const char* expression, const char* file, int line,
             const std::string& detail = std::string()) {
    if (condition) return true;

    ++failures_;
    std::cerr << name_ << ": " << file << ":" << line << ": " << expression;
    if (!detail.empty()) {
      std::cerr << " (" << detail << ")";
    }
    std::cerr << std::endl;
    return false;
  }

  int failures() const { return failures_; }

 private:
  const char* const name_;
  int failures_ = 0;
};

#define TEST_CHECK(context, expression)                                     \
  do {                                                                      \
    if (!(context)->Check((expression), #expression, __FILE__, __LINE__)) { \
      return;                                                               \
    }                                                                       \
  } while (false)

#define TEST_CHECK_EQ(context, expected, actual)                           \
  do {                                                                     \
    const auto expected_value = (expected);                                \
    const auto actual_value = (actual);                                    \
    if (!(context)->Check(expected_value == actual_value,                  \
                          #expected " == " #actual, __FILE__, __LINE__)) { \
      return;                                                              \
    }                                                                      \
  } while (false)

#define TEST_CHECK_OK(context, expression)                              \
  do {                                                                  \
    const ::leveldb::Status status = (expression);                      \
    if (!(context)->Check(status.ok(), #expression, __FILE__, __LINE__, \
                          status.ToString())) {                         \
      return;                                                           \
    }                                                                   \
  } while (false)

std::string KeyFor(int worker, int index) {
  std::ostringstream stream;
  stream << "worker-" << worker << "-" << index;
  return stream.str();
}

class SnapshotHandle {
 public:
  SnapshotHandle(DB* db, const Snapshot* snapshot)
      : db_(db), snapshot_(snapshot) {}
  ~SnapshotHandle() {
    if (snapshot_ != nullptr) db_->ReleaseSnapshot(snapshot_);
  }

  SnapshotHandle(const SnapshotHandle&) = delete;
  SnapshotHandle& operator=(const SnapshotHandle&) = delete;

  const Snapshot* get() const { return snapshot_; }

 private:
  DB* const db_;
  const Snapshot* const snapshot_;
};

class DatabaseFixture {
 public:
  DatabaseFixture() = default;

  DatabaseFixture(const DatabaseFixture&) = delete;
  DatabaseFixture& operator=(const DatabaseFixture&) = delete;

  ~DatabaseFixture() {
    db_.reset();
    if (!dbname_.empty()) {
      const Status status = DestroyDB(dbname_, options_);
      if (context_ != nullptr) {
        context_->Check(status.ok(), "DestroyDB(dbname_, options_)", __FILE__,
                        __LINE__, status.ToString());
      }
    }
  }

  bool SetUp(TestContext* context) {
    context_ = context;
    std::string test_directory;
    Status status = Env::Default()->GetTestDirectory(&test_directory);
    if (!context->Check(status.ok(), "Env::Default()->GetTestDirectory()",
                        __FILE__, __LINE__, status.ToString())) {
      return false;
    }

    dbname_ = test_directory + "/leveldb_integration_" +
              std::to_string(Env::Default()->NowMicros()) + "_" +
              std::to_string(next_database_id_.fetch_add(1));
    options_.create_if_missing = true;
    status = DestroyDB(dbname_, options_);
    if (!context->Check(status.ok(), "DestroyDB(dbname_, options_)", __FILE__,
                        __LINE__, status.ToString())) {
      return false;
    }
    return Open(context);
  }

  bool Open(TestContext* context) {
    DB* db = nullptr;
    const Status status = DB::Open(options_, dbname_, &db);
    if (!context->Check(status.ok(), "DB::Open(options_, dbname_, &db)",
                        __FILE__, __LINE__, status.ToString())) {
      return false;
    }
    db_.reset(db);
    return true;
  }

  bool Destroy(TestContext* context) {
    db_.reset();
    const Status status = DestroyDB(dbname_, options_);
    return context->Check(status.ok(), "DestroyDB(dbname_, options_)", __FILE__,
                          __LINE__, status.ToString());
  }

  bool ExpectValue(TestContext* context, const ReadOptions& read_options,
                   const Slice& key, const std::string& expected) const {
    std::string value;
    const Status status = db_->Get(read_options, key, &value);
    if (!context->Check(status.ok(), "db_->Get(read_options, key, &value)",
                        __FILE__, __LINE__, status.ToString())) {
      return false;
    }
    return context->Check(value == expected, "value == expected", __FILE__,
                          __LINE__);
  }

  bool ExpectNotFound(TestContext* context, const ReadOptions& read_options,
                      const Slice& key) const {
    std::string value;
    const Status status = db_->Get(read_options, key, &value);
    return context->Check(status.IsNotFound(), "status.IsNotFound()", __FILE__,
                          __LINE__, status.ToString());
  }

  DB* db() const { return db_.get(); }
  void Close() { db_.reset(); }
  const std::string& dbname() const { return dbname_; }
  Options& options() { return options_; }

 private:
  static std::atomic<uint64_t> next_database_id_;

  TestContext* context_ = nullptr;
  Options options_;
  std::string dbname_;
  std::unique_ptr<DB> db_;
};

std::atomic<uint64_t> DatabaseFixture::next_database_id_{0};

void TestDatabaseLifecycleAndBasicCrud(TestContext* context) {
  DatabaseFixture fixture;
  if (!fixture.SetUp(context)) return;

  if (!fixture.Destroy(context)) return;
  Options missing_options;
  DB* unopened = nullptr;
  Status status = DB::Open(missing_options, fixture.dbname(), &unopened);
  TEST_CHECK(context, status.IsInvalidArgument());
  TEST_CHECK_EQ(context, nullptr, unopened);

  if (!fixture.Open(context)) return;
  WriteOptions durable_write;
  durable_write.sync = true;
  TEST_CHECK_OK(context, fixture.db()->Put(durable_write, "account:1", "open"));
  if (!fixture.ExpectValue(context, ReadOptions(), "account:1", "open")) {
    return;
  }

  TEST_CHECK_OK(context,
                fixture.db()->Put(WriteOptions(), "account:1", "closed"));
  if (!fixture.ExpectValue(context, ReadOptions(), "account:1", "closed")) {
    return;
  }

  TEST_CHECK_OK(context, fixture.db()->Delete(WriteOptions(), "account:1"));
  if (!fixture.ExpectNotFound(context, ReadOptions(), "account:1")) return;

  fixture.Close();
  Options existing_options;
  existing_options.error_if_exists = true;
  DB* existing = nullptr;
  status = DB::Open(existing_options, fixture.dbname(), &existing);
  TEST_CHECK(context, status.IsInvalidArgument());
  TEST_CHECK_EQ(context, nullptr, existing);
}

void TestWriteBatchSnapshotsAndOrderedIteration(TestContext* context) {
  DatabaseFixture fixture;
  if (!fixture.SetUp(context)) return;

  WriteBatch batch;
  batch.Put("beta", "v1");
  batch.Put("alpha", "a1");
  batch.Put("gamma", "g1");
  batch.Put("beta", "v2");
  batch.Put("removed", "temporary");
  batch.Delete("removed");
  TEST_CHECK_OK(context, fixture.db()->Write(WriteOptions(), &batch));

  if (!fixture.ExpectValue(context, ReadOptions(), "alpha", "a1") ||
      !fixture.ExpectValue(context, ReadOptions(), "beta", "v2") ||
      !fixture.ExpectValue(context, ReadOptions(), "gamma", "g1") ||
      !fixture.ExpectNotFound(context, ReadOptions(), "removed")) {
    return;
  }

  SnapshotHandle snapshot(fixture.db(), fixture.db()->GetSnapshot());
  TEST_CHECK(context, snapshot.get() != nullptr);
  TEST_CHECK_OK(context, fixture.db()->Put(WriteOptions(), "beta", "v3"));
  TEST_CHECK_OK(context, fixture.db()->Delete(WriteOptions(), "alpha"));
  TEST_CHECK_OK(context, fixture.db()->Put(WriteOptions(), "delta", "d1"));

  ReadOptions snapshot_options;
  snapshot_options.snapshot = snapshot.get();
  if (!fixture.ExpectValue(context, snapshot_options, "alpha", "a1") ||
      !fixture.ExpectValue(context, snapshot_options, "beta", "v2") ||
      !fixture.ExpectValue(context, snapshot_options, "gamma", "g1") ||
      !fixture.ExpectNotFound(context, ReadOptions(), "alpha") ||
      !fixture.ExpectValue(context, ReadOptions(), "beta", "v3") ||
      !fixture.ExpectValue(context, ReadOptions(), "delta", "d1")) {
    return;
  }

  std::unique_ptr<Iterator> iterator(fixture.db()->NewIterator(ReadOptions()));
  TEST_CHECK(context, iterator != nullptr);
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"beta", "v3"}, {"delta", "d1"}, {"gamma", "g1"}};

  size_t position = 0;
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
    TEST_CHECK(context, position < expected.size());
    TEST_CHECK_EQ(context, expected[position].first,
                  iterator->key().ToString());
    TEST_CHECK_EQ(context, expected[position].second,
                  iterator->value().ToString());
    ++position;
  }
  TEST_CHECK_EQ(context, expected.size(), position);
  TEST_CHECK(context, iterator->status().ok());

  iterator->Seek("c");
  TEST_CHECK(context, iterator->Valid());
  TEST_CHECK_EQ(context, std::string("delta"), iterator->key().ToString());

  iterator->SeekToLast();
  for (size_t index = expected.size(); index > 0; --index) {
    TEST_CHECK(context, iterator->Valid());
    TEST_CHECK_EQ(context, expected[index - 1].first,
                  iterator->key().ToString());
    iterator->Prev();
  }
  TEST_CHECK(context, !iterator->Valid());
  TEST_CHECK(context, iterator->status().ok());
}

void TestBinaryKeysAndConcurrentWriters(TestContext* context) {
  DatabaseFixture fixture;
  if (!fixture.SetUp(context)) return;

  const std::string binary_key("bin\0key", 7);
  const std::string binary_value("value\0data", 10);
  TEST_CHECK_OK(context,
                fixture.db()->Put(WriteOptions(), binary_key, binary_value));
  if (!fixture.ExpectValue(context, ReadOptions(), binary_key, binary_value)) {
    return;
  }

  constexpr int kWriterCount = 4;
  constexpr int kWritesPerWriter = 64;
  std::atomic<int> ready_writers{0};
  std::atomic<bool> start{false};
  std::vector<std::string> errors(kWriterCount);
  std::vector<std::thread> writers;
  writers.reserve(kWriterCount);

  for (int worker = 0; worker < kWriterCount; ++worker) {
    writers.emplace_back([&, worker] {
      ready_writers.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int index = 0; index < kWritesPerWriter; ++index) {
        const Status status = fixture.db()->Put(
            WriteOptions(), KeyFor(worker, index),
            "value-" + std::to_string(worker) + "-" + std::to_string(index));
        if (!status.ok()) {
          errors[worker] = status.ToString();
          return;
        }
      }
    });
  }

  while (ready_writers.load(std::memory_order_acquire) != kWriterCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (std::thread& writer : writers) {
    writer.join();
  }

  for (int worker = 0; worker < kWriterCount; ++worker) {
    TEST_CHECK(context, errors[worker].empty());
    for (int index = 0; index < kWritesPerWriter; ++index) {
      if (!fixture.ExpectValue(context, ReadOptions(), KeyFor(worker, index),
                               "value-" + std::to_string(worker) + "-" +
                                   std::to_string(index))) {
        return;
      }
    }
  }
}

void TestCompactionPropertiesApproximateSizesAndReopen(TestContext* context) {
  DatabaseFixture fixture;
  if (!fixture.SetUp(context)) return;
  if (!fixture.Destroy(context)) return;

  std::unique_ptr<Cache> cache(NewLRUCache(2 << 20));
  std::unique_ptr<const FilterPolicy> filter(NewBloomFilterPolicy(10));
  Options compact_options;
  compact_options.create_if_missing = true;
  compact_options.write_buffer_size = 64 << 10;
  compact_options.block_size = 1024;
  compact_options.compression = kNoCompression;
  compact_options.block_cache = cache.get();
  compact_options.filter_policy = filter.get();

  DB* compact_db = nullptr;
  Status status = DB::Open(compact_options, fixture.dbname(), &compact_db);
  TEST_CHECK(context, status.ok());
  std::unique_ptr<DB> compact_db_owner(compact_db);

  for (int index = 0; index < 96; ++index) {
    std::ostringstream key;
    key << "record-" << index;
    const std::string value(4096, static_cast<char>('a' + (index % 26)));
    TEST_CHECK_OK(context, compact_db->Put(WriteOptions(), key.str(), value));
  }

  compact_db->CompactRange(nullptr, nullptr);
  ReadOptions checked_read;
  checked_read.verify_checksums = true;
  checked_read.fill_cache = false;
  std::string checked_value;
  TEST_CHECK_OK(context,
                compact_db->Get(checked_read, "record-42", &checked_value));
  TEST_CHECK_EQ(context, std::string(4096, 'q'), checked_value);

  std::string property;
  TEST_CHECK(context, compact_db->GetProperty("leveldb.stats", &property));
  TEST_CHECK(context, !property.empty());
  TEST_CHECK(context, compact_db->GetProperty("leveldb.sstables", &property));
  TEST_CHECK(context,
             !compact_db->GetProperty("leveldb.no-such-property", &property));

  const Range range("record-0", "record-z");
  uint64_t size = 0;
  compact_db->GetApproximateSizes(&range, 1, &size);
  TEST_CHECK(context, size > 0);

  compact_db_owner.reset();
  compact_options.create_if_missing = false;
  compact_db = nullptr;
  status = DB::Open(compact_options, fixture.dbname(), &compact_db);
  TEST_CHECK(context, status.ok());
  compact_db_owner.reset(compact_db);

  std::string recovered_value;
  TEST_CHECK_OK(context,
                compact_db->Get(checked_read, "record-95", &recovered_value));
  TEST_CHECK_EQ(context, std::string(4096, 'r'), recovered_value);
}

struct TestCase {
  const char* name;
  void (*function)(TestContext*);
};

}  // namespace
}  // namespace leveldb

int main() {
  const leveldb::TestCase tests[] = {
      {"DatabaseLifecycleAndBasicCrud",
       &leveldb::TestDatabaseLifecycleAndBasicCrud},
      {"WriteBatchSnapshotsAndOrderedIteration",
       &leveldb::TestWriteBatchSnapshotsAndOrderedIteration},
      {"BinaryKeysAndConcurrentWriters",
       &leveldb::TestBinaryKeysAndConcurrentWriters},
      {"CompactionPropertiesApproximateSizesAndReopen",
       &leveldb::TestCompactionPropertiesApproximateSizesAndReopen},
  };

  int failed_tests = 0;
  for (const leveldb::TestCase& test : tests) {
    leveldb::TestContext context(test.name);
    test.function(&context);
    if (context.failures() == 0) {
      std::cout << "[PASS] " << test.name << std::endl;
    } else {
      ++failed_tests;
      std::cerr << "[FAIL] " << test.name << std::endl;
    }
  }

  std::cout << "LevelDB integration suite: "
            << (failed_tests == 0 ? "PASS" : "FAIL") << std::endl;
  return failed_tests == 0 ? 0 : 1;
}
