
# AgentOS 架构对齐文档（ARCH_ALIGNMENT.md）

## 1. 文档目的

本文件用于：

* 对齐当前 AgentOS 实现与目标架构
* 明确需要补充的核心模块
* 避免反复重构或推倒重来
* 提供增量演进路径

> 原则：不重做，只演进

---

## 2. 当前实现评估

```text
Core Runtime
- AgentLoop: ✅ 已实现基础版本
- Router: 🚧 初版存在，内部已拆分为 SkillRouter / AgentRouter / WorkflowRouter，支持 Skill / Agent / Workflow 路由与健康检查
- Policy: 🚧 基础版存在，支持风险等级、网络权限、工作区路径限制
- Audit: ✅ 基础 JSONL 审计已实现，task 输入中的敏感值会在 audit 输出中脱敏；启动阶段跳过的外部 CLI spec / plugin spec 会写入 `config_diagnostic` audit event

Skill System
- ISkill: ✅ 已定义
- Builtin Skills: ✅ file_read / file_write / file_patch / http_fetch
- Workflow Skill: 🚧 write_patch_read 内建 workflow 与 stored workflow execution 已实现
- Skill Input Schema: ✅ input_schema_json.required 缺失字段、properties.*.type 基础类型不匹配，以及 const / enum / minLength / maxLength / pattern / additionalProperties:false / propertyNames / minProperties / maxProperties / minimum / maximum / exclusiveMinimum / exclusiveMaximum / multipleOf / dependentRequired / dependencies / not.required / if-then-else / allOf / anyOf / oneOf 约束不匹配会在执行前返回 SchemaValidationFailed
- CLI Skill: 🚧 rg_search / git_status / git_diff / curl_fetch 已接入

CLI Integration
- CLI Host: ✅ 支持 cwd 限制、timeout、stdout/stderr 捕获、输出限流、env 白名单、敏感参数脱敏，以及 Windows Job Object / POSIX `setrlimit` 的 `max_processes` / `memory_limit_bytes` / `cpu_time_limit_seconds`，POSIX 还支持 `file_descriptor_limit`
- Spec system: 🚧 代码内 CliSpec 与 runtime/cli_specs/*.tsv 外部 spec loader 已实现，TSV 解析会保留空字段以避免可选列错位，加载成功的外部 spec 会保留 source file/line，同名外部 spec 会成为 skipped spec 诊断，启动注册和 `agentos cli-specs validate` 会阻止外部 spec 覆盖已注册内置 skill，`parse_mode` 必须是 `text` / `json` / `json_lines`，`risk_level` 与 permissions 会按 `PermissionModel` 静态校验，`required_args` 必须被 `args_template` 占位符引用（保留参数 `cwd` 除外），schema 字段必须是 JSON object 形态，无效 numeric/resource 字段会成为 skipped spec 诊断，`timeout_ms` 必须为正数且整数资源限制必须非负，`agentos cli-specs validate` 可输出 skipped spec 诊断，启动注册会把 skipped spec 写入 audit
- Plugin Host: 🚧 runtime/plugin_specs/*.tsv 与 runtime/plugin_specs/*.json + plugin.v1 manifest + PluginSkillInvoker + `stdio-json-v0` / `json-rpc-v0` process protocol support 已实现，并透传底层 CLI 资源限制字段；加载成功的 plugin spec 会保留并在 CLI 输出 source file/line；同名 plugin spec 会成为 skipped spec 诊断；启动注册、`agentos plugins validate` 和 `agentos plugins health` 会阻止 plugin spec 覆盖已注册内置 skill 或有效外部 CLI spec；`risk_level` 与 permissions 会按 `PermissionModel` 静态校验，且 plugin manifest 必须声明 `process.spawn`；`required_args` 必须被 `args_template` 占位符引用（保留参数 `cwd` 除外）；`stdio-json-v0` 校验 JSON-object stdout，`json-rpc-v0` 校验 JSON-RPC 2.0 response 并抽取 JSON-object `result`，随后按 `output_schema_json.required`、`properties.*.type`、string `const` / `enum` / length / pattern、`additionalProperties:false`、`propertyNames`、`minProperties` / `maxProperties`、`dependentRequired` / `dependencies`、`not.required`、`if` / `then` / `else`、numeric range / `multipleOf`、`allOf` / `anyOf` / `oneOf` required-branch 校验后作为结构化 `plugin_output` 返回；`health_args_template` 不能引用运行期输入占位符；schema 字段必须是 JSON object 形态；无效 numeric/resource/bool 字段会成为 skipped spec 诊断，`timeout_ms` 必须为正数且整数资源限制必须非负；`agentos plugins` 可输出加载插件的健康状态和 skipped spec 诊断，`agentos plugins validate` 可只校验 manifest，`agentos plugins health` 可用于包含 binary 可用性和可选 `health_args_template` 探针的脚本化健康检查，`agentos plugins lifecycle` 可汇总 lifecycle 配置和当前 persistent session cap，`agentos plugins inspect name=<plugin> [health=true]` 可输出单个 manifest 的协议、权限、参数、资源限制、sandbox、lifecycle、source 和 valid 状态，并可按需执行单插件 health probe，plugin manifest 支持 `sandbox_mode=workspace|none` 且默认 workspace 模式会拒绝解析到 workspace 外的 path/file/dir 参数，`runtime/plugin_host.tsv` 可配置 `max_persistent_sessions`，启动注册会把 skipped spec 写入 audit；长驻可复用进程生命周期已实现，process-pool hardening 与 runtime session admin UX 待完善

Agent System
- IAgentAdapter: ✅ 已定义
- 单一 Agent 接入: 🚧 local_planner + codex_cli 显式 target 调用
- Subagent Orchestration: 🚧 SubagentManager 已支持显式 sequential / parallel 编排、自动候选 agent 选择、确定性角色分配、显式 per-agent subtask objective、`auto_decompose=true` planner-generated subtask objective、step-level structured output/artifact retention、`agent_outputs[].normalized` 聚合、并发限制和成本预算检查
- 多 Agent Router: 🚧 已支持基于健康状态、capability、历史统计和 lessons 的自动候选选择；复杂模型驱动任务拆分待实现
- WorkspaceSession: 🚧 已支持在 workspace 内打开、使用、关闭支持 session 的 agent adapter

Auth System
- 基础结构: ✅ AuthManager / ProviderAdapter / SessionStore / CredentialBroker
- Profile mapping: ✅ runtime/auth_profiles.tsv provider 默认 profile 映射已实现
- Gemini CLI OAuth passthrough: ✅ 可导入已登录的 `gemini` CLI 浏览器 OAuth 会话
- Native OAuth: 🚧 已支持 PKCE start/callback/listen/token exchange/session persistence、`oauth-login` 单命令编排、Gemini Google OAuth 默认配置、OpenAI PKCE 默认配置 (`auth.openai.com`)、`runtime/auth_oauth_providers.tsv` repo-local defaults 覆盖、含 origin/note 元数据的 `oauth-defaults`、`oauth-config-validate [--all]` 全 provider 诊断；Gemini 和 OpenAI 为 `builtin` provider，`anthropic`/`qwen` 仍为 `stub` provider discovery 条目；Anthropic 公网 PKCE endpoints 与多 provider 产品级登录 UX 仍待补齐
- Credential store: ✅ Windows Credential Manager / macOS Keychain (Security framework) / Linux Secret Service (libsecret 可选依赖) 已通过 `ISecureTokenBackend` 抽象接入；缺 libsecret 的 Linux 主机退化为 `env-ref-only` dev fallback；`SecureTokenStore::MakeInMemoryBackendForTesting()` 提供单元测试用内存后端，CI 中绝不触达真实 keychain
- CLI session: 🚧 Codex / Claude passthrough probe 与导入已实现，并已有可控 fixture 测试覆盖

Memory System
- Task log: ✅ 内存版 + runtime/memory/task_log.tsv 持久化已实现
- Step log: ✅ runtime/memory/step_log.tsv 持久化已实现
- LessonStore: 🚧 runtime/memory/lessons.tsv 重复失败聚合、Router hint 与 PolicyDenied hint 已实现
- Workflow: 🚧 候选生成 + runtime/memory/workflow_candidates.tsv + scoring 已实现，runtime/memory/workflows.tsv 持久化 / promotion / `workflow_run` 执行 / Router 自动选择已实现
- Scoring: 🚧 Skill / Agent 基础统计已持久化，AgentRouter 已接入历史评分
- Storage writes: 🚧 全量 TSV store 已通过 single-writer lock + 临时文件 + 原子替换降低并发写入和 crash corruption 风险；append log 已有 append-intent recovery，会在下一次追加时回放未完成意图或清理已落盘意图；多文件运行时更新已有 prepare/commit/recover 事务 helper 与 `agentos storage recover`
- Append logs: ✅ audit/task/step/scheduler history 已通过 single-writer append helper 和 append-intent recovery 降低并发追加冲突与中断写入风险
- Storage compaction: 🚧 `agentos storage compact` 已可重写 task/step/scheduler logs，并可在 `audit.log` 缺失时基于 `runtime/memory` 重建 task/route/step/task_end 审计事件，也可从 `runtime/scheduler/runs.tsv` 重建 `scheduler_run` 审计事件并替换旧 scheduler-run 行；非 task lifecycle 审计事件会保留，task-scoped 的 `policy` 等事件也会按时间戳稳定合流回对应 task 流程，不在 memory logs 中的 orphan task-scoped preserved events 也会保留；task lifecycle line 和 scheduler_run chunk 会按时间戳合流排序，且优先复用旧 lifecycle 时间戳；无时间戳的 global preserved events 会回退到所有 timed chunk 之后并保持原始顺序；完整 audit 重建仍待实现
- Storage versioning: 🚧 `runtime/storage_manifest.tsv` 已记录组件 format/version/path；`agentos storage status` 会展示每个受管文件的 exists/bytes/lines；更广义的跨格式 migration 仍待实现
- Storage migration helper: 🚧 `agentos storage migrate` 已可显式规范化 manifest/version，把旧的根目录存储文件迁到 `runtime/`，并将已兼容读取的 legacy memory/workflow/scheduler schema 重写为当前 TSV 形状；更广义的跨格式迁移仍待实现
- Storage backend decision: ✅ 当前明确保留 TSV 作为 MVP backend；`storage status` 会输出 `ADR-STORAGE-001`，记录 deferred SQLite target、StorageBackend migration boundary、required backend capabilities 和 TSV import compatibility contract
- Storage export: ✅ `agentos storage export dest=...` 会按 manifest 导出受管 runtime state
- Storage import: ✅ `agentos storage import src=...` 会按 manifest 恢复受管 runtime state

Scheduler
- ScheduledTask: ✅ 已实现 runtime/scheduler/tasks.tsv 持久化
- Scheduler: ✅ 支持一次性任务、interval / recurrence / cron 任务、retry/backoff、missed_run_policy、手动 run-due、前台 tick / daemon loop 与 runtime/scheduler/runs.tsv 执行元数据，并已覆盖 disabled / missed interval / cron 回归测试
- CronSupport: ✅ 已实现五字段 cron 表达式，支持 `*`、`*/n`、单值、逗号列表、范围、范围步进、`@hourly` / `@daily` / `@weekly` / `@monthly` / `@yearly` / `@annually` 别名，以及 day-of-month / day-of-week 同时受限时的 OR 语义

Identity / Trust
- IdentityManager: ✅ 已实现 runtime/trust/identities.tsv 身份目录
- PairingManager: ✅ 已实现 CLI pairing / block / remove / list
- PairingInviteStore: ✅ 已实现 runtime/trust/invites.tsv，一次性 token + TTL 的 invite-create / invite-accept / invites 配对握手
- TrustPolicy / AllowlistStore: ✅ 已实现 runtime/trust/allowlist.tsv 持久化 allowlist，含 paired_epoch_ms / last_seen_epoch_ms 设备生命周期元数据
- RoleCatalog: ✅ 已实现 runtime/trust/roles.tsv 持久化 role permissions 与 user-role assignments
- ApprovalStore: ✅ 已实现 runtime/trust/approvals.tsv 持久化审批请求，高风险 CLI runtime 策略会校验 approval_id 必须处于 approved 状态
- Trust Audit: ✅ identity / pair / block / remove mutation events 已写入 audit.log

Execution Safety
- IdempotencyKey: ✅ TaskRequest / SkillCall 已支持
- ExecutionCache: ✅ runtime/execution_cache.tsv 持久化缓存已实现
- Secret Redaction: ✅ CLI command/stdout/stderr 与 Audit task 字段已覆盖敏感输入值脱敏
```

