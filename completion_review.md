# AgentOS 项目完成度审核报告

> **审核日期**: 2026-04-25
> **审核依据**: [plan.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/plan.md) · [README.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/README.md) · [docs/ROADMAP.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/docs/ROADMAP.md) · [docs/ARCH_ALIGNMENT.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/docs/ARCH_ALIGNMENT.md) · 源代码与测试文件

---

## 总评

| 维度 | 评分 | 说明 |
|------|------|------|
| **MVP 完成度** | **~90%** | 本地可运行 MVP 已达标，所有核心子系统均有实现并完成多项审核后硬化 |
| **产品级完成度** | **~55-60%** | Auth 硬化、Plugin 生命周期、存储事务性、Agent 编排深度仍有明确缺口 |
| **plan.md 执行率** | **~98%** | Phase A-I 已完成；Post-Review Phase J-N 中仅剩少量生产化/依赖评估项 |
| **文档一致性** | **良好** | README / ROADMAP / ARCH_ALIGNMENT / plan.md 四文档基本同步 |
| **测试覆盖** | **良好** | 11 个 CTest 目标 + GitHub Actions CI（Windows + Ubuntu） |

> [!IMPORTANT]
> 项目定位为 **"可运行的本地 MVP，而非生产级系统"**。在此定位下，完成度已经较高。生产化的主要剩余阻塞项是 Auth 交互 UX/非 Windows 凭据存储、Plugin 更深进程池/隔离策略、Timezone/DST 与结构化 JSON 依赖迁移。

---

## 1. 代码规模概览

### 1.1 源代码结构

```
src/                        ← 主源码
├── main.cpp                  ~850 lines (Runtime 组合根 + demo/run 分发；命令组已拆到 src/cli)
├── core/                     ← 内核
│   ├── models.hpp              5,799 bytes
│   ├── audit/                  AuditLogger
│   ├── execution/              ExecutionCache
│   ├── loop/                   AgentLoop
│   ├── orchestration/          SubagentManager + WorkspaceSession
│   ├── policy/                 PolicyEngine + PermissionModel + RoleCatalog + ApprovalStore
│   ├── registry/               SkillRegistry + AgentRegistry
│   ├── router/                 Router + SkillRouter/AgentRouter/WorkflowRouter
│   └── schema/                 SchemaValidator
├── auth/                     ← 认证子系统 (15 files)
├── hosts/
│   ├── agents/                 5 agent adapters (10 files)
│   ├── cli/                    CliHost + CliSkillInvoker + SpecLoader (6 files)
│   └── plugin/                 PluginHost + loader/schema/health/execution/session 分拆模块
├── memory/                   ← 记忆 + Workflow (8 files)
├── scheduler/                ← 调度器 (2 files, 26KB)
├── skills/builtin/           ← 5 内建 Skill (10 files)
├── storage/                  ← 存储版本/导出/导入 (6 files)
├── trust/                    ← 身份/信任 (12 files)
└── utils/                    ← 工具函数 (12 files)
```

### 1.2 测试文件

| 测试目标 | 文件 | 大小 |
|----------|------|------|
| `agentos_cli_integration_tests` | [cli_integration_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/cli_integration_tests.cpp) | 55 KB |
| `agentos_cli_plugin_tests` | [cli_plugin_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/cli_plugin_tests.cpp) | 92 KB |
| `agentos_workflow_router_tests` | [workflow_router_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/workflow_router_tests.cpp) | 49 KB |
| `agentos_file_skill_policy_tests` | [file_skill_policy_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/file_skill_policy_tests.cpp) | 39 KB |
| `agentos_policy_trust_tests` | [policy_trust_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/policy_trust_tests.cpp) | 34 KB |
| `agentos_subagent_session_tests` | [subagent_session_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/subagent_session_tests.cpp) | 28 KB |
| `agentos_scheduler_tests` | [scheduler_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/scheduler_tests.cpp) | 26 KB |
| `agentos_storage_tests` | [storage_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/storage_tests.cpp) | 26 KB |
| `agentos_agent_provider_tests` | [agent_provider_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/agent_provider_tests.cpp) | 23 KB |
| `agentos_auth_tests` | [auth_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/auth_tests.cpp) | 21 KB |
| `agentos_spec_parsing_tests` | [spec_parsing_tests.cpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/spec_parsing_tests.cpp) | 3 KB |
| **共享夹具** | [test_command_fixtures.hpp](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/tests/test_command_fixtures.hpp) | 6.5 KB |

