# AgentOS Skill System 设计文档

## 1. 目标

Skill System 负责为 AgentOS 提供统一的执行能力抽象。  
系统中所有“能做事的单元”都应被收敛为可观察、可路由、可审计、可演化的能力对象。

---

## 2. Skill 分类

## 2.1 Atomic Skill
最小基础能力，通常由内核内建或受控模块实现。

特点：

- 高复用
- 输入输出明确
- 低状态
- 容易做权限与边界控制

示例：
- file_read
- file_write
- file_patch
- http_fetch
- memory_store
- memory_recall

## 2.2 Workflow Skill
由多个 Skill 组合而成。

特点：

- 表达某类常见任务模式
- 通常来自历史执行沉淀
- 可由系统自动生成候选

示例：
- analyze_repo_changes
- summarize_log_errors
- draft_report_from_sources

## 2.3 CLI Skill
对现有 CLI 工具的包装。

特点：

- 外部工具纳管
- 配置驱动
- 易于快速扩展
- 需重点控制安全与 timeout

示例：
- rg_search
- git_diff
- jq_transform
- curl_fetch

## 2.4 Agent Skill
把二级代理以“能力”的方式被上层路由或调用。

特点：

- 具备自己的推理与规划能力
- 结果不是简单 stdout
- 常需 session 与结果标准化

示例：
- codex_reason
- claude_orchestrate
- gemini_research
- qwen_bulk_rewrite

---

## 3. 统一要求

所有 Skill 必须满足：

- 有 manifest
- 有输入 schema
- 有输出 schema 或至少有结构化结果
- 可被审计
- 可被评分
- 有风险等级
- 能被策略系统约束

---

## 4. 核心数据结构

## 4.1 SkillManifest

```cpp
struct SkillManifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> capabilities;
    std::string input_schema_json;
    std::string output_schema_json;
    std::string risk_level;
    std::vector<std::string> permissions;
    bool supports_streaming;
    bool idempotent;
    int timeout_ms;
};
```

## 4.2 SkillCall

```cpp
struct SkillCall {
    std::string call_id;
    std::string skill_name;
    std::string json_args;
    std::string workspace_id;
    std::string user_id;
};
```

## 4.3 SkillResult

```cpp
struct SkillResult {
    bool success;
    std::string json_output;
    std::string error_code;
    std::string error_message;
    int duration_ms;
};
```

---

## 5. 核心接口建议

```cpp
class ISkillAdapter {
public:
    virtual SkillManifest manifest() const = 0;
    virtual SkillResult execute(const SkillCall& call) = 0;
    virtual bool healthy() const = 0;
    virtual ~ISkillAdapter() = default;
};
```

---

## 6. Workflow 作为一等公民

Workflow 不是“脚本”，而是 Skill System 中的一种能力对象。

建议 Workflow 具备：

- name
- version
- steps
- preconditions
- success heuristics
- fallback logic
- score

示例：

```json
{
  "name": "repo_change_report",
  "steps": [
    {"skill": "git_diff"},
    {"skill": "rg_search"},
    {"skill": "summarize_text"},
    {"skill": "write_report"}
  ]
}
```

---

## 7. 注册中心

SkillRegistry 需要负责：

- 注册与注销
- 健康状态管理
- Manifest 查询
- 路由层查询
- 版本管理
- 启用/禁用控制

建议接口：

```cpp
class SkillRegistry {
public:
    void register_skill(std::shared_ptr<ISkillAdapter> skill);
    std::optional<std::shared_ptr<ISkillAdapter>> find(const std::string& name);
    std::vector<SkillManifest> list() const;
};
```

---

## 8. 安全要求

### 8.1 权限声明
每个 Skill 必须声明权限。

示例：
- filesystem.read
- filesystem.write
- process.spawn
- network.access

### 8.2 风险等级
建议等级：

- low
- medium
- high
- critical

### 8.3 工作区限制
涉及文件或命令的 Skill 默认应受 workspace 限制。

---

## 9. 评分与演化

每个 Skill 需要记录：

- 调用次数
- 成功率
- 平均耗时
- 平均成本
- 用户接受率
- 回滚率

示例：

```cpp
struct SkillStats {
    int total_calls;
    int success_calls;
    double avg_latency_ms;
    double avg_cost;
    double acceptance_rate;
};
```

系统可根据这些分数：

- 提升优先级
- 降级
- 灰度下线
- 转化为 Workflow 候选

---

## 10. 第一阶段建议内建 Skill

- file_read
- file_write
- file_patch
- http_fetch
- workflow_run

这五个即可支撑较多上层能力。
