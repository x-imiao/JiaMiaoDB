#ifndef JIAMIAODB_CATALOG_H
#define JIAMIAODB_CATALOG_H

#include <string>
#include <vector>
#include <map>
#include <tuple>
#include <cstdint>
#include <stdexcept>
#include "common/json.h"

using json = Json;

/* ═══════════════════════════════════════════════════════
   Catalog — 命名空间与用户管理

   管理 database / schema 层级和用户元数据。
   参照 CRDB descriptor 模型，所有元数据可序列化到 checkpoint。
   ═══════════════════════════════════════════════════════ */

struct UserInfo {
    std::string name;
    std::string password_hash;  // salted SHA-256 hex
    std::string salt;           // hex
};

struct SchemaInfo {
    std::string name;
};

struct DatabaseInfo {
    std::string name;
    std::map<std::string, SchemaInfo> schemas;  // 必须包含 "public"
};

class Catalog {
public:
    Catalog();

    // ── Database ──
    void create_database(const std::string& name);
    void drop_database(const std::string& name);
    bool database_exists(const std::string& name) const;
    std::vector<std::string> list_databases() const;

    // ── Schema ──
    void create_schema(const std::string& db_name, const std::string& schema_name);
    void drop_schema(const std::string& db_name, const std::string& schema_name);
    bool schema_exists(const std::string& db_name, const std::string& schema_name) const;
    std::vector<std::string> list_schemas(const std::string& db_name) const;

    // ── User ──
    void create_user(const std::string& name, const std::string& password);
    void drop_user(const std::string& name);
    bool user_exists(const std::string& name) const;
    bool verify_password(const std::string& name, const std::string& password) const;
    std::vector<std::string> list_users() const;

    // ── Session context ──
    void set_current_database(const std::string& name);
    std::string current_database() const;

    // ── Qualified name helper ──
    // Given a table name, produce "db.schema.table"
    // If name already contains dots, use as-is
    // Otherwise prepend current_db.public.
    std::string qualify_table_name(const std::string& name) const;

    // ── Serialization ──
    json to_json() const;
    void from_json(const json& j);
    bool has_data() const { return !databases_.empty() || !users_.empty(); }

    // ── Internal accessors (供 catalog_codec 序列化使用, 不暴露给业务) ──
    //   业务代码不应使用. catalog_codec 内部用 Snapshot 拿只读视图,
    //   internal_set_user 绕过 hash_password 直接灌入 hash + salt (用于 load 恢复)
    struct Snapshot {
        std::string current_db;
        std::vector<std::pair<std::string, std::vector<std::string>>>
            databases;  // db_name -> [schema_name, ...]
        std::vector<std::tuple<std::string, std::string, std::string>>
            users;      // (name, password_hash, salt)
    };
    Snapshot internal_snapshot() const;
    void     internal_set_user(const std::string& name,
                               const std::string& password_hash,
                               const std::string& salt);

private:
    std::map<std::string, DatabaseInfo> databases_;
    std::map<std::string, UserInfo> users_;
    std::string current_db_;

    // ── Crypto ──
    std::string sha256(const std::string& data) const;
    std::string generate_salt() const;
    std::string hash_password(const std::string& password, const std::string& salt) const;
};

#endif // JIAMIAODB_CATALOG_H