> 测试代码总量约 **400+ KB**，测试代码量已超过不少源模块。

### 1.3 运行时持久化文件

```
runtime/
├── audit.log                 36 KB (JSONL 审计日志)
├── auth_profiles.tsv         provider 默认 profile 映射
├── auth_sessions.tsv         认证会话存储
├── execution_cache.tsv       幂等执行缓存
├── storage_manifest.tsv      存储版本元数据
├── memory/
│   ├── task_log.tsv / step_log.tsv
│   ├── skill_stats.tsv / agent_stats.tsv
│   ├── lessons.tsv
│   ├── workflow_candidates.tsv / workflows.tsv
├── scheduler/
│   ├── tasks.tsv / runs.tsv
└── trust/
    ├── identities.tsv / allowlist.tsv
```

### 1.4 CI

- [ci.yml](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/.github/workflows/ci.yml): GitHub Actions，`push` + `pull_request` 触发
- 矩阵: `ubuntu-latest` + `windows-latest`
- 流程: Checkout → Ninja → (MSVC on Windows) → Configure → Build → CTest

---

## 2. plan.md Phase 逐项核查

### Phase A: 文档与规划 ✅ 全部完成

| 项目 | 状态 | 核实 |
|------|------|------|
| 创建 plan.md | ✅ | 文件存在，334 行，48KB |
| 同步 ROADMAP.md | ✅ | Last synced: 2026-04-25 |
| README 链接 plan.md | ✅ | README L84 引用 |
| 更新 ARCHITECTURE.md 目录布局 | ✅ | 含 auth/scheduler/trust/orchestration |
| 过时设计文档添加 Known Gaps | ✅ | ARCH_ALIGNMENT.md 有详细状态标注 |

---

### Phase B: Auth 完善 ✅ 全部完成，生产化仍有后续项

| 项目 | 状态 | 核实 |
|------|------|------|
| `auth refresh` 命令 | ✅ | provider_adapters.cpp 中有 refresh 路径 |
| **真实 OAuth refresh token exchange** | ✅ | 已有 refresh-token request 构建、curl-backed exchange helper、token response 解析、managed session/token 持久化与 provider adapter refresh 编排 |
| 工作区默认 profile 映射 | ✅ | auth_profile_store.cpp, runtime/auth_profiles.tsv, `auth profiles [provider]`, login/OAuth completion `set_default=true` |
| credential store 状态与 Windows 后端 | ✅ | Windows Credential Manager 已接入；非 Windows 仍明确标注 env-ref-only dev fallback |
| OAuth PKCE 骨架/明确推迟 | ✅ | 已实现 PKCE start/callback/listen/token exchange/session persistence，并补 Gemini 默认 OAuth 配置 |
| Auth 测试覆盖 | ✅ | auth_tests.cpp 21KB，CLI session fixture 覆盖 |

> [!NOTE]
> 原报告中“唯一显式未完成项”已过期：当前工作区已实现原生 OAuth authorization-code / refresh-token exchange helper、session persistence 和 Gemini 默认配置。剩余 Auth 生产化缺口集中在浏览器启动 UX、更广泛 provider discovery 与非 Windows credential store。

---

### Phase C: Scheduler 硬化 ✅ 全部完成

