# LevelDB 项目代码调研报告

## 1. 范围与结论

本文基于仓库当前源码基线 `bb74ef739973a70ca9f0d90788a1da89b788c0ad`（CMake 声明版本 `1.23.0`）梳理 LevelDB 的实现边界、模块职责与关键数据流。除另有说明外，文中的“当前实现”均指该基线，而不是早期设计说明中的预期方案。

LevelDB 是一个嵌入式、持久化、有序的字节串到字节串映射库。它采用 LSM Tree：新写入先追加到 WAL 并放入内存 MemTable，随后刷写为 SSTable；后台 compaction 持续归并重叠数据，以控制读放大和空间放大。它不提供 SQL、二级索引、网络服务或跨进程并发访问。

核心结论如下：

1. **正确性中心是 internal key 与 sequence number。** 一条逻辑记录的排序键为 `(user_key, sequence, type)`，其中 user key 升序、sequence 降序；因此最新版本排在前面，删除是一个普通的 tombstone，而 snapshot 只需固定一个 sequence number。
2. **写入路径以 WAL 为先，并通过单写者队列批量化。** 队首写者合并一组 `WriteBatch`，为其分配连续 sequence，再执行“追加日志 → 可选 Sync → 写入 MemTable”。同组请求共享结果和一次日志追加。
3. **读取路径优先内存、再查 LSM 各层。** L0 文件可重叠，可能需要按文件号从新到旧检查多个文件；L1 至 L6 的 key 范围不重叠，每层最多定位一个候选文件。SSTable 内部先查索引块，Bloom Filter 可以在数据块读取前否定不存在的键。
4. **后台工作同时承担 flush 和 compaction。** 只有一个 `DBImpl` 后台任务处于已调度/运行状态；它优先将 immutable MemTable 刷盘，再执行 size compaction 或 seek compaction。长时间持有 snapshot 会阻止旧版本和 tombstone 被安全清除。
5. **持久状态以 MANIFEST 为权威，WAL 用于恢复尚未生成 SSTable 的写入。** `CURRENT` 原子指向 MANIFEST；VersionEdit 记录文件增删、日志号、sequence 与 compaction 指针。恢复时先重建 Version，再回放仍有效的 WAL。

## 2. 工程定位、构建与目录

### 2.1 构建特征

| 项目 | 当前实现 |
| --- | --- |
| 构建系统 | CMake，最低版本 3.22，入口为 `CMakeLists.txt` |
| 语言标准 | 默认 C++17；C 默认设为 C11 但允许降级 |
| 平台适配 | `util/env_posix.cc` 或 `util/env_windows.cc`，由 CMake 按平台选择 |
| C++ 运行时约束 | 构建明确关闭异常和 RTTI |
| 可选依赖 | `crc32c`、Snappy、Zstd、tcmalloc，均通过库符号探测后链接 |
| 默认目标 | 静态/共享库 `leveldb`、工具 `leveldbutil`、单测与基准（均可通过选项关闭） |
| 测试依赖 | `third_party/googletest`；基准依赖 `third_party/benchmark` |

构建定义集中在 `CMakeLists.txt:7-44`、`CMakeLists.txt:120-286`。公开头文件位于 `include/leveldb/`；调用方只应依赖这一目录中的接口，`db/`、`table/`、`util/` 与 `port/` 均为内部实现细节。

### 2.2 模块地图

| 目录/文件 | 职责 | 关键入口 |
| --- | --- | --- |
| `include/leveldb/` | 稳定 C++/C API、Options、Env、Cache、Table | `db.h`、`options.h`、`env.h` |
| `db/` | DB 编排、WAL、MemTable、版本元数据、恢复与 compaction | `db_impl.cc`、`version_set.cc`、`memtable.cc` |
| `table/` | SSTable 构建、打开、块读取、索引、过滤器和迭代器 | `table_builder.cc`、`table.cc`、`block*.cc` |
| `util/` | Arena、LRU Cache、Bloom、编码、CRC、环境公共实现 | `cache.cc`、`bloom.cc`、`coding.cc` |
| `port/` | 平台同步原语、压缩适配、线程注解 | `port.h`、`port_stdcxx.h` |
| `helpers/memenv/` | 面向测试和嵌入场景的内存 Env | `memenv.cc` |
| `benchmarks/` | `db_bench` 等基准程序 | `db_bench.cc` |
| `doc/` | 用户指南、实现/日志/SSTable 格式说明 | `index.md`、`impl.md` |

