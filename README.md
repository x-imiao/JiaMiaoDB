# JiaMiaoDB

AI Native Database · v1.0

一个用 C++17 编写的单节点内存数据库。其设计参考 PostgreSQL 的内核语义（多版本并发控制、SSI 串行化快照隔离、Savepoint、XID 回卷保护、Checkpoint 事务感知）与 RocksDB 的存储层架构（MemTable + 二进制 WAL + Pugh 真并发跳表 + Arena 内存池），并把大语言模型作为一等公民接入协议层，提供自然语言到 SQL 的端到端查询链路。全栈自实现，仅依赖 libcurl。

## 特性

- **完整 ACID**: MVCC + WAL + Checkpoint + 崩溃恢复
- **多隔离级**: Read Committed / Repeatable Read / Serializable (SSI 2-cycle)
- **Savepoint / 子事务**: 嵌套支持，部分回滚
- **多级锁**: Spinlock / LWLock / RegularLock，表级并发
- **LSM MemTable (Phase 2)**: Pugh 真并发 SkipList (SWMR lock-free 读) + Arena bump allocator (64KB 块) + 二进制 Tuple
- **VACUUM**: 物理清理 + Frozen XID 冻结
- **XID 回卷保护**: 自动冻结 + 紧急停机
- **WAL v3**: length-prefix binary + CRC32 校验 + 可选 fsync
- **Checkpoint v3**: 8B magic + 强类型 Row/Index，体积可比 JSON 缩 5-10x
- **Catalog/CLog 二进制**: 嵌入 Checkpoint v3 段，启动无 JSON 反序列化
- **Catalog 命名空间**: database → schema → table 层级
- **AI NL→SQL**: 集成 LLM，schema 上下文注入
- **PG 风格 Memory Context**: `jmalloc` / `jmfree` + AllocSet/Bump context，5 个热点转换
- **零外部依赖**: 除 libcurl 外全部自实现

## 构建

```bash
./build.sh
```

产物在 `bld/` 目录。

## 运行测试

```bash
cd bld && ctest --output-on-failure
# 或: ./bld/jiamiaodb_test
```

## 测试覆盖

188 单元测试，全部通过：
- 二进制 codec 专项: 9 (O-1) + 9 (O-2) + 8 (O-3) + 9 (O-4)
- SQL 功能门禁: 13 (CRUD / 事务 / WHERE / 多表 / 数据类型 / NULL / PK 冲突 / 持久化 / 并发 / 索引)
- 事务子系统: 多隔离级、Savepoint 嵌套压力、SSI pivot、VACUUM、XID 回卷
- LSM 存储: SkipList 1W+4R 100k 并发、Arena、MemTable flush
- 崩溃恢复: Checkpoint + WAL replay 端到端
- 其他: 锁管理、Parser、NL→SQL、配置加载
