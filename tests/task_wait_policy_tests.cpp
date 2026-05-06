#include "core/execution/task_wait_policy.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestResearchPolicy() {
    const auto policy = agentos::ResolveTaskWaitPolicy(agentos::TaskWaitPolicyKind::research);
    Expect(policy.name == "interactive_research", "research policy should have a stable name");
    Expect(policy.idle_timeout_ms == 60000, "research idle timeout should allow slow tool setup");
    Expect(policy.soft_deadline_ms == 120000, "research soft deadline should be two minutes");
    Expect(policy.hard_deadline_ms == 600000, "research hard deadline should be ten minutes");

    agentos::TaskRequest task{};
    task.task_id = "research-test";
    task.task_type = "analysis";
    task.objective = "research";
    task.workspace_path = std::filesystem::temp_directory_path();
    agentos::ApplyTaskWaitPolicy(task, policy);

    Expect(task.timeout_ms == 600000, "apply should set agent hard deadline");
    Expect(task.inputs["wait_policy"] == "interactive_research", "apply should record policy name");
    Expect(task.inputs["idle_timeout_ms"] == "60000", "apply should record idle timeout");
    Expect(task.inputs["soft_deadline_ms"] == "120000", "apply should record soft deadline");
    Expect(task.inputs["hard_deadline_ms"] == "600000", "apply should record hard deadline");
    Expect(task.inputs["heartbeat_interval_ms"] == "15000", "apply should record heartbeat interval");
}

void TestDevelopmentPolicyDepth() {
    const auto research = agentos::ResolveTaskWaitPolicy(agentos::TaskWaitPolicyKind::research);
    const auto development = agentos::ResolveTaskWaitPolicy(agentos::TaskWaitPolicyKind::development);

    Expect(development.name == "interactive_development", "development policy should have a stable name");
    Expect(development.soft_deadline_ms > research.soft_deadline_ms,
        "development policy should allow deeper artifact work before soft deadline");
    Expect(development.hard_deadline_ms == research.hard_deadline_ms,
        "development and research should share the ten minute hard ceiling");
}

}  // namespace

int main() {
    TestResearchPolicy();
    TestDevelopmentPolicyDepth();

    if (failures != 0) {
        std::cerr << failures << " task wait policy test(s) failed\n";
        return 1;
    }
    std::cout << "task wait policy tests passed\n";
    return 0;
}
