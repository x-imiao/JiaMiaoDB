#ifndef JIAMIAODB_SUPERVISOR_H
#define JIAMIAODB_SUPERVISOR_H

#include <string>
#include <vector>
#include "../types.h"
#include "../ai/planner.h"

class StorageEngine;

/* ═══════════════════════════════════════════════════════
   Supervisor — 安全监督层

   硬编码安全规则，AI 不可绕过。
   所有 ExecutionPlan 必须先经过验证才能执行。
   ═══════════════════════════════════════════════════════ */

struct VerificationResult {
    bool passed = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

class Supervisor {
public:
    explicit Supervisor(StorageEngine* storage);

    VerificationResult verify(const UnifiedIntent& intent, const ExecutionPlan& plan);

private:
    StorageEngine* storage_;

    void check_schema_exists(const UnifiedIntent& intent, std::vector<std::string>& errors);
    void check_write_safety(const UnifiedIntent& intent, const ExecutionPlan& plan,
                            std::vector<std::string>& errors, std::vector<std::string>& warnings);
    void check_delete_safety(const UnifiedIntent& intent, std::vector<std::string>& errors);
    void check_resource_budget(const ExecutionPlan& plan, std::vector<std::string>& warnings);
    void check_index_usage(const ExecutionPlan& plan, std::vector<std::string>& warnings);
};

#endif // JIAMIAODB_SUPERVISOR_H
