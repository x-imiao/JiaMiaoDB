# JiaMiaoDB

AI Native Database · v2.3.0

一个用 C++17 编写的单节点内存数据库，支持自然语言查询和传统 SQL，包含完整的事务子系统（多级锁、RC/RR/SSI 隔离、Savepoint、VACUUM、XID 回卷保护）。存储层已沿 RocksDB 路线重构为 LSM 风格（真并发 SkipList + Arena bump allocator + 二进制 WAL/Checkpoint/Catalog/CLog），启动路径无 JSON 词法分析开销。

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

## 二进制化路线（O-1 ~ O-6, 已完成）

| 阶段 | 范围 | 收益 |
|------|------|------|
| O-1 | WAL record `data` 字段 JSON→二进制 (`wal_payload`) | replay 路径去掉 `json::parse` 词法分析 + AST 构建 |
| O-2 | Checkpoint 文件 JSON→二进制 (`checkpoint.bin` + 强类型 Row/Index) | 启动快 5-10x, 体积收缩 5-10x |
| O-3 | Catalog 快照 JSON→二进制 (`catalog_encode`) | Checkpoint 嵌入段去掉 JSON |
| O-4 | CLog 状态 JSON→二进制 (`clog_encode`, 5B/entry) | 1M 事务 ~5MB (vs JSON ~80MB) |
| O-5 | 删除 v2 WAL (`wal.h` / `wal.cpp`) | 启动代码无 v2 fallback 路径 |
| O-6 | `src/vendor/json.h` → `src/common/json.h` | Json 类从 "vendor 第三方" 转为 "项目自有", 收敛依赖 |

所有阶段均通过功能门禁 (13 个 SQL 功能测试, 模拟用户视角)。

## 测试覆盖

188 单元测试，全部通过：
- 二进制 codec 专项: 9 (O-1) + 9 (O-2) + 8 (O-3) + 9 (O-4)
- SQL 功能门禁: 13 (CRUD / 事务 / WHERE / 多表 / 数据类型 / NULL / PK 冲突 / 持久化 / 并发 / 索引)
- 事务子系统: 多隔离级、Savepoint 嵌套压力、SSI pivot、VACUUM、XID 回卷
- LSM 存储: SkipList 1W+4R 100k 并发、Arena、MemTable flush
- 崩溃恢复: Checkpoint + WAL replay 端到端
- 其他: 锁管理、Parser、NL→SQL、配置加载