| 项目 | 状态 | 核实 |
|------|------|------|
| `schedule tick` 命令 | ✅ | scheduler.cpp 26KB 实现 |
| `schedule daemon` 模式 | ✅ | 复用 tick 执行路径的前台长运行 |
| Cron 表达式支持 | ✅ | 五字段 cron + 6 个别名 + DOM/DOW OR 语义 |
| retry/backoff 字段 | ✅ | ScheduledTask 含 max_retries/retry_backoff_seconds |
| Scheduler 执行元数据 | ✅ | runtime/scheduler/runs.tsv |
| missed/disabled/failed 测试 | ✅ | scheduler_tests.cpp 26KB |
| missed-run policy | ✅ | run-once / skip 两种策略 |

---

### Phase D: Workflow 演化 ✅ 全部完成

| 项目 | 状态 | 核实 |
|------|------|------|
| 持久化 WorkflowStore | ✅ | workflow_store.cpp, workflows.tsv |
| promote-workflow 命令 | ✅ | 含保存前校验 |
| Router 优先匹配 workflow | ✅ | WorkflowRouter 实现 |
| stored workflow 执行 | ✅ | workflow_run_skill.cpp |
| required-input 适用性检查 | ✅ | + input_equals/number/bool/regex/any/expr |
| LessonStore | ✅ | lesson_store.cpp, lessons.tsv |
| Lesson → Router hint | ✅ | 抑制重复失败 workflow |
| Lesson → Policy hint | ✅ | PolicyDenied 附加历史提示 |

> 额外完成了远超计划的功能: `show-workflow`, `clone-workflow`, `update-workflow`, `set-workflow-enabled`, `remove-workflow`, `validate-workflows`, `explain-workflow`, 以及 `input_expr` 嵌套布尔表达式。

---

### Phase E: Agent 与 Subagent 系统 ✅ 全部完成

| 项目 | 状态 | 核实 |
|------|------|------|
| Router 拆分 Skill/Agent/WorkflowRouter | ✅ | router_components.cpp |
| 自动 agent 候选选择 | ✅ | SubagentManager 基于 health/capability/stats/lessons |
| WorkspaceSession 抽象 | ✅ | workspace_session.cpp |
| 并发/成本限制 | ✅ | max_parallel_subagents + budget_limit |
| 确定性角色分配 + subtask objectives | ✅ | roles= + subtasks= |
| Provider adapters 扩展 | ✅ | Gemini + Anthropic + Qwen + local_planner |

> 实际 agent adapter 数量 **5 个** (local_planner, codex_cli, gemini, anthropic, qwen)，超过 PRD 要求的 "至少 1 个二级代理"。

---

### Phase F: CLI 与 Plugin 生态 ✅ 全部完成

