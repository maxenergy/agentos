
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
- Audit: ✅ 基础 JSONL 审计已实现

Skill System
- ISkill: ✅ 已定义
- Builtin Skills: ✅ file_read / file_write / file_patch / http_fetch
- Workflow Skill: 🚧 write_patch_read 内建 workflow 已实现
- CLI Skill: 🚧 rg_search / git_status / git_diff / curl_fetch 已接入

CLI Integration
- CLI Host: ✅ 支持 cwd 限制、timeout、stdout/stderr 捕获、输出限流、env 白名单
- Spec system: 🚧 代码内 CliSpec 已实现，外部 spec 文件尚未实现

Agent System
- IAgentAdapter: ✅ 已定义
- 单一 Agent 接入: 🚧 mock_planner + codex_cli 显式 target 调用
- Subagent Orchestration: 🚧 SubagentManager 已支持显式 sequential / parallel 编排、自动候选 agent 选择、并发限制和成本预算检查
- 多 Agent Router: 🚧 已支持基于健康状态、capability、历史统计和 lessons 的自动候选选择，任务拆分/角色分配待实现
- WorkspaceSession: 🚧 已支持在 workspace 内打开、使用、关闭支持 session 的 agent adapter

Auth System
- 基础结构: ✅ AuthManager / ProviderAdapter / SessionStore / CredentialBroker
- Profile mapping: ✅ runtime/auth_profiles.tsv provider 默认 profile 映射已实现
- OAuth: ❌
- CLI session: 🚧 Codex / Claude passthrough probe 与导入已实现，并已有可控 fixture 测试覆盖

Memory System
- Task log: ✅ 内存版 + runtime/memory/task_log.tsv 持久化已实现
- Step log: ✅ runtime/memory/step_log.tsv 持久化已实现
- LessonStore: 🚧 runtime/memory/lessons.tsv 重复失败聚合、Router hint 与 PolicyDenied hint 已实现
- Workflow: 🚧 候选生成 + runtime/memory/workflow_candidates.tsv + scoring 已实现，runtime/memory/workflows.tsv 持久化 / promotion / `workflow_run` 执行 / Router 自动选择已实现
- Scoring: 🚧 Skill / Agent 基础统计已持久化，AgentRouter 已接入历史评分

Scheduler
- ScheduledTask: ✅ 已实现 runtime/scheduler/tasks.tsv 持久化
- Scheduler: 🚧 支持一次性任务、interval / recurrence 任务、retry/backoff、missed_run_policy、手动 run-due、前台 tick / daemon loop 与 runtime/scheduler/runs.tsv 执行元数据，并已覆盖 disabled / missed interval 回归测试
- CronSupport: ❌ 尚未实现 cron 表达式

Identity / Trust
- IdentityManager: ✅ 已实现 runtime/trust/identities.tsv 身份目录
- PairingManager: ✅ 已实现 CLI pairing / block / remove / list
- TrustPolicy / AllowlistStore: ✅ 已实现 runtime/trust/allowlist.tsv 持久化 allowlist
- Trust Audit: ✅ identity / pair / block / remove mutation events 已写入 audit.log

Execution Safety
- IdempotencyKey: ✅ TaskRequest / SkillCall 已支持
- ExecutionCache: ✅ runtime/execution_cache.tsv 持久化缓存已实现
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
TrustPolicy: ✅ 已接入 PolicyEngine
AllowlistStore: ✅ runtime/trust/allowlist.tsv
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
* PermissionModel 已提供统一权限名、通配符匹配、未知权限拒绝和风险等级解析
* 更细的用户/角色级授权仍需后续演进

#### 必须补充

