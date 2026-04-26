# AgentOS 架构设计文档

## 1. 设计目标

AgentOS 的架构目标不是做一个“更大的模型壳”，而是做一个：

- 核心稳定
- 能接外部工具
- 能接外部专家
- 能记录与学习
- 能逐步演化

的工程化系统。

---

## 2. 总体架构

```text
+--------------------------------------------------+
|                   Agent Core                     |
|--------------------------------------------------|
| Agent Loop | Router | Policy | Memory | Audit   |
+----------------------+---------------------------+
                       |
      +----------------+------------------+
      |                                   |
      v                                   v
+----------------------+      +--------------------------+
| Builtin Skills       |      | External Capability Host |
|----------------------|      |--------------------------|
| file_read            |      | CLI Host                |
| file_write           |      | Plugin Host             |
| file_patch           |      | Agent Adapter Host      |
| http_fetch           |      |                          |
| workflow_run         |      |                          |
+----------------------+      +--------------------------+
```

---

## 3. 模块划分

## 3.1 Agent Core
Agent Core 是系统的最小稳定内核，负责：

- Task 生命周期
- 上下文装载
- 路由决策
- 执行调度
- 失败恢复
- 策略校验
- 审计日志
- 记忆触发

它不应该承担重依赖、重计算、重状态的复杂功能。

### Core 子模块
- AgentLoop
- Router
- PolicyEngine
- MemoryManager
- AuditLogger
- Registry

---

## 3.2 Skill System
Skill System 是 AgentOS 的基础能力层。

Skill 分为四类：

### Atomic Skill
最小基础能力，适合高频、低复杂度、易受控操作。

示例：
- file_read
- file_write
- file_patch
- http_fetch

### Workflow Skill
由多个 Skill 组合而成。

示例：
- read_logs_and_summarize
- repo_change_report
- generate_patch_and_validate

### CLI Skill
将已有 CLI 工具封装为可控 Skill。

示例：
- rg_search
- git_diff
- jq_transform

### Agent Skill
把二级代理作为一种可调用能力使用。

示例：
- codex_reason
- claude_plan
- gemini_research
- qwen_bulk_transform

---

## 3.3 External Capability Host

### CLI Host
负责受控执行 CLI：

- 命令模板渲染
- cwd 控制
- stdout/stderr 捕获
- timeout
- 结构化解析
- 审计

### Plugin Host
负责多语言插件进程：

- Python
- Rust
- Node
- Go
- Java

统一通过 JSON-RPC / stdio / gRPC 协议接入。

当前实现已有 MVP 协议入口：`runtime/plugin_specs/*.tsv` 与 `runtime/plugin_specs/*.json` 可声明 repo-local `plugin.v1` / `stdio-json-v0` / `json-rpc-v0` 插件，启动时注册为 Skill，并透传底层 CLI 的资源限制字段。`stdio-json-v0` 校验 JSON-object stdout；`json-rpc-v0` 校验 JSON-RPC 2.0 response 并抽取 JSON-object `result`。`agentos plugins health` 支持 binary 可用性检查、可选 `health_args_template` 探针，以及 persistent JSON-RPC lifecycle round-trip；`agentos plugins inspect name=<plugin> [health=true]` 可脚本化查看单个 manifest 的协议、权限、参数、资源限制、sandbox、lifecycle、source 和 valid 状态，并可按需执行单插件 health probe。`agentos plugins lifecycle` 汇总 manifest lifecycle、当前 persistent session cap 和 plugin host config diagnostics。未知 manifest version / protocol 会被跳过。manifest 支持 `sandbox_mode=workspace|none`，默认 workspace 模式会拒绝解析到 workspace 外的路径类运行参数。manifest 也支持 `lifecycle_mode=oneshot|persistent`、`startup_timeout_ms` 和 `idle_timeout_ms`，其中 `persistent` 当前仅允许 `json-rpc-v0`，并通过长驻进程 stdin/stdout JSON-RPC session 复用处理请求。`runtime/plugin_host.tsv` 的 `max_persistent_sessions` 可限制长驻 session 数量并按 LRU 驱逐，非法配置会在 `plugins lifecycle` 中输出诊断。实现已拆为 manifest loader、schema validator、JSON-RPC helper、persistent session、health、sandbox、spec utils、SkillInvoker 与 runtime execution 文件，降低 `plugin_host.cpp` 的职责密度。

### Agent Adapter Host
负责托管二级代理：

- 启动 / 关闭会话
- 发送任务
- 收集结果
- 中断与超时
- 健康检查
- 标准化输出

---

## 4. 数据流

### 4.1 基础执行流程
1. 用户提交任务
2. Core 加载上下文和记忆
3. Router 选择 Skill / Agent
4. PolicyEngine 校验权限和边界
5. Executor 执行
6. Result Normalizer 归一化结果
7. MemoryManager 记录过程
8. AuditLogger 持久化审计