---

## 3. 目标架构（冻结）

以下模块定义为 **AgentOS 核心架构，不轻易改变**

### 3.1 Core Runtime（最小内核）

* AgentLoop
* Router（SkillRouter / AgentRouter / WorkflowRouter）
* PolicyEngine
* Scheduler
* MemoryManager
* AuditLogger
* Registry（Skill / Agent）

---

### 3.2 Skill System

* Atomic Skill
* Workflow Skill
* CLI Skill
* Agent Skill

---

### 3.3 External Capability Layer

* CLI Host
* Plugin Host
* Agent Adapter Host

---

### 3.4 Auth System

* AuthManager
* ProviderAdapter
* SessionStore
* CredentialBroker

---

### 3.5 Memory & Evolution

* Task Log
* Step Log
* Workflow Generator
* Skill/Agent Scoring

---

## 4. 关键差距分析（来自 OpenClaw + Hermes）

---

## 4.1 必须补充（Critical）

### ① Identity / Trust / Pairing（来自 OpenClaw）

#### 当前状态

🚧 最小闭环已实现

#### 问题

* 外部输入已具备本地/远程边界，并已接入基础身份目录
* 已有 identity / device pairing / allowlist
* 远程触发默认拒绝，必须通过 pairing

#### 必须补充

```text
IdentityManager: ✅ runtime/trust/identities.tsv
PairingManager: ✅ 已实现
PairingInviteStore: ✅ runtime/trust/invites.tsv + 一次性 TTL 邀请握手
TrustPolicy: ✅ 已接入 PolicyEngine
AllowlistStore: ✅ runtime/trust/allowlist.tsv + paired/last_seen device lifecycle metadata
RoleCatalog: ✅ runtime/trust/roles.tsv
```

