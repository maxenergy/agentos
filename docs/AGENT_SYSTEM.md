# AgentOS 二级代理系统设计文档

## 1. 目标

AgentOS 不是只调用普通工具，它还需要在必要时借助“外部专家型代理”。  
这些代理本身具有：

- 推理能力
- 编排能力
- 会话状态
- 工具使用能力
- 代码补丁生成能力

因此，它们不能被简单视为普通 CLI，而应作为“二级代理”被统一接入。

---

## 2. 支持对象

第一阶段计划接入：

- Claude Code
- Codex CLI
- Gemini CLI
- Qwen Code
- 一个实验位（可接未来新代理或自研代理）

数量建议长期控制在 5 个以内。

---

## 3. 设计原则

### 3.1 不作为硬编码内核依赖
这些代理在产品形态上可以表现为“系统自带能力”，但在架构上应作为：

- 官方托管的外部代理
- 可禁用、可替换、可降级
- 有统一接口与统一路由

### 3.2 与普通 CLI 区分
普通 CLI：

- 单次调用
- 明确输入输出
- 通常无长会话

二级代理：

- 自己会规划
- 可能自己再调工具
- 可能产生中间事件流
- 常常需要 session

因此应单独定义接口。

---

## 4. 核心接口

## 4.1 AgentCapability

```cpp
struct AgentCapability {
    std::string name;
    int score;
};
```

## 4.2 AgentProfile

```cpp
struct AgentProfile {
    std::string agent_name;
    std::string version;
    std::string description;
    std::vector<AgentCapability> capabilities;

    bool supports_session;
    bool supports_streaming;
    bool supports_patch;
    bool supports_subagents;
    bool supports_network;

    std::string cost_tier;
    std::string latency_tier;
    std::string risk_level;
};
```

## 4.3 AgentTask

```cpp
struct AgentTask {
    std::string task_id;
    std::string task_type;
    std::string objective;
    std::string workspace_path;
    std::string context_json;
    std::string constraints_json;
    int timeout_ms;
    double budget_limit;
};
```

## 4.4 AgentArtifact

```cpp
struct AgentArtifact {
    std::string type;
    std::string uri;
    std::string content;
    std::string metadata_json;
};
```

## 4.5 AgentResult

```cpp
struct AgentResult {
    bool success;
    std::string summary;
    std::string structured_output_json;
    std::vector<AgentArtifact> artifacts;

    int duration_ms;
    double estimated_cost;

    std::string error_code;
    std::string error_message;
};
```

## 4.6 IAgentAdapter

```cpp
class IAgentAdapter {
public:
    virtual AgentProfile profile() const = 0;
    virtual bool healthy() const = 0;

    virtual std::string start_session(const std::string& session_config_json) = 0;
    virtual void close_session(const std::string& session_id) = 0;

    virtual AgentResult run_task(const AgentTask& task) = 0;
    virtual AgentResult run_task_in_session(
        const std::string& session_id,
        const AgentTask& task) = 0;

    virtual bool cancel(const std::string& task_id) = 0;

    virtual ~IAgentAdapter() = default;
};
```

---

## 5. Agent Registry

AgentRegistry 负责：

- 注册/注销代理
- 查询 Profile
- 管理健康状态
- 维护启用/禁用状态
- 暴露给 Router 候选集

建议接口：

```cpp
class AgentRegistry {
public:
    void register_agent(std::shared_ptr<IAgentAdapter> agent);
    std::optional<std::shared_ptr<IAgentAdapter>> find(const std::string& name);
    std::vector<AgentProfile> list_profiles() const;
};
```

---

## 6. Agent Router

AgentRouter 负责根据任务特征选择最合适代理。

### 6.1 静态初始角色建议
初始可配置为：

- Claude Code：编排、任务拆解、多步骤协调
- Codex CLI：复杂代码推理、精准 patch
- Gemini CLI：搜索、资料整合、外部研究
- Qwen Code：低成本任务、批量草稿、回退路径

### 6.2 动态评分
最终路由不能只靠初始假设。必须引入运行数据：

- 成功率
- 首次通过率
- 平均时延
- 平均成本
- 用户接受率
- 回滚率
- 当前健康状态

### 6.3 路由原则
- 默认单主代理
- 按需增加一个辅助代理
- 高级模式下才允许更复杂协作
- 防止并发失控和成本爆炸

---

### 6.4 当前实现状态

当前代码已具备以下 MVP：

- AgentRegistry 可注册并列出多个 `IAgentAdapter`
- Router 可基于健康状态、历史评分与 lessons 选择单一默认 agent
- SubagentManager 支持显式 agent 列表的 `sequential` / `parallel` 编排
- SubagentManager 在 agents 为空时可基于 enabled、capability、历史统计与 lessons 自动选择候选
- SubagentManager 支持 `auto_decompose=true`，先调用具备 `decomposition` capability 的规划 agent，将其 `plan_steps[].action` 映射到各 role / agent 的 subtask objective
- Codex CLI、Gemini、Anthropic、Qwen 与 local_planner adapter 会输出 `agent_result.v1` normalized result，包含 `summary`、`content`、`model`、`artifacts`、`metrics`、`tool_calls`、`provider_metadata` 与 `raw_output`
- SubagentManager 会在每个 `TaskStepRecord` 保留 agent 的 structured output / artifacts，并在整体 `output_json.agent_outputs[].normalized` 聚合各 agent 的 normalized output
- SubagentManager 复用 `PolicyEngine`、`AuditLogger`、`MemoryManager`
- WorkspaceSession 已支持基础 open / run / close 生命周期
- parallel 模式已支持并发上限与 estimated-cost budget 检查
- CLI 已支持 `agentos subagents run agents=<agent[,agent]> mode=<sequential|parallel>`

仍待补充：

- 更复杂的模型驱动任务拆分
- 更完整的 session health / idle timeout 管理
- 更完整的失败重试控制

---

## 7. 结果归一化

不同代理输出格式差异很大，因此 Agent Adapter 必须做结果标准化。

统一字段建议包括：

- summary
- changed_files
- patches
- tests
- warnings
- followup_suggestions

这样上层 Agent Core 不需要理解各家代理细节。

---

## 8. 评分系统

每个代理需维护：

```cpp
struct AgentRuntimeStats {
    int total_runs;
    int success_runs;
    int failed_runs;
    double avg_duration_ms;
    double avg_cost;
    double avg_user_score;
    double patch_accept_rate;
};
```

用途：

- 改善路由
- 判断是否降级或禁用
- 选择默认主代理
- 为 Workflow 选择更合适的专家

---

## 9. 会话模式

很多二级代理的真正价值在 session 模式中体现。  
因此第一阶段虽然可先支持单次调用，但架构必须提前预留：

- start_session
- run_task_in_session
- close_session
- session health / idle timeout

---

## 10. 第一阶段建议

第一版先接 1 个二级代理即可，比如：

- Codex CLI
或
- Claude Code

验证以下能力：
- Adapter 接口跑通
- 单次任务调用可用
- 结果标准化
- 状态与健康检查
- 路由与审计串起来
