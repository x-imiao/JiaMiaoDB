#include "nl_parser.h"
#include <curl/curl.h>
#include "json.h"
#include <sstream>
#include <iostream>

using json = Json;

NLParser::NLParser(const std::string& api_key, const std::string& api_base, const std::string& model)
    : api_key_(api_key), api_base_(api_base), model_(model) {}

static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t total = size * nmemb;
    output->append((char*)contents, total);
    return total;
}

std::string NLParser::build_prompt(const std::string& input, const std::string& schema) {
    std::string prompt = R"({
  "model": ")" + model_ + R"(",
  "messages": [
    {
      "role": "system",
      "content": "你是一个数据库意图解析器。将用户的自然语言翻译成结构化的 JSON。

规则：
1. 先判断是不是数据库操作。如果不是（打招呼、闲聊），返回：{\"category\": \"conversation\", \"reply\": \"友好的回复\"}
2. 如果是数据库操作，输出 JSON（不要 markdown，不要额外文字）：

{
  \"category\": \"query\" | \"mutation\" | \"schema\",
  \"tables\": [\"表名\"],
  \"description\": \"简短的操作描述\",
  \"isWrite\": true | false,
  \"operation\": {
    \"type\": \"select\" | \"insert\" | \"update\" | \"delete\" | \"create_table\" | \"drop_table\",
    // 根据 type 填写对应字段
  }
}

SELECT: {\"type\": \"select\", \"columns\": [\"col1\"], \"from\": \"table\", \"where\": {\"type\": \"binary_op\", \"op\": \"=\", \"left\": {\"type\": \"column_ref\", \"name\": \"col\"}, \"right\": {\"type\": \"literal\", \"value\": \"val\"}}}
INSERT: {\"type\": \"insert\", \"table\": \"t\", \"columns\": [\"col1\"], \"values\": [[\"val1\"]]}
UPDATE: {\"type\": \"update\", \"table\": \"t\", \"sets\": [{\"column\": \"col\", \"value\": {\"type\": \"literal\", \"value\": \"val\"}}], \"where\": ...}
DELETE: {\"type\": \"delete\", \"table\": \"t\", \"where\": ...}
CREATE TABLE: {\"type\": \"create_table\", \"name\": \"t\", \"columns\": [{\"name\": \"col\", \"type\": \"TEXT\", \"nullable\": false}]}
DROP TABLE: {\"type\": \"drop_table\", \"name\": \"t\"}

表达式格式：{\"type\": \"binary_op\"|\"column_ref\"|\"literal\", \"op\": \"=\"|\"!=\"|\"<\"|\">\"|\"AND\"|\"OR\", \"left\": ..., \"right\": ...}

重要：
- SELECT 必须有 columns 和 from
- INSERT 的 values 是二维数组
- description 用中文
- 如果不确定，category 用 query"
    };

    if (!schema.empty()) {
        prompt += "当前数据库 Schema:\n" + schema + "\n\n";
    }
    prompt += "用户输入: " + input + R"("
    }
  ],
  "temperature": 0.1,
  "max_tokens": 2000,
  "response_format": { "type": "json_object" }
})";

    return prompt;
}

std::string NLParser::call_api(const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    std::string url = api_base_ + "/chat/completions";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key_).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // 跳过代理证书问题
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "[NL] API 请求失败: " << curl_easy_strerror(res) << "\n";
        return "";
    }

    return response;
}

NLParseResult NLParser::parse_response(const std::string& response, const std::string& input) {
    NLParseResult result;

    if (response.empty()) {
        result.success = false;
        result.error = "MiniMax API 返回空";
        return result;
    }

    try {
        json j = json::parse(response);

        // Extract content from choices[0].message.content
        std::string content;
        if (j.contains("choices") && !j["choices"].empty()) {
            content = j["choices"][0]["message"]["content"].get_string();
        } else {
            result.success = false;
            result.error = "MiniMax 返回格式异常";
            return result;
        }

        // Strip thinking tags
        auto think_start = content.find("<think>");
        while (think_start != std::string::npos) {
            auto think_end = content.find("</think>", think_start);
            if (think_end != std::string::npos) {
                content.erase(think_start, think_end + 8 - think_start);
            } else break;
            think_start = content.find("<think>");
        }

        // Extract JSON
        auto brace_start = content.find('{');
        auto brace_end = content.rfind('}');
        if (brace_start == std::string::npos || brace_end == std::string::npos) {
            result.success = false;
            result.error = "API 返回没有有效 JSON";
            return result;
        }
        std::string json_str = content.substr(brace_start, brace_end - brace_start + 1);

        json parsed = json::parse(json_str);

        // Conversation check
        if (parsed.contains("category") && parsed["category"].get_string() == "conversation") {
            result.success = true;
            result.is_conversation = true;
            result.reply = parsed.value("reply", std::string("你好！"));
            return result;
        }

        // Build UnifiedIntent
        result.success = true;
        result.intent.raw_input = input;
        result.intent.description = parsed.value("description", std::string(""));
        result.intent.is_write = parsed.value("isWrite", false);

        std::string cat = parsed.value("category", std::string("query"));
        if (cat == "query") result.intent.category = IntentCategory::QUERY;
        else if (cat == "mutation") result.intent.category = IntentCategory::MUTATION;
        else if (cat == "schema") result.intent.category = IntentCategory::SCHEMA;

        if (parsed.contains("tables")) {
            for (const auto& t : parsed["tables"]) result.intent.tables.push_back(t.get_string());
        }

        // Parse operation
        if (parsed.contains("operation")) {
            auto& op = parsed["operation"];
            std::string op_type = op.value("type", std::string(""));

            if (op_type == "select") {
                auto stmt = std::make_unique<SelectStmt>();
                stmt->from_table = op.value("from", std::string(""));
                if (op.contains("columns")) {
                    for (const auto& c : op["columns"]) stmt->columns.push_back(c.get_string());
                }
                // TODO: parse where expression
                result.intent.statement.type = StatementType::SELECT;
                result.intent.statement.select = std::move(stmt);
            } else if (op_type == "insert") {
                auto stmt = std::make_unique<InsertStmt>();
                stmt->table = op.value("table", std::string(""));
                if (op.contains("columns")) {
                    for (const auto& c : op["columns"]) stmt->columns.push_back(c.get_string());
                }
                if (op.contains("values")) {
                    for (const auto& val_arr : op["values"]) {
                        std::vector<Value> row;
                        for (const auto& v : val_arr) {
                            if (v.is_null()) row.push_back(nullptr);
                            else if (v.is_int()) row.push_back(v.get_int());
                            else if (v.is_float()) row.push_back(v.get_float());
                            else if (v.is_bool()) row.push_back(v.get_bool());
                            else row.push_back(v.get_string());
                        }
                        stmt->values.push_back(std::move(row));
                    }
                }
                result.intent.statement.type = StatementType::INSERT;
                result.intent.statement.insert = std::move(stmt);
            } else if (op_type == "update") {
                auto stmt = std::make_unique<UpdateStmt>();
                stmt->table = op.value("table", std::string(""));
                result.intent.statement.type = StatementType::UPDATE;
                result.intent.statement.update = std::move(stmt);
            } else if (op_type == "delete") {
                auto stmt = std::make_unique<DeleteStmt>();
                stmt->table = op.value("table", std::string(""));
                result.intent.statement.type = StatementType::DELETE;
                result.intent.statement.delete_stmt = std::move(stmt);
            } else if (op_type == "create_table") {
                auto stmt = std::make_unique<CreateTableStmt>();
                stmt->name = op.value("name", std::string(""));
                if (op.contains("columns")) {
                    for (const auto& c : op["columns"]) {
                        ColumnDef col;
                        col.name = c.value("name", std::string(""));
                        col.type = data_type_from_name(c.value("type", std::string("TEXT")));
                        col.nullable = c.value("nullable", true);
                        col.primary_key = c.value("primary_key", false);
                        stmt->columns.push_back(col);
                    }
                }
                result.intent.statement.type = StatementType::CREATE_TABLE;
                result.intent.statement.create_table = std::move(stmt);
            } else if (op_type == "drop_table") {
                auto stmt = std::make_unique<DropTableStmt>();
                stmt->name = op.value("name", std::string(""));
                result.intent.statement.type = StatementType::DROP_TABLE;
                result.intent.statement.drop_table = std::move(stmt);
            }
        }

    } catch (const std::exception& e) {
        result.success = false;
        result.error = "解析响应失败: ";
        result.error += e.what();
    }

    return result;
}

NLParseResult NLParser::parse(const std::string& input, const std::string& schema_context) {
    if (!is_configured()) {
        NLParseResult result;
        result.success = false;
        result.error = "AI API 未配置";
        return result;
    }

    std::string body = build_prompt(input, schema_context);
    std::string response = call_api(body);
    return parse_response(response, input);
}