#### 原则

* 默认不信任外部输入
* 所有远程触发必须经过 pairing

---

### ② Policy Engine + 权限系统（来自 OpenClaw）

#### 当前状态

✅ MVP 已实现

#### 问题

* Skill / CLI Skill / Agent 已通过 PolicyEngine 统一检查
* 高危、网络、工作区逃逸、远程未配对触发已可拦截
* 高危/未知风险操作需要 `allow_high_risk=true` 和可审计的、已批准的 `approval_id`
* `TaskRequest.permission_grants` 已可按请求/用户上下文约束 Skill/Agent 权限
* PermissionModel 已提供统一权限名、通配符匹配、未知权限拒绝和风险等级解析
* 持久化 RoleCatalog 与用户授权管理已接入 `runtime/trust/roles.tsv`，支持 role/user assignment show/removal；ApprovalStore 支持单条 approval show/approve/revoke；`trust device-show` 支持单个 paired device 查询，仍需更完整 fleet/admin UX
* 持久化 ApprovalStore 已接入 `runtime/trust/approvals.tsv`，支持 request / approve / revoke / list，并由 CLI runtime 的 PolicyEngine 校验 approved 状态

#### 必须补充

```text
PolicyEngine: ✅ 已实现；constructor overloads 已收敛为单一 `PolicyEngineDependencies` dependency object，TrustPolicy / RoleCatalog / ApprovalStore 的可选依赖在 composition root 显式声明
Task lifecycle recording: ✅ AgentLoop 与 SubagentManager 已通过 shared `core/execution/task_lifecycle` helper 记录 step 与 finalize task，减少 audit/memory 写入路径分叉。
PermissionModel: ✅ 已实现统一权限判断、namespace wildcard、unknown permission deny
PermissionGrants: ✅ 已实现 per-task grants 对 Skill manifest permissions 与 agent.invoke 的约束
RiskLevel: ✅ 已实现 low/medium/high/critical/unknown 解析，unknown 默认要求 high-risk approval + approval_id
```