## 3. 公开接口与使用语义

`DB` 的实现对象是 `DBImpl`。`DB` 明确承诺同一对象可被多个线程并发访问（`include/leveldb/db.h:42`），但 `Iterator`、`WriteBatch` 等单独对象的共享仍由调用方同步。

| API | 语义与实现落点 |
| --- | --- |
| `DB::Open` | 打开目录、加 `LOCK`、恢复 MANIFEST/WAL，并创建或复用当前 WAL/MemTable；见 `db/db_impl.cc:1503`。 |
| `Put` / `Delete` | 是 `Write` 的便捷封装，分别写入 value 或 deletion record。 |
| `Write` | 一个 `WriteBatch` 内的编辑按顺序原子应用；单写者队列还能把相邻请求合组；见 `db/db_impl.cc:1206`。 |
| `Get` | 读取一个明确或隐式 snapshot，依序查询内存和当前 Version；见 `db/db_impl.cc:1121`。 |
| `NewIterator` | 抓取 MemTable、immutable MemTable 与 Version 的引用，创建合并内部迭代器，再由 `DBIter` 隐藏内部版本。 |
| `GetSnapshot` / `ReleaseSnapshot` | 固定/释放一个 sequence number；未释放的最旧 snapshot 决定 compaction 可回收版本的下界。 |
| `CompactRange` | 先强制刷 MemTable，再逐层执行同步等待的手动 compaction。 |
| `DestroyDB` / `RepairDB` | 前者删除数据库文件；后者用于尽可能恢复损坏库，后者可能丢失数据。 |

### 3.1 重要 Options 与默认值

| 参数 | 默认值 | 直接影响 |
| --- | ---: | --- |
| `write_buffer_size` | 4 MiB | MemTable 达此近似内存量后切换，影响写吞吐、峰值内存与恢复时长。 |
| `max_open_files` | 1000 | TableCache 的文件表容量；内部预留 10 个非表文件句柄。 |
| `block_cache` | 空指针 | 空时 `DBImpl` 创建 8 MiB 内部 LRU；缓存的是解压后的数据块。 |
| `block_size` | 4 KiB | SSTable 数据块的未压缩目标大小，也是点读 I/O 粒度。 |
| `max_file_size` | 2 MiB | SSTable compaction 输出目标上限。 |
| `compression` | Snappy | 也支持 Zstd 和不压缩；压缩收益不足 12.5% 时当前块以原文存储。 |
| `filter_policy` | 空指针 | 配置后在 SSTable 中写入 filter meta block；通常使用 Bloom Filter。 |
| `ReadOptions::verify_checksums` | `false` | 该次读是否校验数据块 CRC。 |
| `ReadOptions::fill_cache` | `true` | 是否把本次读到的可缓存数据块放入 block cache。 |
| `WriteOptions::sync` | `false` | 成功返回前是否调用 `WritableFile::Sync()`；关闭时机器掉电可能丢失最近写入。 |

这些默认值定义在 `include/leveldb/options.h:83-185`。数据库建立后不能改变 comparator 的名称和排序语义；MANIFEST 恢复时会校验 comparator 名称，见 `db/version_set.cc:861`。

## 4. 总体架构与核心不变量

```text
应用线程
  │  Put / Delete / Write              Get / NewIterator
  ▼                                           │
DBImpl：writer queue、snapshot、VersionSet、后台调度 ──┐
  │                                                     │
  ├─ WAL (.log) ──► MemTable (Arena + SkipList)          │
  │                    │                                 │
  │                    └─ immutable MemTable ──flush─────┤
  │                                                      ▼
  └────────────────────────────────────────────► SSTable (.ldb/.sst)
                                                   L0（可重叠）
                                                   L1..L6（范围不重叠）
                                                        │
CURRENT ──► MANIFEST ──► VersionEdit（元数据提交） ◄────┘
```

