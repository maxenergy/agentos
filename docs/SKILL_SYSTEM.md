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
- agent.invoke
- task.submit

当前实现中的 `PermissionModel` 支持：

- 明确权限名
- `*` 全通配
- `filesystem.*` 这类 namespace wildcard
- 未知权限默认拒绝

`TaskRequest.permission_grants` 可进一步约束当前任务实际授予的权限；当 grants 非空时，Skill manifest 声明的权限必须被 grants 覆盖，Agent 调用必须具备 `agent.invoke`。当 grants 为空且配置了 `RoleCatalog` 时，`PolicyEngine` 会使用 `runtime/trust/roles.tsv` 中的 user-role 权限作为授予集合。

### 8.2 风险等级
建议等级：

- low
- medium
- high
- critical

当前实现会把未知风险等级视为高风险请求。CLI runtime 中必须同时提供 `allow_high_risk=true` 和已批准的 `approval_id`；审批记录保存在 `runtime/trust/approvals.tsv`，可通过 `agentos trust approval-request` / `approval-approve` / `approval-revoke` / `approvals` 管理。未配置 ApprovalStore 的低层单元测试 PolicyEngine 仍保持只校验 `approval_id` 存在的兼容行为。

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

---

## 11. Plugin Skill lifecycle 与 process pool 策略

Plugin Skill 由 `runtime/plugin_specs/*.tsv` / `*.json` 通过 `PluginHost` 加载，支持
两种 lifecycle：

- `lifecycle_mode=oneshot`（默认）— 每次调用启动一个独立子进程，立即收割。`stdio-json-v0` 与
  `json-rpc-v0` 都支持 oneshot。
- `lifecycle_mode=persistent` — 仅 `json-rpc-v0` 支持。`PluginHost` 维护一个长驻 stdin/stdout
  会话，按 request_id 路由 JSON-RPC 调用，命中失败/超时时自动重启会话。

`PluginHost` 通过两层上限管理 persistent 会话池：

- **全局上限**：`PluginHostOptions.max_persistent_sessions`（默认 16），可在
  `runtime/plugin_host.tsv` 中配置：

  ```
  max_persistent_sessions	16
  ```

  超出全局上限时按 LRU `last_used_at` 驱逐其他 plugin 的最旧 session。

- **每 plugin 上限**：`PluginSpec.pool_size`（默认 1），manifest 字段。当某个 plugin name
  的当前活跃会话数 ≥ `pool_size` 时，`PluginHost` 会先驱逐该 plugin 自己最旧的 session。
  实际生效值为 `min(pool_size, max_persistent_sessions)`。`pool_size` 仅在 `lifecycle_mode=persistent`
  时有意义；oneshot 时被忽略但仍可在 manifest 中声明。

每个 session 还有 `idle_timeout_ms`（默认 30000）。当一个 session 距离 `last_used_at`
超过此值时，下一次该 session 的请求会先重启进程。

### 11.1 Admin CLI

`agentos plugins` 提供运行时 session 管理：

- `agentos plugins sessions [name=<plugin>]` — 列出当前 PluginHost 中活跃的 persistent
  sessions，包含 plugin 名、pid、started_at_unix_ms、last_used_at_unix_ms、idle_for_ms、
  idle_timeout_ms、idle_expired、request_count、alive。带 `name=` 时只统计该 plugin，并在 summary 输出
  `matched=true|false`。summary 还会输出 `scope=process persistence=none`，明确该命令看到
  的是当前 CLI 进程内的 in-memory session 状态，而不是跨进程 daemon 状态；同时输出
  `idle_expired=<n>` 与 `dead=<n>`，方便脚本判断是否需要 restart/close。
- `agentos plugins session-prune [name=<plugin>] [dry_run=true]` — 清理当前 PluginHost
  中 idle-expired 或 dead 的 persistent sessions；不带 `name=` 时扫描全部 sessions。
  `dry_run=true` 只输出 `would_prune=<n>`，不修改 session 状态。输出 `pruned=<n>`、
  `matched=true|false`、`dry_run=true|false`、`reason=idle_expired_or_dead`、
  `scope=process persistence=none`。
- `agentos plugins session-restart name=<plugin>` — 强制重启某个 plugin 的所有 persistent
  sessions（适合修复挂死或想清空状态的场景），输出 `matched=true|false` 区分是否命中活跃
  session。
- `agentos plugins session-close name=<plugin>` — 优雅关闭某个 plugin 的所有 persistent
  sessions，输出 `matched=true|false` 区分是否命中活跃 session。

由于 CLI 命令和 daemon 进程是独立的，目前 `sessions/session-restart/session-close` 主要
反映当前 CLI 进程内启动的 PluginHost 状态；持续 daemon 化部署时可继续扩展为跨进程查询。
这些 session admin 命令可显式传入 `scope=process`（默认值）。`scope=daemon` 当前会返回
`plugin_sessions_unavailable scope=daemon supported_scope=process`，避免脚本误把当前进程状态
当成 daemon 全局状态。

这五个即可支撑较多上层能力。