#### 示例权限

* filesystem.read
* filesystem.write
* process.spawn
* network.access
* agent.invoke

---

### ③ Idempotent Execution（来自 OpenClaw）

#### 当前状态

✅ 已实现 MVP

#### 问题

* 已支持 idempotency_key、task input/context fingerprint 与成功结果缓存
* ExecutionCache 命中需要 `idempotency_key` 与 task fingerprint 同时匹配；同 key 不同 inputs/context 会重新执行，避免误用旧成功结果。旧 9 列 TSV 缓存仍可读取，但新写入/compact 会保存 fingerprint。
* 非幂等且具备 `filesystem.write` 权限的 Skill 已强制要求 `idempotency_key`
* Scheduler 会为每次 due-task run 自动填充稳定的 per-run key，避免重复执行造成副作用

#### 必须补充

```text
TaskId: ✅ TaskRequest.task_id
CallId: ✅ SkillCall.call_id
IdempotencyKey: ✅ TaskRequest / SkillCall
ExecutionCache: ✅ runtime/execution_cache.tsv
Side-effect enforcement: ✅ filesystem.write + non-idempotent skill requires idempotency_key
Skill input schema: ✅ input_schema_json.required missing fields plus properties.*.type / const / enum / minLength / maxLength / pattern / additionalProperties:false / propertyNames / minProperties / maxProperties / minimum / maximum / exclusiveMinimum / exclusiveMaximum / multipleOf / dependentRequired / dependencies / not.required / if-then-else / allOf / anyOf / oneOf mismatches return SchemaValidationFailed before skill execution
```