### 4.1 Internal Key 与多版本可见性

每个 MemTable/SSTable 中保存的是 internal key，不是裸 user key：

```text
internal_key = user_key || fixed64((sequence_number << 8) | value_type)
value_type   = kTypeValue(1) | kTypeDeletion(0)
```

`sequence_number` 最高可用 56 位。`InternalKeyComparator` 先按用户 comparator 比较 user key；user key 相同则按 sequence **降序**比较。这让一次查找只要构造 `(user_key, snapshot_sequence, kTypeValue)` 作为 seek key，就能命中“该 snapshot 可见的最新版本”。删除不是立即物理删除，而是写入 tombstone，直到 compaction 证明它对所有仍存数据与仍存 snapshot 都不再需要。

对应定义和比较器在 `db/dbformat.h:54-183`；MemTable 实际编码在 `db/memtable.cc:76`。

### 4.2 内存与引用生命周期

- MemTable 使用 `Arena` 分配条目，并以 SkipList 按 internal key 排序。节点不会在 MemTable 生命周期内单独删除，因此在外层保证对象不析构时，读可以无锁遍历。
- 当前 MemTable、immutable MemTable 和 `Version` 都通过 `Ref/Unref` 延长生命周期。读路径在持锁时取得指针和引用，然后释放全局锁进行 I/O；迭代器通过 cleanup 回调归还引用。
- `VersionSet` 以一组不可变 `Version` 表示一个文件视图。新元数据提交生成并安装新 Version；仍被 iterator/compaction 引用的旧 Version 保留，避免读到被换代的文件清单。
- `pending_outputs_` 记录正在生成的 SSTable，文件清理会把它们视为 live，避免 compaction 输出尚未提交前被误删。

### 4.3 LSM 层级与阈值

固定层数为 7（L0 至 L6）。L0 以文件数量控制，L1 及以上以总字节数控制，容量按 10 倍增长。

| 层级 | 文件 key 范围 | 触发/容量规则 | 默认值 |
| --- | --- | --- | --- |
| L0 | 可重叠 | 开始 size compaction | 4 个文件 |
| L0 | 可重叠 | 写入软限速 | 8 个文件，每个写请求最多睡眠 1 ms |
| L0 | 可重叠 | 写入硬阻塞 | 12 个文件 |
| L1 | 不重叠且按 key 排序 | 总大小上限 | 10 MiB |
| L2 | 不重叠且按 key 排序 | 总大小上限 | 100 MiB |
| L3..L6 | 不重叠且按 key 排序 | 每升一层 ×10 | 1 GiB、10 GiB、100 GiB、1 TiB |

L0 阈值和最多将新的 MemTable 输出直接下推到 L2 的策略位于 `db/dbformat.h:25-45`。容量函数位于 `db/version_set.cc:20-49`。需要注意，`WriteLevel0Table()` 是历史命名：当前 `Version::PickLevelForMemTableOutput()` 在 L0/L1 均无重叠且 L2 重叠量可接受时，确实会把新表直接安放到 L1 或 L2（`db/version_set.cc:470-495`）。

## 5. 写入路径

### 5.1 从 API 到 MemTable

```text
Put/Delete
   │
   └─ DBImpl::Write
        ├─ 入 writers_ FIFO；仅队首执行提交
        ├─ MakeRoomForWrite：背压、必要时切换 WAL/MemTable
        ├─ BuildBatchGroup：合并相邻写请求
        ├─ 分配连续 sequence 区间
        ├─ WAL AddRecord(batch) → 可选 logfile Sync()
        ├─ WriteBatchInternal::InsertInto(mem_)
        └─ 唤醒已合组写者和新的队首
```

