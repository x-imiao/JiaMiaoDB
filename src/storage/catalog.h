#ifndef JIAMIAODB_CATALOG_H
#define JIAMIAODB_CATALOG_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <stdexcept>
#include "json.h"

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