```text
PolicyEngine: ✅ 已实现
PermissionModel: ✅ 已实现统一权限判断、namespace wildcard、unknown permission deny
RiskLevel: ✅ 已实现 low/medium/high/critical/unknown 解析，unknown 默认要求 high-risk approval
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

* 已支持 idempotency_key 与成功结果缓存
* 后续仍需为更多副作用 Skill 强制要求 idempotency_key

#### 必须补充

```text
TaskId: ✅ TaskRequest.task_id
CallId: ✅ SkillCall.call_id
IdempotencyKey: ✅ TaskRequest / SkillCall
ExecutionCache: ✅ runtime/execution_cache.tsv
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
* WorkflowStore 持久化、promotion、`workflow_run` 执行持久定义与 Router 自动选择已实现
* 自动选择目前基于 enabled + trigger_task_type + required_inputs 匹配 + score 排序，尚未引入复杂条件表达式
* LessonStore 已可按 task_type / target / error_code 聚合重复失败
* Router 会使用 lesson hint 抑制重复失败的自动 workflow，并降低重复失败 agent 的候选分
* PolicyDenied 会附加已有 lesson hint，帮助解释重复失败模式，但不会改变硬性 allow/deny 规则

#### 必须补充

```text
TaskLog: ✅ runtime/memory/task_log.tsv
StepLog: ✅ runtime/memory/step_log.tsv
LessonStore: 🚧 runtime/memory/lessons.tsv + Router / PolicyDenied hint
WorkflowStore: 🚧 workflow_candidates.tsv 候选层 + workflows.tsv 持久化定义 / promotion / required_inputs / 执行 / Router 自动选择已实现
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
* recurrence=every:<n>s|m|h|d 已作为 cron 前的轻量复现语法
* disabled 任务会跳过执行；missed interval 支持 run-once 或 skip stale run，并从当前 scheduler time 计算下一次运行
* 仍缺少完整 cron 表达式

#### 必须补充

```text
Scheduler: 🚧 已实现 add/list/remove/run-due/tick/daemon/history/retry/recurrence/missed-run-policy，并覆盖 disabled/missed interval 回归测试
ScheduledTask: ✅ 已实现 runtime/scheduler/tasks.tsv
CronSupport: 🚧 every:<n>s|m|h|d 已实现，完整 cron 待实现
```

---

### ⑥ Subagent Orchestration（来自 Hermes）

#### 当前状态

🚧 MVP 已实现

#### 问题

* 已支持显式 agent 列表的 sequential / parallel 编排
* 空 agents 列表会自动选择健康且 capability 匹配的候选 agent，并按历史统计与 lessons 调整排序
* WorkspaceSession 已可管理支持 session 的 agent adapter 生命周期，并通过 run_task_in_session 执行任务
* parallel 模式会在启动前检查 max_parallel_subagents；执行后会汇总 estimated_cost 并按 budget_limit / manager 默认上限判定
* 仍缺少自动任务拆分、角色分配和更完整的 session health / idle timeout 管理

#### 必须补充

```text
SubagentManager: 🚧 已实现显式编排、并行执行、自动候选选择、并发限制与成本预算检查
AgentRouter: 🚧 基础历史评分已接入单代理选择，SubagentManager 已复用统计/lesson 进行候选排序
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

* Claude
* Codex
* Gemini
* Qwen

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
2. Idempotent Execution ✅ 已实现 idempotency_key + ExecutionCache
3. TaskLog ✅ 已实现 TaskLog / StepLog 持久化
4. Identity / Trust / Pairing ✅ 已实现 identity store + pairing + allowlist + remote trigger 拦截

---

### Phase 2

4. Workflow Generator ✅ 已实现基于 TaskLog/StepLog 的候选生成
5. Agent Scoring ✅ Router 已使用 agent success_rate / latency 进行基础排序

---

### Phase 3

6. Scheduler 🚧 MVP 已实现，一次性 / interval / recurrence / retry 任务可持久化，并支持 run-due / tick / daemon 执行与 run history
7. Subagent Manager 🚧 MVP 已实现，显式 sequential / parallel 编排、自动候选选择、成本/并发限制与 WorkspaceSession 基础生命周期可用

---

### Phase 4

8. CLI 安全增强 ✅ cwd / timeout / output limit / env allowlist 已实现
9. Workflow 优化 🚧 Workflow Scoring 已实现，Workflow 固化与自动选择待补充

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
