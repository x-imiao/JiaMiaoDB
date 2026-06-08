#ifndef JIAMIAODB_NL_PARSER_H
#define JIAMIAODB_NL_PARSER_H

#include <string>
#include "../types.h"

/* ═══════════════════════════════════════════════════════
   NL Parser — 自然语言 → UnifiedIntent

   通过 MiniMax API 将自然语言翻译成结构化 Intent。
   使用 curl 做 HTTP 请求，解析 JSON 响应。
   后续可替换为本地模型。
   ═══════════════════════════════════════════════════════ */

struct NLParseResult {
    bool success = false;
    bool is_conversation = false;
    std::string reply;
    UnifiedIntent intent;
    std::string error;
};

class NLParser {
public:
    NLParser(const std::string& api_key, const std::string& api_base, const std::string& model);

    bool is_configured() const { return !api_key_.empty(); }

    NLParseResult parse(const std::string& input, const std::string& schema_context = "");

private:
    std::string api_key_;
    std::string api_base_;
    std::string model_;

    std::string build_prompt(const std::string& input, const std::string& schema);
    std::string call_api(const std::string& prompt);
    NLParseResult parse_response(const std::string& response, const std::string& input);
};

#endif // JIAMIAODB_NL_PARSER_H