1. 每个请求包装为 `DBImpl::Writer` 并进入 `writers_`。非队首等待条件变量；这既串行化 MemTable 写入，也形成 group commit 的窗口。
2. 队首调用 `MakeRoomForWrite()`。若当前 MemTable 未满，继续；若已有 immutable MemTable 正在刷盘则等待；若 L0 文件达到 12 则等待。否则新建 WAL，把旧 `mem_` 变为 `imm_`，设置 `has_imm_`，并立即调度后台任务。
3. `BuildBatchGroup()` 合并后续请求。总上限通常为 1 MiB；若首批不超过 128 KiB，则限制为“首批大小 + 128 KiB”。`sync=true` 的请求不会并入一个 `sync=false` 首请求，防止弱持久化语义覆盖强请求。
4. 为整个合组分配从 `LastSequence()+1` 开始的连续 sequence；`WriteBatch` 中每条编辑依序消耗一个 sequence。
5. 持有队首责任但释放全局 mutex 后，先 `log_->AddRecord()`，再按首请求的 `sync` 选项同步文件，最后把批次解释为 `MemTable::Add()` 调用。只有前述步骤成功，才推进 `LastSequence`。
6. 若 `Sync()` 失败，日志落盘状态不确定，代码将该状态记录为 background error，后续写入失败，避免以未知 WAL 状态继续服务。

实现入口为 `db/db_impl.cc:1206-1405`；批格式与写入 MemTable 的解释器在 `db/write_batch.cc`，MemTable 的值格式在 `db/memtable.cc:76-132`。

### 5.2 WAL 格式与持久化边界

每个逻辑 WAL record 是一个序列化 `WriteBatch`：8 字节起始 sequence、4 字节条目数，后接 `Put`/`Delete` 记录。日志层再将它拆分成 32 KiB block 中的物理 record：`crc32c(type || data)`、16 位长度、类型和负载；大记录使用 `FIRST/MIDDLE/LAST` 片段。`log::Writer::AddRecord()` 和 `log::Reader::ReadRecord()` 分别实现写入与重组（`db/log_writer.cc:34`、`db/log_reader.cc:73`）。

写成功的持久化语义取决于 `WriteOptions::sync`：

- `sync=false`：WAL 已交给操作系统；进程异常通常可由 OS 缓存保留，机器掉电前的最近写入可能丢失。
- `sync=true`：在写完成前同步 WAL；这提高掉电恢复保证，但延迟由存储介质的 flush 能力主导。

WAL 没有压缩，数据块的 Snappy/Zstd 压缩仅用于 SSTable。

## 6. 读路径、迭代器与缓存

### 6.1 点查找

`DBImpl::Get()` 先在 mutex 保护下选定 snapshot，并对 `mem_`、`imm_`、当前 `Version` 加引用；随后解锁执行查找，最后根据读取统计决定是否触发 compaction。

查询顺序为：

1. **当前 MemTable**：SkipList seek 到目标 internal key；若命中 tombstone，立即返回 NotFound。
2. **immutable MemTable**：仅在 flush 尚未完成时存在，逻辑与当前 MemTable 相同。
3. **L0**：收集 user key 范围可能命中的全部文件，按文件号从新到旧查询，因为 L0 范围可以重叠。
4. **L1 至 L6**：每层文件范围互不重叠，通过二分定位一个候选文件；若命中 value 或 tombstone 即停止，否则继续下一层。

`Version::Get()` 的重叠选择与访问统计在 `db/version_set.cc:281-404`，顶层读逻辑在 `db/db_impl.cc:1121-1167`。如果一次读跨越多个文件，首个文件会被记为 seek compaction 候选；此外 iterator 约每 1 MiB 的读取量随机采样一次，也可能推动 seek compaction。

### 6.2 SSTable 内部一次 Get

```text
TableCache（文件句柄 + Table 对象）
  → index block 二分定位数据块句柄
  → 可选 filter block：确认“不可能存在”则提前返回
  → block cache 查找 (table cache_id, block offset)
  → 未命中则随机读块、按需校验 CRC、解压
  → 数据块 restart array 二分 + restart 区间顺序扫描
```

`Table::InternalGet()` 在索引块定位后先查询过滤器，再打开数据块迭代器（`table/table.cc:214`）。`TableCache` 使用文件号作为 cache key，缓存 `RandomAccessFile + Table` 对；block cache 的 key 则由 Table 专属 `cache_id` 和 block offset 构成，避免不同文件同 offset 碰撞。

默认的内部 LRU block cache 为 8 MiB；实现分成 16 个分片以降低锁竞争（`util/cache.cc:350-419`）。外部传入 cache 时，调用者拥有其生命周期。批量扫描应考虑 `fill_cache=false`，以避免冷数据驱逐热点块。