### 4.2 成长流程
1. 任务执行结束
2. 系统分析步骤序列
3. 判断是否形成高频稳定模式
4. 生成 Workflow 候选
5. 更新 Skill/Agent 评分
6. 优化后续路由策略

---

## 5. 接口分层

建议把执行能力分为三层接口：

### 5.1 ISkillAdapter
用于原子技能与普通能力

### 5.2 IWorkflowAdapter
用于内部组合执行

### 5.3 IAgentAdapter
用于二级代理

其中 IAgentAdapter 单独存在，避免与普通 CLI 混淆。

---

## 6. 路由架构

建议 Router 进一步拆分：

- SkillRouter
- WorkflowRouter
- AgentRouter

### 路由依据
- 任务类型
- 约束条件
- 成本预算
- 时延要求
- 历史成功率
- 近期健康状态
- 用户偏好

### 路由机制
分两层：

1. 静态初始策略
2. 动态评分修正

---

## 7. 关键设计原则

### 7.1 最小核心原则
核心负责决策与协调，不直接承载不稳定或重依赖能力。

### 7.2 一切能力外部化
尽可能把复杂能力变成可替换的外部单元。

### 7.3 先统一抽象，再接入能力
先定义 Skill、CLI、Agent 的统一模型，再接各种能力。

### 7.4 进化优先体现在 Workflow 与路由
第一阶段不要直接追求系统自改原生代码，而是先让它会积累 Workflow 和经验。

### 7.5 策略与安全是先验层
不是出问题后补安全，而是设计时就要限制边界。

---

## 8. 建议的代码目录

```text
agentos/
  CMakeLists.txt
  README.md
  plan.md
  src/
    auth/
    core/
      audit/
      execution/
      loop/
      orchestration/
      policy/
      registry/
      router/
    hosts/
      agents/
      cli/
      plugin/
    memory/
    scheduler/
    skills/
      builtin/
    trust/
    utils/
  tests/
  docs/
```

---

## 8.1 当前实现偏差

当前实现已经新增或提前落地了若干原规划未列出的目录：

- `src/auth/`：认证管理、provider adapter、session store、credential broker
- `src/cli/`：CLI command group dispatchers extracted from `main.cpp` as command groups stabilize; currently includes agents, auth, cli-specs, memory, plugins, storage, schedule, subagents, and trust commands
- `src/core/execution/`：idempotency execution cache
- `src/core/execution/task_lifecycle.*`：AgentLoop / SubagentManager shared step recording and task finalization helpers
- `src/core/orchestration/`：SubagentManager
- `src/core/router/`：Router facade plus separate `SkillRouter` / `AgentRouter` / `WorkflowRouter` implementation files
- `src/hosts/plugin/plugin_spec_utils.*`：shared PluginSpec support validation and PluginSpec-to-CliSpec conversion helpers
- `src/hosts/plugin/plugin_manifest_loader.cpp`：TSV / JSON plugin manifest parsing, validation diagnostics, source file/line metadata
- `src/hosts/plugin/plugin_schema_validator.*`：successful plugin output validation against `output_schema_json`
- `src/hosts/plugin/plugin_json_rpc.*`：JSON-RPC 2.0 request rendering, response validation, and result-object extraction
- `src/hosts/plugin/plugin_persistent_session.hpp`：persistent JSON-RPC process session lifecycle and request round-trip handling
- `src/hosts/plugin/plugin_health.cpp`：plugin binary, health probe, and persistent lifecycle health checks
- `src/hosts/plugin/plugin_execution.cpp`：PluginHost runtime execution, session-map management, and output validation orchestration
- `src/hosts/plugin/plugin_sandbox.*`：workspace sandbox path-argument containment checks for plugin execution
- `src/hosts/plugin/plugin_skill_invoker.cpp`：PluginSkillInvoker adapter implementation, separated from PluginHost runtime execution
- `src/scheduler/`：ScheduledTask 与 Scheduler
- `src/storage/`：storage manifest、export/import、status、migration policy
- `src/trust/`：IdentityManager、PairingManager、PairingInviteStore、AllowlistStore（含 paired / last_seen 设备生命周期元数据）、TrustPolicy

仍未实现：

- Plugin Host deeper process-pool policy and fuller lifecycle admin UX
- SQLite 或其他事务型存储后端（当前以 `ADR-STORAGE-001` 决策记录推迟，迁移前需先抽象 StorageBackend interface 并保留 TSV import 兼容路径）

WorkflowStore 当前已作为 `src/memory/` 下的持久化组件落地，写入 `runtime/memory/workflows.tsv`；暂未拆成独立 `workflow/` 目录。

---

## 9. 第一阶段落地顺序

1. Core 骨架
2. Builtin Skills
3. CLI Host
4. 一个二级代理
5. 认证子系统
6. 记忆系统
7. Workflow 生成
8. 动态评分与优化