#### 原则

* 所有有副作用操作必须幂等
* 支持安全重试

---

### ④ Memory + Workflow Learning（来自 Hermes）

#### 当前状态

✅ 基础闭环已实现

#### 问题

* TaskLog / StepLog / Skill-Agent Stats 已持久化
* Workflow 候选已可从成功历史中生成
* WorkflowStore 持久化、promotion、保存前校验、`memory stored-workflows` 过滤列表、`memory show-workflow`、`memory clone-workflow`、`memory update-workflow`、`memory set-workflow-enabled`、`memory remove-workflow`、`memory validate-workflows`、`memory explain-workflow`、`workflow_run` 执行持久定义与 Router 自动选择已实现
* 自动选择目前基于 enabled + trigger_task_type + required_inputs + input_equals + input_number_gte/lte + input_bool + input_regex + input_any 复合 OR 组 + input_expr 嵌套布尔表达式匹配 + score 排序；更完整的 workflow definition UX 仍待演进
* LessonStore 已可按 task_type / target / error_code 聚合重复失败
* Router 会使用 lesson hint 抑制重复失败的自动 workflow，并降低重复失败 agent 的候选分
* PolicyDenied 会附加已有 lesson hint，帮助解释重复失败模式，但不会改变硬性 allow/deny 规则

#### 必须补充

```text
TaskLog: ✅ runtime/memory/task_log.tsv
StepLog: ✅ runtime/memory/step_log.tsv
LessonStore: 🚧 runtime/memory/lessons.tsv + Router / PolicyDenied hint
WorkflowStore: 🚧 workflow_candidates.tsv 候选层 + workflows.tsv 持久化定义 / promotion / filtered stored-workflows / show-workflow / clone-workflow / update-workflow rename/edit/clear / set-workflow-enabled / remove-workflow / validate-workflows / explain-workflow / required_inputs / input_equals / input_number_gte/lte / input_bool / input_regex / input_any / input_expr / 执行 / Router 自动选择已实现
```