### 6.3 Iterator 与 snapshot

`NewInternalIterator()` 组合当前/immutable MemTable 迭代器、每个 L0 文件迭代器，以及 L1+ 每层的懒打开 concatenating iterator；随后 `MergingIterator` 按 internal key 合并。`DBIter` 再过滤：

- sequence 大于读取 snapshot 的版本不可见；
- 新值遮蔽同 user key 的旧值；
- tombstone 遮蔽旧值且不向用户输出；
- 反向遍历必须回扫同 user key 的版本，所以通常比正向遍历成本更高。

实现见 `db/db_impl.cc:1083`、`db/version_set.cc:229-245`、`table/merger.cc` 与 `db/db_iter.cc`。迭代器创建时持有 Version 和 MemTable 引用，故可以跨越后台 compaction 使用；但必须在销毁 DB 前销毁 iterator。

### 6.4 Bloom Filter

仅当提供 `filter_policy` 时，TableBuilder 才生成 filter block 并在 metaindex 中登记。内置 Bloom policy 的 probes 数约为 `bits_per_key × ln(2)`，限制在 1 到 30；默认推荐调用 `NewBloomFilterPolicy(10)`。它只能否定不存在，命中仍必须读取并验证数据块。

Bloom Filter 直接哈希内部 key，因此代码通过 `InternalFilterPolicy` 抹去 sequence/type，只把 user key 交给用户 policy。自定义 comparator 与 filter policy 的等价关系必须一致，否则可能产生错误的“否定命中”。实现位置为 `util/bloom.cc:17-90` 与 `db/dbformat.h` 中的 `InternalFilterPolicy`。

## 7. SSTable、MANIFEST 与文件清理

### 7.1 SSTable 格式

一个 `.ldb`（也兼容 `.sst` 扩展名）包含若干数据块、可选 meta 块、metaindex、index 和固定 48 字节 footer。每个块尾部有 1 字节 compression type 和 4 字节 CRC32C；footer 保存 metaindex/index 的 `(offset, size)` 与 magic number `0xdb4775248b80fb57`。

数据块中的 key 按序排列，使用前缀压缩：每条记录编码为 `shared_bytes`、`unshared_bytes`、`value_length`、key 后缀和 value。每 `block_restart_interval`（默认 16）条写入一个完整 key 的 restart point；读取时先二分 restart array，再在小区间内顺序解码。

| 组件 | 写入实现 | 读取实现 |
| --- | --- | --- |
| 数据块和 restart points | `table/block_builder.cc` | `table/block.cc` |
| 压缩、CRC、footer | `table/table_builder.cc` | `table/format.cc` |
| index/metaindex/filter 加载 | `table/table_builder.cc:213` | `table/table.cc:38-234` |
| filter 切片 | `table/filter_block.cc` | `table/filter_block.cc` |

TableBuilder 只有在压缩结果至少比原始块小 12.5% 时才保留 Snappy/Zstd 结果，否则把该块标记为不压缩。这样即使全局打开压缩，不可压缩数据也不会承担无效解压成本。

### 7.2 元数据提交

文件名和职责如下：

| 文件 | 内容与作用 |
| --- | --- |
| `000123.log` | 当前或待恢复 WAL。 |
| `000123.ldb` / `.sst` | 不可变 SSTable。 |
| `MANIFEST-000123` | VersionEdit 追加日志：comparator、文件增删、log number、next file number、last sequence、compaction pointer。 |
| `CURRENT` | 只包含当前 MANIFEST 文件名和换行；通过“写临时文件并 Sync → Rename”更新。 |
| `LOCK` | 进程级独占打开锁。 |
| `LOG` / `LOG.old` | 信息日志，不是 WAL。 |

`VersionSet::LogAndApply()` 先根据 VersionEdit 构造新 Version，再将 edit 写入 MANIFEST 并 Sync；若是新 MANIFEST，最后原子更新 `CURRENT`，成功后才安装新 Version（`db/version_set.cc:777-859`）。因此已提交的文件集合始终由 CURRENT 所指 MANIFEST 决定。

### 7.3 文件回收

