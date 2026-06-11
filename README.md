# JiaoMiaoDB

AI Native Database · v2.1.0

一个用 C++17 编写的单节点内存数据库，支持自然语言查询和传统 SQL，包含完整的事务子系统（多级锁、RC/RR/SSI 隔离、Savepoint、VACUUM、XID 回卷保护）。存储层已沿 RocksDB 路线重构为 LSM 风格（MemTable + SkipList）。

## 特性

- **完整 ACID**: MVCC + WAL + Checkpoint + 崩溃恢复
- **多隔离级**: Read Committed / Repeatable Read / Serializable (SSI 2-cycle)
- **Savepoint / 子事务**: 嵌套支持，部分回滚
- **多级锁**: Spinlock / LWLock / RegularLock，表级并发
- **LSM MemTable (Phase 1)**: 二进制 Tuple + SkipList 内存表，append-only + MVCC 多版本共存
- **VACUUM**: 物理清理 + Frozen XID 冻结
- **XID 回卷保护**: 自动冻结 + 紧急停机
- **WAL v2**: 格式版本化 + 截断 + v1 向后兼容
- **Catalog 命名空间**: database → schema → table 层级
- **AI NL→SQL**: 集成 LLM，schema 上下文注入
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

## 文档

完整技术文档位于 `~/mywork/db-agent-memory/skills/jmdb/`：
- `jmdb_manual.md` — 全量参考手册
- `jmdb_transaction.md` — 事务子系统
- `jmdb_storage.md` — 存储子系统现状讲解
- `jmdb_concurrency.md` — 并发控制深度讲解
- `jmdb_distributed.md` — 分布式演进路线

## 测试覆盖

132 单元测试 (v0.3.0 / LSM Phase 1, 全部通过，含 1 个 Savepoint 3 层嵌套压力测试 100 轮)