---

### ⑤ Scheduler（来自 Hermes）

#### 当前状态

🚧 MVP 已实现

#### 问题

* AgentOS 已可记录、手动触发到期任务，并可用前台 tick loop 周期执行
* `schedule daemon` 已作为长期运行的前台 wrapper 接入，复用同一套 due-task 执行路径
* Scheduler run history 已独立写入 runtime/scheduler/runs.tsv
* 失败任务可通过 max_retries / retry_backoff_seconds 进行有限重试
* recurrence=every:<n>s|m|h|d 可用于简单间隔任务
* cron="*/5 * * * *" 可用于五字段 cron 调度
* disabled 任务会跳过执行；missed interval 支持 run-once 或 skip stale run，并从当前 scheduler time 计算下一次运行

#### 必须补充

```text
Scheduler: ✅ 已实现 add/list/remove/run-due/tick/daemon/history/retry/recurrence/cron/missed-run-policy，并覆盖 disabled/missed interval/cron 回归测试
ScheduledTask: ✅ 已实现 runtime/scheduler/tasks.tsv
CronSupport: ✅ 五字段 cron、常用别名和 DOM/DOW OR 日历语义已实现，timezone/DST 边界可继续增强
```

---

### ⑥ Subagent Orchestration（来自 Hermes）

#### 当前状态

🚧 MVP 已实现

#### 问题

* 已支持显式 agent 列表的 sequential / parallel 编排
* 空 agents 列表会自动选择健康且 capability 匹配的候选 agent，并按历史统计与 lessons 调整排序
* 可通过 `roles=agent:role` 指定角色，未指定时会按 agent capability 推导角色
* 每个 subagent 会收到 role-scoped subtask objective 和包含 role/parent task 的 context
* 可通过 `subtasks=role_or_agent=objective;...` 或 `subtask_<agent|role>=...` 给不同 subagent 分派不同 objective
* 可通过 `auto_decompose=true` 先调用具备 `decomposition` capability 的规划 agent，抽取 `plan_steps[].action` 并映射为 deterministic subtask objective
* Codex CLI、Gemini、Anthropic、Qwen 与 local_planner adapter 会输出 `agent_result.v1` normalized result，并保留 raw provider output
* subagent 的 `TaskStepRecord` 会保留 agent structured output / artifacts，整体结果会通过 `agent_outputs[].normalized` 聚合各 agent 的 normalized fields
* WorkspaceSession 已可管理支持 session 的 agent adapter 生命周期，并通过 run_task_in_session 执行任务
* parallel 模式会在启动前检查 max_parallel_subagents；执行后会汇总 estimated_cost 并按 budget_limit / manager 默认上限判定
* 仍缺少模型驱动复杂任务拆分和更完整的 session health / idle timeout 管理

#### 必须补充