`RemoveObsoleteFiles()` 汇总所有 live Version 引用的 SSTable、`pending_outputs_`、当前/上一个 WAL 与当前/更高编号 MANIFEST；再删除不在集合内的文件，并先从 TableCache 逐出要删的表。后台错误存在时不做回收，因为无法确认新 Version 是否已经持久化。实现位于 `db/db_impl.cc:225-289`。

## 8. Flush、Compaction 与恢复

### 8.1 Immutable MemTable flush

MemTable 满后，写线程把它转为 `imm_` 并立即启用新的 WAL/MemTable。后台任务优先把 `imm_` 迭代写入 SSTable，成功后用 VersionEdit 登记文件并将 `prev_log_number` 清零。只有 MANIFEST 提交成功，旧 MemTable/WAL 才可通过正常回收路径淘汰。

`WriteLevel0Table()` 生成文件时会将文件号加入 `pending_outputs_`，写表时释放 mutex，随后再登记 VersionEdit；`CompactMemTable()` 负责最终安装和清理（`db/db_impl.cc:505-575`）。

### 8.2 自动 compaction

`VersionSet::Finalize()` 计算每层 compaction score：L0 为文件数除以 4，其他层为当前字节数除以该层容量上限。`PickCompaction()` 优先选择 score ≥ 1 的 size compaction；若没有，再选择因多次 seek 标记的文件（`db/version_set.cc:1252`）。

对选中的层 L：

1. 选择一个输入文件，依赖 `compact_pointer_` 在 key 空间轮转；L0 会扩展为所有重叠 L0 文件。
2. 选择 L+1 的全部重叠文件，并补齐同 user key 的边界文件，防止同一个 user key 被不正确地拆分到本次 compaction 内外。
3. 在不增加 L+1 输入集且总输入不超过 `25 × max_file_size` 时，可扩展 L 输入集以摊薄开销。
4. 合并两个输入集，输出写入 L+1。输出文件达到 `max_file_size`（默认 2 MiB），或与 L+2 的重叠累计超过 `10 × max_file_size` 时切分。
5. 以新的 VersionEdit 原子删除输入文件、加入输出文件，然后回收旧文件。

当一个文件没有 L+1 重叠，且与 L+2 的重叠不超过阈值时，可只修改 MANIFEST 把该文件移动到下一层，无需重写数据（`db/version_set.cc:1499-1507`）。

### 8.3 版本与 tombstone 回收规则

在合并排序流中，同 user key 的记录按新到旧出现。若记录的 sequence 不大于仍存的最老 snapshot，并且之前已见过该 key 的新版本，则旧版本可以丢弃。对于 tombstone，除满足 snapshot 条件外，还必须确认更低层没有覆盖该 key 的文件，才可安全丢弃；否则 tombstone 仍需要遮蔽低层旧值。逻辑在 `DBImpl::DoCompactionWork()`（`db/db_impl.cc:898-1081`）。

这解释了两个运营现象：频繁更新相同 key 会暂时造成空间放大；遗忘释放 snapshot 会明显延迟旧值和删除标记回收。

### 8.4 打开与崩溃恢复

恢复流程为：

1. 创建目录（必要时）并获取 `LOCK`；处理 `create_if_missing` 与 `error_if_exists`。
2. 读取 `CURRENT` 指向的 MANIFEST，重放 VersionEdit，校验 comparator，恢复文件集合、日志号、文件号和最后 sequence。
3. 枚举目录确认 MANIFEST 引用的文件存在，找出编号不小于 MANIFEST `log_number` 的 WAL（以及历史兼容的 previous log），按编号升序回放。
4. WAL reader 始终校验 CRC；不完整的尾记录被忽略。回放中若临时 MemTable 超过写缓冲阈值，直接生成 SSTable。
5. 若可复用最后一个 WAL/MANIFEST 且 `reuse_logs=true`，保留追加；否则新建 WAL，并在需要时提交新的 MANIFEST edit。
6. 删除不再被引用的文件并按需调度 compaction。

流程主入口为 `DBImpl::Recover()` 和 `RecoverLogFile()`（`db/db_impl.cc:292-503`），MANIFEST 解析位于 `VersionSet::Recover()`（`db/version_set.cc:861-1014`）。