| 项目 | 状态 | 核实 |
|------|------|------|
| 外部 CLI spec loader | ✅ | cli_spec_loader.cpp, runtime/cli_specs/*.tsv |
| jq_transform CLI skill | ✅ | 已实现 |
| 敏感参数脱敏 | ✅ | secret_redaction.cpp |
| 进程资源控制 | ✅ | Windows Job Object + POSIX setrlimit |
| Plugin Host manifest + stdio / JSON-RPC protocol | ✅ | TSV + JSON manifest、schema validator、health、execution 与 persistent session 已拆分到 focused modules |

---

### Phase G: 安全与策略 ✅ 全部完成

| 项目 | 状态 | 核实 |
|------|------|------|
| idempotency_key 强制执行 | ✅ | execution_cache.cpp |
| 高风险操作审批 | ✅ | approval_store.cpp |
| Role/user-level permission grants | ✅ | role_catalog.cpp |
| Trust/pairing/identity 审计 | ✅ | audit.log 中有记录 |
| Secret redaction 测试 | ✅ | 跨模块覆盖 |

---

### Phase H: 存储与可靠性 ✅ 全部完成

| 项目 | 状态 | 核实 |
|------|------|------|
| TSV vs SQLite 决策 | ✅ | 明确 TSV MVP，SQLite 推迟 |
| 文件锁/单写者 | ✅ | atomic_file.cpp 10KB |
| storage 版本元数据 | ✅ | storage_version_store.cpp, storage_manifest.tsv |
| storage migrate 助手 | ✅ | 含 legacy path 迁移 + schema 规范化 |
| crash-safe writes | ✅ | 临时文件 + 原子替换 + append-intent recovery |
| export/import 工具 | ✅ | storage_export.cpp + `storage verify [src=<directory>] [strict=true]` completeness diagnostics + pre-import backups + `storage backups` discoverability + `storage restore-backup` rollback；recover 已隔离 corrupt committed transaction 并报告 `failed=`；atomic/append helpers 已有同进程并发写测试 |

---

### Phase I: 测试与 CI ✅ 全部完成

| 项目 | 状态 | 核实 |
|------|------|------|
| 模块化单元测试拆分 | ✅ | 从单一 smoke 拆为 11 个目标 |
| CLI 集成测试 | ✅ | cli_integration_tests.cpp 55KB |
| 负面路径测试 | ✅ | policy/trust/scheduler/auth |
| GitHub Actions CI | ✅ | ci.yml, Windows + Ubuntu |
| 跨平台命令夹具 | ✅ | test_command_fixtures.hpp |

---

## 3. PRD 需求交叉核查

基于 [docs/PRD.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/docs/PRD.md) 第 5 节 "MVP 范围":

| PRD MVP 要求 | 实现状态 | 备注 |
|-------------|---------|------|
| Agent loop | ✅ | AgentLoop 完整实现 |
| Skill Registry | ✅ | skill_registry.cpp |
| Agent Registry | ✅ | agent_registry.cpp |
| Router | ✅ | 已拆分为三个子路由器 |
| Memory 基础版 | ✅ | 远超基础——含 Lesson/Workflow/Stats |
| Policy 基础版 | ✅ | 远超基础——含 Role/Approval/Trust |
| file_read / file_write / file_patch | ✅ | 3 个文件 skill |
| http_fetch | ✅ | http_fetch_skill.cpp |
| workflow_run | ✅ | 含 stored workflow 执行 |
| CLI Host | ✅ | + 外部 spec + 资源限制 |
| 1 个二级代理接入 | ✅ | 实际 5 个 adapter |
| 认证子系统基础版 | ✅ | 多 provider + 多模式 |
| 任务事件日志 | ✅ | task_log.tsv + step_log.tsv |
| Workflow 自动生成骨架 | ✅ | candidate + scoring + promotion |
| Skill/Agent 基础评分 | ✅ | skill_stats.tsv + agent_stats.tsv |

> [!TIP]
> **PRD MVP 范围已 100% 覆盖**，多数模块超额完成。

---

## 4. 剩余缺口汇总

按优先级排列：

### 🔴 高优先级（生产化阻塞）

| 缺口 | 所属模块 | 影响 |
|------|---------|------|
| OAuth provider-specific UX/config | Auth | PKCE start/callback URL 解析校验、一次性 localhost callback listener、authorization-code/refresh-token request 构建、curl-backed exchange helper、response 解析、managed AuthSession persistence helper、single-command `oauth-login`、scriptable oauth-complete、Gemini Google OAuth 默认 endpoint/scope、repo-local `runtime/auth_oauth_providers.tsv` defaults 覆盖、`oauth-config-validate`、`oauth-defaults` 和参数化 provider adapter 原生 login/refresh 已落地，但仍缺更广泛 provider discovery 和完整多 provider 交互式 login UX |
| 非 Windows 系统 credential store 集成 | Auth | Windows 已接入 Credential Manager；macOS/Linux 仍为 env-ref-only dev fallback |
| Plugin 进程池与 lifecycle admin UX | Plugin Host | persistent `json-rpc-v0` session manager 已实现 MVP，并覆盖 crash/restart、session close、idle restart、LRU eviction 与 lifecycle_event；模块拆分、workspace-configurable session cap、plugin_host config diagnostics、`plugins inspect name=<plugin> [health=true]` 和 `plugins lifecycle` 已完成；新增 `PluginSpec.pool_size` 每 plugin 进程池上限（受全局 `max_persistent_sessions` 约束）和 `agentos plugins sessions` / `session-restart` / `session-close` 运行时 session admin 命令，剩余主要是 daemon 化跨进程 session 共享与更细的 OS 级隔离策略 |
| 更强 Plugin sandbox 模型 | Plugin Host | 已有 `sandbox_mode=workspace|none` 的路径参数约束；更强隔离仍需 OS sandbox/cgroup/job 策略 |

### 🟡 中优先级（功能增强）

| 缺口 | 所属模块 | 影响 |
|------|---------|------|
| 更复杂的模型驱动任务拆分 | Subagent | 当前 auto_decompose 仍以 planner `plan_steps` 映射为主 |
| Timezone/DST calendar hardening | Scheduler | 跨时区/夏令时边界场景 |
| Windows file-handle limits | CLI Host | POSIX 有 RLIMIT_NOFILE，Windows 缺少对应 |
| 更丰富的 JSON Schema 关键字 | Skill/Plugin | 已覆盖大部分常用关键字，仍可扩展 |
| SQLite 或跨格式存储迁移 | Storage | TSV 已明确为 MVP backend；多文件 prepare/commit/recover helper 已补齐事务性维护入口；`ADR-STORAGE-001` 已定义 SQLite 迁移边界 |

### 🟢 低优先级（UX 打磨）

| 缺口 | 所属模块 |
|------|---------|
| 更丰富的 workflow definition 编辑 UX | Memory |
| 更丰富的设备管理 admin UX | Trust，当前已有 `trust device-show` 单设备查询 |
| 更丰富的 Role/Approval admin UX | Policy，当前已有 role/user-role/approval 单项 show 命令 |
| 部分旧设计文档与实现的持续同步 | Docs |
| 扩展 failure/regression 测试套件 | Tests |

---

## 5. 文档一致性审查

| 文档 | 与代码一致性 | 问题 |
|------|------------|------|
| [README.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/README.md) | ✅ 高 | 当前实现状态段落与代码吻合，CLI 示例可执行 |
| [plan.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/plan.md) | ✅ 高 | Completion Snapshot 与代码吻合，Progress Log 详尽 |
| [ROADMAP.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/docs/ROADMAP.md) | ✅ 高 | Last synced: 2026-04-25，状态标记与代码吻合 |
| [ARCH_ALIGNMENT.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/docs/ARCH_ALIGNMENT.md) | ✅ 高 | 逐模块状态与实际代码核实一致 |
| [PRD.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/docs/PRD.md) | ⚠️ 中 | PRD 列出的 OAuth / 多账号 / session 型工具在 Auth 仍有缺口 |
| [AUTH_PRD.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/docs/AUTH_PRD.md) | ⚠️ 中 | 描述了 OAuth/refresh/cloud credentials，代码尚未完全实现 |
| [AUTH_DESIGN.md](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/docs/AUTH_DESIGN.md) | ⚠️ 中 | 同上，已有 Known Gaps 标注 |

---

## 6. 架构健康度

### ✅ 做得好的方面

1. **模块化清晰**: `core/` / `auth/` / `hosts/` / `memory/` / `scheduler/` / `skills/` / `trust/` / `storage/` / `utils/` 分层明确
2. **接口先行**: `ISkill`、`IAgentAdapter` 抽象良好，新 adapter 接入成本低
3. **防御性编程**: 敏感值脱敏、unknown permission deny、idempotency enforcement、schema validation 层层防护
4. **测试工程化**: 从单体 smoke 拆分为 11 个模块化测试目标，跨平台 CI 覆盖
5. **渐进演化**: plan.md + ROADMAP 驱动的增量开发，没有推翻重写
6. **运行时可观测**: audit.log + storage status + memory stats + plugins health 提供充分可见性

### ⚠️ 需要关注的方面

1. **结构化 JSON 依赖仍未接入**: 当前仍靠本地 `json_utils` 和专用解析/校验路径支撑 schema、provider 与 plugin 协议，继续扩关键字前应评估 nlohmann/json 等依赖。
2. **TSV 作为 MVP 存储后端**: 对复杂查询和跨进程并发仍有固有限制；已有 atomic write、append-intent recovery、prepare/commit/recover 和 ADR-STORAGE-001，SQLite 仍按边界延后。
3. **Auth 交互 UX 仍需产品化**: 已有 `oauth-login` 单命令 PKCE/token exchange/session persistence 和可选浏览器启动，但尚不是完整产品级多 provider login 流。
4. **Plugin 隔离仍是路径约束级别**: `sandbox_mode=workspace|none` 能限制路径参数，但不是 OS sandbox/cgroup/job 完整隔离。

---

## 7. 分模块完成度雷达

```
Core Runtime        ████████████████████ 95%  ← 缺 failure recovery
Builtin Skills      ████████████████████ 95%  ← 缺更多 Schema 关键字
CLI Integration     ██████████████████░░ 90%  ← 缺 Windows file-handle limits
Agent System        █████████████████░░░ 85%  ← 缺更复杂的模型驱动任务拆分
Auth System         ██████████████████░░ 91%  ← 缺更广泛 provider discovery / 非 Windows credential store / 完整多账号策略
Memory & Evolution  ██████████████████░░ 90%  ← 基本完备
Identity / Trust    ██████████████████░░ 90%  ← 缺 admin UX
Scheduler           ██████████████████░░ 90%  ← 缺 timezone/DST
Plugin Host         ██████████████████░░ 90%  ← 已补 per-plugin pool_size + sessions/session-restart/session-close 管理；剩余 daemon 跨进程 session 共享 / OS sandbox
Storage             ██████████████████░░ 91%  ← 缺跨格式迁移和更完整 audit recovery
Tests & CI          ██████████████████░░ 90%  ← 已相当充分
Documentation       ██████████████████░░ 90%  ← 仍需随剩余生产化项持续更新
```

---

## 8. 结论与建议

### 总体判断

> **AgentOS 作为本地 MVP 已达到设计目标**。plan.md 中 Phase A-I 已完成，Post-Review Phase J-N 的主要架构债也已处理大半。PRD MVP 范围已覆盖，多数模块超额交付；剩余工作更偏生产化和依赖治理。

### 后续优先建议

1. **Auth 硬化** — 如果需要脱离外部 CLI 独立工作，更广泛 OAuth provider-specific UX/config、非 Windows 系统 credential store 和完整多账号策略仍是第一优先级
2. **结构化 JSON 依赖评估** — 在继续扩 schema/provider/plugin 协议前，评估替换手写 JSON 路径的成本和迁移切面
3. **Plugin 进程池与隔离策略** — 在现有 persistent session 和 LRU cap 基础上补更细 pool policy、admin UX 与 OS 级隔离边界
4. **存储升级路径** — 保持 TSV 接口兼容，为 SQLite 迁移预留抽象层
5. **持续同步文档** — AUTH_PRD / AUTH_DESIGN 需在 Auth 硬化后更新

---

*此报告基于对项目全部源代码、测试、文档和运行时状态的审查生成。*

---

## 9. Codex 复核修正（2026-04-25）

复核结论：本报告的总体判断基本正确，但有几处已被当前工作区实现更新覆盖。

### 仍然正确

- 原生 OAuth PKCE 的 start/callback URL 解析校验、一次性 localhost callback listener、authorization-code/refresh-token request 构建、curl-backed exchange helper、response 解析、managed AuthSession persistence helper、scriptable oauth-complete、single-command oauth-login、Gemini Google OAuth 默认 endpoint/scope、repo-local OAuth defaults 覆盖、`oauth-config-validate` 和参数化 provider adapter 原生 login/refresh 已落地；更广泛 provider discovery、多 provider 产品级 UX 和非 Windows credential store 仍未完成。
- `SecureTokenStore` 在 Windows 上已接入 Credential Manager；非 Windows 平台仍是 `env-ref-only` dev fallback。
- Plugin Host 长驻可复用进程生命周期已实现；剩余是更深 process-pool policy、admin UX 与 OS sandbox。
- TSV 仍是 MVP 存储后端，事务型后端和更广义跨格式迁移仍未完成。
- `main.cpp` 命令组和 `plugin_host.cpp` 主要职责均已拆分；后续关注单个命令组文件继续增长的问题。

### 已过期或需收窄

- “Plugin sandbox 模型缺失”已过期：当前 `PluginSpec` 已支持 `sandbox_mode=workspace|none`，默认 workspace 模式会拒绝解析到 workspace 外的 path/file/dir 类运行参数。
- “Phase F: CLI 与 Plugin 生态全部完成”应收窄为 MVP 完成；Plugin Host 已补长驻进程生命周期，但生产化仍受 process-pool policy、admin UX 与 OS sandbox 限制。
- “Phase H: 存储与可靠性全部完成”应收窄为 MVP 完成；事务语义、SQLite/跨格式迁移和完整审计恢复仍是开放项。
- Plugin Host 完成度可从 75% 上调到约 85%，但不应标为生产级完成。

### 新开发计划入口

新的分阶段计划已写入 [`plan.md`](file:///c:/Users/rogers/Downloads/agentos_docs_bundle/agentos/plan.md) 的 “Post-Review Development Plan”：

1. Phase J: Auth Production Hardening（已完成 Windows Credential Manager 后端、PKCE/token exchange/session persistence/callback listener/scriptable completion/single-command oauth-login/provider adapter 参数化编排与 Gemini Google OAuth 默认配置；仍待更广泛 provider discovery、多 provider 产品级 UX 与非 Windows credential store）
2. Phase K: Plugin Long-Running Lifecycle
3. Phase L: Storage Reliability
   - ✅ `storage recover` now isolates corrupt committed transaction state, reports `failed=`, avoids partial staged-payload application, and continues replaying later valid transactions.
4. Phase M: Agent Result Normalization（已完成 `agent_result.v1` adapter 输出与 SubagentManager normalized 聚合）
5. Phase N: CLI And Docs Maintenance

---

## 10. Opus 审核复核修正（2026-04-26）

复核结论：该审核的大方向正确，尤其是对 Auth UX、ExecutionCache 语义、main/plugin 大文件、PolicyEngine 依赖形态、Router 文件边界、AgentLoop/SubagentManager 平行执行路径和 Lesson 语义偏弱的判断成立。以下几点需要按当前工作区修正：

- 已过期：Phase M 并非“尚未开始”。当前 Codex CLI、Gemini、Anthropic、Qwen、local_planner 已输出 `agent_result.v1`，SubagentManager 已聚合 `agent_outputs[].normalized` 并保留 raw provider output。
- 部分成立：AUTH_PRD/AUTH_DESIGN 已有“当前实现状态与缺口”，但对“脚本化 PKCE 半流程，不是产品级浏览器登录 UX”的提示不够醒目，已补充明确警示。
- 已修正：ExecutionCache 现在以 `idempotency_key` + task input/context fingerprint 共同决定缓存命中；同 key 不同 inputs/context 会重新执行，同时兼容读取旧 9 列 TSV 缓存。
- 成立：auto_decompose 过去主要覆盖 happy path，已补充 planner 失败与 planner 输出缺少 `plan_steps[].action` 的负面路径测试。
- 已大部分完成：拆分 `main.cpp`、拆分 `plugin_host.cpp`、收敛 PolicyEngine 构造、抽 shared step lifecycle、抽 LessonHintProvider 均已落地；仍待评估引入结构化 JSON 依赖。
- 2026-04-26：补齐 Plugin Host per-plugin process pool 与 admin UX——`PluginSpec.pool_size` 受全局 `max_persistent_sessions` 约束、`PluginHost::list_sessions/close_sessions_for_plugin/restart_sessions_for_plugin`，并新增 `agentos plugins sessions / session-restart / session-close` 子命令、TSV/JSON manifest pool_size 解析及 pool/admin 回归测试。