```text
SubagentManager: 🚧 已实现显式编排、并行执行、自动候选选择、确定性角色分配、显式 per-agent subtask objective、planner-generated subtask objective、step-level structured output/artifact retention、`agent_outputs[].normalized` 聚合、并发限制与成本预算检查
AgentRouter: 🚧 基础历史评分已接入单代理选择，SubagentManager 已复用统计/lesson 进行候选排序
Router implementation boundary: ✅ `SkillRouter`、`AgentRouter`、`WorkflowRouter` 已从原 `router_components.cpp` 拆为独立实现文件，保持同一 public header。
CLI command split: ✅ `agentos agents`、`agentos auth`、`agentos cli-specs`、`agentos memory`、`agentos plugins`、`agentos storage`、`agentos schedule`、`agentos subagents` 与 `agentos trust` command groups 已从 `main.cpp` 抽到 `src/cli/*_commands.*`；`main.cpp` 保留 runtime composition root、demo/run 分发与启动期 capability 注册。
WorkspaceSession: 🚧 已实现基础 open/run/close 生命周期
```

---

## 4.2 重要但可延后（Important）

### ⑦ CLI 安全增强

```text
timeout 强制化: ✅ CliSpec.timeout_ms
stdout/stderr 限流: ✅ CliSpec.output_limit_bytes
env 白名单: ✅ CliSpec.env_allowlist + 默认最小系统环境
cwd 限制: ✅ cwd 必须在 workspace 内
敏感参数脱敏: ✅ command_display / stdout / stderr 会按敏感参数名隐藏值
Windows 资源限制: ✅ CliSpec.max_processes / memory_limit_bytes / cpu_time_limit_seconds 通过 Job Object 强制
POSIX 资源限制: ✅ CliSpec.max_processes / memory_limit_bytes / cpu_time_limit_seconds / file_descriptor_limit 通过 setrlimit 尽力强制
```

---

### ⑦b Plugin Host

```text
PluginSpec loader: ✅ runtime/plugin_specs/*.tsv + runtime/plugin_specs/*.json
Manifest version: ✅ plugin.v1 TSV/JSON MVP，未知版本会跳过
PluginSkillInvoker: ✅ 注册为 SkillRegistry skill；adapter implementation 已从 `plugin_host.cpp` 拆到 `plugin_skill_invoker.cpp`
Protocol MVP: ✅ `stdio-json-v0` JSON-object stdout capture plus `json-rpc-v0` JSON-RPC 2.0 response/result extraction with structured plugin_output embedding
Protocol validation: ✅ 未知 protocol 会跳过加载，运行时也会拒绝；`json-rpc-v0` 会拒绝 malformed response 和 JSON-RPC error response
Output schema: ✅ required / properties.*.type / string const-enum-length-pattern / additionalProperties:false / propertyNames / minProperties / maxProperties / dependentRequired / dependencies / not.required / if-then-else / numeric range / multipleOf / allOf-anyOf-oneOf required-branch validation
Health validation: ✅ manifest version / protocol / name / binary / command availability
Health/admin reporting: ✅ CheckPluginHealth + optional health_args_template probes + agentos plugins + agentos plugins validate + agentos plugins health + agentos plugins inspect
Load diagnostics: ✅ skipped plugin specs include file / line / reason
TSV parsing: ✅ plugin manifest loader preserves empty fields to avoid optional-column shifts
JSON manifest parsing: ✅ plugin manifest loader accepts string arrays, object-valued schema fields, numeric resource fields, and boolean idempotent fields
Health probes: ✅ optional health_args_template / health_timeout_ms in TSV and JSON manifests, with validation rejecting runtime-input placeholders
Resource limits: ✅ plugin manifest 可透传底层 CliSpec memory/process/CPU/fd 限制字段
Lifecycle fields: ✅ `lifecycle_mode=oneshot|persistent`、`startup_timeout_ms`、`idle_timeout_ms` 已支持 TSV/JSON manifest、CLI 输出和校验；`persistent` 当前要求 `json-rpc-v0`
Lifecycle runtime: ✅ persistent `json-rpc-v0` session manager 已支持 stdin/stdout JSON-RPC round-trip、request id、timeout、stderr capture、失败后 session eviction/restart、destructor shutdown、lifecycle-aware health、`lifecycle_event` 输出、active session count、manual close、idle-timeout restart、configurable max persistent session cap 和 LRU eviction
Reusable process lifecycle: ✅ MVP 已实现；更细的 process-pool policy 与更丰富 admin UX 待补
Plugin host module split: ✅ `plugin_manifest_loader.cpp` 已承接 TSV/JSON manifest loading 与 diagnostics，`plugin_schema_validator.*` 已承接 output schema validation，`plugin_json_rpc.*` 已承接 JSON-RPC request/response helpers，`plugin_persistent_session.hpp` 已承接 persistent process lifecycle，`plugin_health.cpp` 已承接 health checks，`plugin_execution.cpp` 已承接 PluginHost runtime execution，`plugin_spec_utils.*` 已承接 PluginSpec 支持性校验与 PluginSpec-to-CliSpec 转换，`plugin_skill_invoker.cpp` 已承接 SkillRegistry adapter。
Plugin sandbox boundary: ✅ workspace path-argument containment 已拆到 `plugin_sandbox.*`，由 PluginHost runtime execution 复用。
Sandboxing: ✅ `sandbox_mode=workspace|none`，默认 workspace 模式会限制路径类运行参数留在 workspace 内
```

---

### ⑧ Agent Scoring

```text
success_rate
cost
latency
acceptance
```

---

### ⑨ Workflow Scoring

```text
use_count: ✅ 已实现
success_rate: ✅ 已实现
failure_count: ✅ 已实现
avg_duration_ms: ✅ 已实现
score: ✅ success_count / success_rate / failure_count / latency 综合评分
```

---

## 4.3 可外部化模块（External）

这些不进入核心：

### 渠道适配

* Slack / Telegram / 微信
* Email / Webhook

---

### MCP / Skill Hub

* MCP adapters
* 外部 skill marketplace

---

### Execution Backends

* Docker
* SSH
* 云执行

---

### 二级代理供应商

* Claude: ✅ API-key/auth-session backed Messages adapter plus Claude CLI passthrough execution
* Codex
* Gemini: ✅ API-key/auth-session backed `generateContent` adapter
* OpenAI: ✅ API-key/browser-OAuth backed Chat Completions adapter (`api.openai.com/v1/chat/completions`, sync + V2 SSE streaming, default gpt-4o)
* Qwen: ✅ API-key/auth-session backed Alibaba Cloud Model Studio OpenAI-compatible Chat Completions adapter

---

### UI / Voice

* Voice wake
* Canvas
* Mobile UI

---

### 训练与轨迹导出

* RL data
* trajectory export

---

## 5. 架构冻结决策（重要）

以下设计不再推翻：

```text
- C++ 核心内核
- Skill 抽象
- AgentAdapter 抽象
- CLI Host 模式
- Auth 分层结构
```

---

## 6. 增量演进路线（推荐顺序）

### Phase 1（现在开始）

1. PolicyEngine ✅ 已实现，已接入 TrustPolicy / PermissionModel
2. Idempotent Execution ✅ 已实现 idempotency_key + ExecutionCache + 副作用写入强制 key
3. TaskLog ✅ 已实现 TaskLog / StepLog 持久化
4. Identity / Trust / Pairing ✅ 已实现 identity store + pairing invite handshake + device lifecycle + allowlist + remote trigger 拦截

---

### Phase 2

4. Workflow Generator ✅ 已实现基于 TaskLog/StepLog 的候选生成
5. Agent Scoring ✅ Router 已使用 agent success_rate / latency 进行基础排序

---

### Phase 3

6. Scheduler ✅ MVP 已实现，一次性 / interval / recurrence / cron / retry 任务可持久化，并支持 run-due / tick / daemon 执行与 run history
7. Subagent Manager 🚧 MVP 已实现，显式 sequential / parallel 编排、自动候选选择、确定性角色分配、成本/并发限制与 WorkspaceSession 基础生命周期可用

---

### Phase 4

8. CLI 安全增强 ✅ cwd / timeout / output limit / env allowlist 已实现
9. Workflow 优化 🚧 Workflow Scoring、WorkflowStore 固化、workflow definition 校验、workflow applicability explain、`workflow_run` 持久定义执行、Router 自动选择与 `required_inputs` / `input_equals` / 数值范围 / 布尔输入 / 正则输入 / `input_any` 复合 OR / `input_expr` 嵌套布尔表达式匹配已实现，更完整的 workflow definition UX 待补充

---

## 7. 重构原则

### 可以做

* 模块拆分
* 接口优化
* 内部实现替换

### 不要做

* 推翻核心架构
* 改抽象模型
* 重写系统

---

## 8. 判断标准（很关键）

每次开发前问：

> 这是增强“能力”还是增强“决策能力”？

优先做：

* 路由优化
* 记忆
* Workflow

而不是：

* 再接一个工具
* 再接一个模型

---

## 9. 一句话总结

AgentOS 的目标不是：

❌ 最多工具
❌ 最强模型

而是：

> ✅ 最会“选择 + 协作 + 学习”的系统

---