## 9. 并发、隔离与失败处理

| 主题 | 机制 | 结果 |
| --- | --- | --- |
| 进程互斥 | `Env::LockFile(LOCK)` | 同一数据库目录只能由一个进程打开。 |
| 写并发 | `mutex_` 保护 writer FIFO；队首批量提交 | 写入线性化，降低 WAL 追加与 Sync 次数。 |
| 读并发 | 持锁抓取引用后解锁；MemTable SkipList 无锁读 | 多个 Get/iterator 可并行读文件和内存。 |
| 读写隔离 | sequence + snapshot + immutable Version/MemTable | 每次读看到起始时刻或指定 snapshot 的一致视图。 |
| 文件 I/O | `Env` 抽象；随机读文件可并发使用 | 可注入自定义存储、限速或内存 Env。 |
| 后台任务 | `Env::Schedule` 执行；`DBImpl` 内部只挂起一个任务 | flush 优先于一般 compaction，任务完成后按需再调度。 |
| 后台错误 | `bg_error_` 记住首个错误并唤醒等待者 | 阻止后续压缩/写入；回收也停止，避免扩大不一致。 |

`Env::Schedule` 本身允许在不同线程并行执行多个工作项，但 `DBImpl::background_compaction_scheduled_` 防止同一 DB 同时调度多个后台 compaction。`TableCache` 自带同步；全局 `DBImpl::mutex_` 不跨越磁盘读、建表和大部分 compaction I/O 持有。

## 10. 调优、接入与运维要点

### 10.1 面向负载的配置建议

| 场景 | 重点配置 | 原因 |
| --- | --- | --- |
| 高吞吐批量导入 | 较大的 `write_buffer_size`、较大 `max_file_size`、`WriteBatch`、必要时 `sync=false` | 降低 MemTable 切换和 compaction 频率，但提高内存、恢复时间与单次 compaction 抖动。 |
| 随机点读、工作集超内存 | 合理增大 `block_cache`、设置 `NewBloomFilterPolicy(10)`、提高 `max_open_files` | 减少无效数据块 I/O、解压和文件反复打开。 |
| 大范围顺序扫描 | 视测量增大 `block_size`，扫描时 `fill_cache=false` | 增强顺序吞吐，避免污染热点 block cache。 |
| 严格掉电持久性 | 关键提交使用 `WriteOptions::sync=true`；可用批量分摊 Sync | 同步语义以 WAL 的 `Sync()` 成功为边界。 |
| 高频覆盖/删除 | 监控 L0 文件数、compaction 时间和磁盘余量；及时释放 snapshot | 覆盖与 tombstone 在 compaction 前都会保留多个版本。 |

实际参数应以 `db_bench` 和业务代表性负载测量决定。`max_file_size` 变大通常减少文件数，但会提高 compaction 的单次工作量和尾延迟；`write_buffer_size` 变大同样会加长崩溃恢复时 WAL 回放。

### 10.2 推荐监控项

- `leveldb.num-files-at-level<N>`：L0 接近 8/12 时是写入延迟和阻塞的直接先兆。
- `leveldb.stats`：关注每层 compaction 的耗时、读写字节和次数。
- `leveldb.sstables`：检查 key 范围、层级分布与是否出现异常重叠。
- `leveldb.approximate-memory-usage`：结合外部 block cache 容量评估内存预算。
- 信息日志 `LOG`：关注 `Too many L0 files`、background/compaction error、WAL/MANIFEST 恢复事件。
- 磁盘余量和 WAL Sync 延迟：compaction 需要临时输出空间，满盘或同步失败会将 DB 置入错误状态。

属性名称定义于 `include/leveldb/db.h:108-123`，实现见 `db/db_impl.cc:1407-1467`。

### 10.3 使用限制与风险

1. LevelDB 只有单进程目录锁，没有服务端协议和复制机制；多进程共享或分布式高可用必须由上层实现。
2. 没有事务、条件更新、范围删除和二级索引。`WriteBatch` 只保证同一批编辑的原子应用，并不替代读改写事务。
3. 自定义 comparator 一旦用于建库，排序行为与名称必须保持兼容；不兼容变更会破坏已有文件的顺序假设。
4. L0 文件过多会同时放大点查找和迭代合并成本，并最终阻塞写入；后台 compaction 吞吐需要长期不低于有效写放大后的写入速率。
5. snapshot/iterator 生命周期过长会保留 Version 和旧数据；客户端应使用 RAII，并尽早 `ReleaseSnapshot()`。
6. `RepairDB` 是最后恢复手段，不应当作普通启动流程；它的目标是尽可能找回可读数据，而非保持完整历史。

## 11. 测试、扩展与阅读路径

### 11.1 现有质量保障

CMake 默认构建聚合目标 `leveldb_tests`，涵盖 DB 行为、恢复、损坏处理、日志、版本编辑、SkipList、SSTable、Bloom、缓存、编码和 Arena；另有 C API 测试及平台 Env 测试。仓库中 `db/`、`table/`、`util/`、`helpers/memenv/` 下共有 13 个包含 `TEST(...)` 的测试源文件和 49 个测试声明。故障注入和历史 issue 测试文件存在，但部分未被默认聚合目标纳入，修改关键恢复/持久化逻辑时应明确评估并补充相应测试。

基准入口 `benchmarks/db_bench.cc` 可用于写入、随机读、顺序读、压缩和缓存配置的回归比较；它适合比较同一机器上的相对变化，不应直接把历史 README 性能数字视为当前环境的容量结论。

### 11.2 变更落点指南

| 需求 | 首选修改区域 | 必须同时关注 |
| --- | --- | --- |
| 新公开功能 | `include/leveldb/` + `db/db_impl.*` | API 稳定性、C API、单测、版本兼容性。 |
| 更改持久格式 | `db/dbformat.*`、`table/format.*`、`version_edit.*` | 已有数据兼容、CRC、恢复和 format 单测；持久枚举/tag 不可随意改号。 |
| 改变 compaction 策略 | `db/version_set.*`、`db/db_impl.cc` | snapshot 可见性、边界文件、pending outputs、文件回收。 |
| 新过滤器/缓存策略 | `include/leveldb/filter_policy.h`、`util/`、`table/` | comparator 兼容性、错误时“可能命中”的保守语义、内存归属。 |
| 新平台或存储介质 | `port/` 与 `Env` 实现 | 锁、原子 rename、随机读并发安全、Sync 语义、线程调度。 |

建议按以下顺序阅读源码：`include/leveldb/db.h` → `db/db_impl.cc` 的 `Write`/`Get` → `db/dbformat.h` → `db/memtable.cc` → `db/version_set.cc` → `table/table_builder.cc`/`table/table.cc` → `db/log_writer.cc`/`db/log_reader.cc`。这个顺序先建立用户可见行为，再追踪内部排序、落盘和恢复。

## 12. 关键源码索引

| 主题 | 主要代码位置 |
| --- | --- |
| 公开 DB 契约 | `include/leveldb/db.h` |
| 选项与默认值 | `include/leveldb/options.h` |
| DB 共享状态、锁与后台控制 | `db/db_impl.h` |
| 打开、恢复、写、读、flush、compaction | `db/db_impl.cc` |
| internal key、层数和阈值 | `db/dbformat.h`、`db/dbformat.cc` |
| WriteBatch 编码和写入 MemTable | `db/write_batch.cc` |
| MemTable/Arena/SkipList | `db/memtable.cc`、`db/skiplist.h`、`util/arena.*` |
| Version、MANIFEST、选取 compaction | `db/version_set.cc`、`db/version_edit.cc` |
| 文件命名和 CURRENT 原子更新 | `db/filename.cc` |
| WAL 分块和校验 | `db/log_format.h`、`db/log_writer.cc`、`db/log_reader.cc` |
| SSTable 格式、读写和缓存 | `table/format.*`、`table/table_builder.cc`、`table/table.cc`、`db/table_cache.cc` |
| Block/过滤器/合并迭代器 | `table/block*`、`table/filter_block.*`、`table/merger.cc` |
| LRU、Bloom、OS 抽象 | `util/cache.cc`、`util/bloom.cc`、`include/leveldb/env.h` |
