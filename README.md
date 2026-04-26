# AgentOS

AgentOS 是一个 **基于 C++ 的 AI Agent Operating System 内核**，目标是构建一个：

- 有稳定基础能力的核心运行时
- 能调用外部 CLI / 插件工具的执行系统
- 能借助外部“专家型二级代理”的协作系统
- 能在执行中复盘并逐步成长的记忆与进化系统

## 一句话定义

> Minimal core, managed tools, external experts, continuous learning.

## 核心设计理念

AgentOS 不追求把所有能力都塞进内核，而是采用以下原则：

1. **最小稳定内核**
   - 负责任务循环、路由、策略、记忆、审计、生命周期控制
   - 尽量不承担高风险、重依赖、重状态的能力

2. **外部化能力**
   - 基础工具通过 Skill 接入
   - 各类现成工具通过 CLI Host 接入
   - 多语言能力通过 Plugin Host 接入
   - 专家型 AI 工具通过二级代理接口接入

3. **执行中学习**
   - 记录任务、步骤、结果、代价、成功率
   - 从成功案例中生成 Workflow
   - 从失败案例中提炼约束与经验
   - 持续优化 Skill 与 Agent 的路由策略

## 主要能力

- 统一 Skill 抽象
- 统一二级代理抽象
- CLI 工具纳管
- OAuth / API Key / 官方 CLI 登录态复用
- 结构化记忆
- Workflow 自动沉淀
- 基于评分的路由优化

## 当前实现状态

已完成最小 C++20/CMake 工程骨架：

- AgentLoop / Router（SkillRouter / AgentRouter / WorkflowRouter）/ PolicyEngine / PermissionModel（含 permission_grants、持久 RoleCatalog 与高风险 ApprovalStore 校验）/ AuditLogger（含敏感值脱敏）
- SkillRegistry / AgentRegistry
- MemoryManager 基础任务记录与评分骨架
- 内建 Skill：file_read / file_write / file_patch / http_fetch / workflow_run
- CLI Host：受控 cwd、timeout、stdout/stderr 捕获、输出限流、env 白名单、敏感参数脱敏，Windows Job Object 与 POSIX `setrlimit` 资源限制（如 `max_processes` / `memory_limit_bytes` / `cpu_time_limit_seconds`）
- CLI Skill：rg_search / git_status / git_diff / jq_transform / curl_fetch
- Plugin Host：`runtime/plugin_specs/*.tsv` 与 `runtime/plugin_specs/*.json` 可声明 repo-local `plugin.v1` / `stdio-json-v0` / `json-rpc-v0` 插件 skill，manifest 必须声明 `process.spawn` 权限，`stdio-json-v0` 成功执行的 stdout 必须是 JSON object；`json-rpc-v0` 成功执行的 stdout 必须是 JSON-RPC 2.0 response 且 `result` 为 JSON object，并会按 `output_schema_json.required`、`properties.*.type`、string `const` / `enum` / length / pattern、`additionalProperties:false`、numeric range / `multipleOf` 校验后作为结构化 `plugin_output` 返回，同时保留原始 stdout，并透传 CLI 资源限制字段；manifest 支持 `sandbox_mode=workspace|none`、`lifecycle_mode=oneshot|persistent`、`startup_timeout_ms` 和 `idle_timeout_ms`，其中 `persistent` 当前要求 `json-rpc-v0`；`runtime/plugin_host.tsv` 可配置 `max_persistent_sessions`；`agentos plugins` / `validate` / `health` 会显示 loaded plugin 的 source file/line 和 lifecycle 字段，`agentos plugins lifecycle` 汇总 oneshot/persistent 配置、当前 session cap 和 plugin host config diagnostics，`agentos plugins inspect name=<plugin> [health=true]` 可脚本化查看单个 manifest 的协议、权限、参数、资源限制、sandbox 和 lifecycle，并可按需执行单插件 health probe；`agentos plugins health` 支持 binary 可用性检查和可选 `health_args_template` 探针，探针不能引用运行期输入占位符；无效外部 CLI/plugin spec 会在启动时写入 `runtime/audit.log`
- 本地规划二级代理：local_planner（离线确定性分析、任务拆解、会话状态和结构化计划输出）
- Codex CLI 二级代理适配器：codex_cli（显式 target 调用）
- Model provider agents：Gemini、Anthropic、Qwen 均可通过现有 auth session/default profile 调用；Qwen 使用 Alibaba Cloud Model Studio OpenAI-compatible Chat Completions
- Auth 子系统：AuthManager / Provider Adapter / SessionStore / SecureTokenStore / CredentialBroker
- Auth Provider：OpenAI/Codex、Anthropic/Claude、Gemini、Qwen
- Gemini CLI OAuth / Google ADC passthrough：可导入已登录的 `gemini` CLI 浏览器 OAuth 会话，或通过 `gcloud auth application-default print-access-token` 使用 Google ADC，并通过 `target=gemini` 调用
- CLI session passthrough：Codex CLI / Claude CLI 登录态探测与导入
- API key profile：通过环境变量引用保存，不落明文 key
- Credential Store：Windows 使用 Credential Manager 存储托管 token；其他平台当前为 `env-ref-only` dev fallback
- Workspace auth profile：`runtime/auth_profiles.tsv` 保存 provider 默认 profile 映射
- Identity / Trust：IdentityManager、远程触发身份字段、PairingManager、PairingInviteStore、TrustPolicy、AllowlistStore、RoleCatalog
- Remote Trigger Policy：远程任务默认拒绝，必须先 pairing 并具备 `task.submit`
- Pairing Handshake：`agentos trust invite-create` 可创建带 TTL 的一次性配对邀请，`invite-accept` 消费 token 后写入 allowlist，`invites` 仅列出未过期未消费邀请
- Device Lifecycle：allowlist 记录 paired / last_seen 时间戳，`trust device-show` / `device-label` / `device-seen` / `block` / `unblock` / `remove` 可查询和管理已配对设备
- Approval Workflow：`runtime/trust/approvals.tsv` 保存审批请求，`trust approval-request` / `approval-approve` / `approval-revoke` / `approvals` 管理高风险执行审批；CLI runtime 中高风险执行必须引用已批准的 `approval_id`
- Trust/Policy Admin：`trust device-show` / `role-show` / `user-role-show` / `approval-show` 可脚本化查询单个设备、角色、用户授权和审批记录，并对缺失项返回非 0
- Idempotent Execution：`idempotency_key` + task fingerprint + `runtime/execution_cache.tsv`，直接副作用 Skill 必须带 key，Scheduler 会按 run 自动生成 key；缓存命中需要 key 与任务输入/上下文指纹同时匹配，同 key 不同 inputs 会重新执行
- Persistent Memory：TaskLog / StepLog / Skill-Agent Stats / LessonStore / Workflow Candidates / WorkflowStore promotion
- Lesson Hints：重复 workflow 失败会抑制自动 workflow，重复 agent 失败会降低 agent 路由优先级，重复 PolicyDenied 会附加历史失败提示
- Workflow Generator / Scoring：基于历史 Task/Step 生成候选 workflow，可带 `required_inputs` / `input_equals` / 数值范围 / 布尔输入 / 正则输入 / `input_any` 复合 OR 条件 / `input_expr` 嵌套布尔表达式 promote 到 WorkflowStore，并由 Router 自动优先执行
- Workflow Validation：`agentos memory validate-workflows` 可校验已持久化 workflow 定义，`promote-workflow` 会在保存前拒绝格式错误的适用条件
- Workflow Explainability：`agentos memory explain-workflow <name> ...` 可逐项解释某个 workflow 对当前输入是否适用
- Agent Scoring：Router 可基于历史 success_rate / latency 选择 agent
- External CLI Specs：`runtime/cli_specs/*.tsv` 可声明 repo-local CLI skill，并启动时动态注册
- Scheduler：一次性 / interval 任务持久化，`schedule run-due` / `schedule tick` / `schedule daemon` 复用 AgentLoop 执行，支持 retry/backoff、missed-run policy、五字段 cron、`@hourly` / `@daily` / `@weekly` / `@monthly` / `@yearly` / `@annually` 别名，以及 day-of-month / day-of-week OR 语义，并记录独立 run history
- Subagent Orchestration：显式或自动候选 agent 的 sequential / parallel 编排，支持 `roles=agent:role` 确定性角色分配、`subtasks=role_or_agent=objective;...` 或 `subtask_<agent|role>=...` 的 per-agent objective 分派；也支持 `auto_decompose=true` 调用具备 `decomposition` capability 的规划 agent 生成 plan_steps 并映射到 subtask objective；subagent step 会保留 agent structured output，并在总输出中聚合 `agent_outputs[].normalized` 的 `agent_result.v1` 结果；并发/成本限制与 WorkspaceSession 基础生命周期，复用 Policy / Audit / Memory
- CTest 模块化测试：11 个测试目标覆盖 CLI integration、storage、auth、agent provider、policy/trust、scheduler、workflow/router、subagent/session、CLI/plugin、shared spec parsing、file skill/policy

## 开发计划

当前完成度审查与后续 TODO 维护在 [plan.md](plan.md)。每完成一项能力，应同步更新该文件和相关 docs。
当前状态是可运行的本地 MVP，不是生产级完成；Gemini 已支持复用 Gemini CLI 的浏览器 OAuth 登录态、原生 PKCE OAuth 登录、`oauth-defaults` OAuth 配置查询、repo-local `runtime/auth_oauth_providers.tsv` OAuth defaults 覆盖、`oauth-start open_browser=true` 系统浏览器启动尝试、`oauth-login` start/listen/token-exchange/session-persist 单命令编排和 Google OAuth 默认 endpoint/scope，Windows 已接入 Credential Manager，但生产化仍需补齐更完整的多 provider 交互式登录 UX、非 Windows credential store、存储事务/恢复和更完整的 agent orchestration。
仓库已包含 GitHub Actions CI，默认在 `push` 和 `pull_request` 上执行 Windows + Ubuntu 的 `configure/build/test`。

## 构建与运行

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
build\agentos.exe demo
```

如果当前 shell 没有加载 MSVC 环境，可先执行：

```powershell
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 && cmake -S . -B build -G Ninja && cmake --build build'
```

本地 CI 验证可直接复用以下命令：

Windows:

```powershell
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 && cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure'
```

Linux/macOS:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

常用命令：

```powershell
build\agentos.exe run read_file path=README.md
build\agentos.exe run workflow_run workflow=write_patch_read path=runtime/wf.txt content=hello find=hello replace=done idempotency_key=demo-wf-1
build\agentos.exe run write_file path=runtime/note.txt content=hello idempotency_key=demo-write-1
build\agentos.exe cli-demo
build\agentos.exe agents
build\agentos.exe cli-specs validate
build\agentos.exe plugins
build\agentos.exe plugins validate
build\agentos.exe plugins health
build\agentos.exe plugins lifecycle
build\agentos.exe plugins inspect name=cli_plugin health=true
build\agentos.exe memory summary
build\agentos.exe memory stats
build\agentos.exe memory workflows
build\agentos.exe memory promote-workflow write_file_workflow required_inputs=path,content input_equals=mode=workflow input_number_gte=priority=5 input_number_lte=size=10 input_bool=approved=true input_regex=branch=release/.* input_any=equals:mode=workflow^|equals:mode=automated input_expr="equals:mode=workflow&&(exists:ticket||regex:branch=release/.*)"
build\agentos.exe memory stored-workflows
build\agentos.exe memory lessons
build\agentos.exe storage status
build\agentos.exe storage backups
build\agentos.exe storage restore-backup name=import-123
build\agentos.exe storage verify strict=true
build\agentos.exe storage verify src=runtime_export strict=true
build\agentos.exe storage import src=runtime_export
build\agentos.exe run workflow_run workflow=write_file_workflow path=runtime/promoted.txt content=hello idempotency_key=demo-promoted-1
build\agentos.exe run write_file path=runtime/auto_promoted.txt content=hello idempotency_key=demo-auto-promoted-1
build\agentos.exe trust identity-add identity=phone user=local-user label=dev-phone
build\agentos.exe trust identities
build\agentos.exe trust pair identity=phone device=device1 label=dev-phone permissions=task.submit
build\agentos.exe trust list
build\agentos.exe trust device-show identity=phone device=device1
build\agentos.exe trust role-set role=reader permissions=filesystem.read
build\agentos.exe trust user-role user=alice roles=reader
build\agentos.exe trust roles
build\agentos.exe trust role-show role=reader
build\agentos.exe trust user-role-show user=alice
build\agentos.exe trust approval-request subject=critical-write reason=operator-check requested_by=alice
build\agentos.exe trust approval-show approval=APPROVAL_ID
build\agentos.exe trust user-role-remove user=alice
build\agentos.exe trust role-remove role=reader
build\agentos.exe run read_file path=README.md remote=true origin_identity=phone origin_device=device1
build\agentos.exe trust block identity=phone device=device1
build\agentos.exe trust remove identity=phone device=device1
build\agentos.exe trust identity-remove identity=phone
build\agentos.exe schedule add id=demo-once task=write_file due=now path=runtime/scheduled.txt content=hello
build\agentos.exe schedule add id=demo-every task=write_file due=now recurrence=every:5m path=runtime/every.txt content=hello
build\agentos.exe schedule add id=demo-cron task=write_file cron="*/5 * * * *" path=runtime/cron.txt content=hello
build\agentos.exe schedule add id=demo-cron-hourly task=write_file cron=@hourly path=runtime/cron-hourly.txt content=hello
build\agentos.exe schedule add id=demo-cron-yearly task=write_file cron=@yearly path=runtime/cron-yearly.txt content=hello
build\agentos.exe schedule add id=demo-retry task=write_file due=now max_retries=1 retry_backoff_seconds=60 path=runtime/retry.txt
build\agentos.exe schedule add id=demo-skip task=write_file due=now recurrence=every:5m missed_run_policy=skip path=runtime/skip.txt
build\agentos.exe schedule run-due
build\agentos.exe schedule tick iterations=1 interval_ms=0
build\agentos.exe schedule daemon iterations=1 interval_ms=0
build\agentos.exe schedule history
build\agentos.exe schedule list
build\agentos.exe schedule remove id=demo-once
build\agentos.exe subagents run agents=local_planner mode=sequential objective=Plan_the_next_phase
build\agentos.exe subagents run agents=local_planner mode=sequential roles=local_planner:planner objective=Plan_with_explicit_role
build\agentos.exe subagents run agents=local_planner mode=sequential roles=local_planner:planner "subtasks=planner=Draft an implementation plan" objective=Coordinate_decomposed_work
build\agentos.exe subagents run mode=sequential task=analysis objective=Auto_select_an_agent
build\agentos.exe subagents run agents=local_planner mode=parallel budget_limit=1 objective=Budgeted_parallel_plan
build\agentos.exe subagents run agents=local_planner mode=parallel objective=Parallel_plan_smoke
build\agentos.exe auth providers
build\agentos.exe auth profiles qwen
build\agentos.exe auth credential-store
build\agentos.exe auth oauth-config-validate
build\agentos.exe auth oauth-start gemini client_id=CLIENT_ID redirect_uri=http://127.0.0.1:48177/callback
build\agentos.exe auth oauth-login gemini client_id=CLIENT_ID redirect_uri=http://127.0.0.1:48177/callback port=48177 open_browser=true profile=default set_default=true
build\agentos.exe auth oauth-callback callback_url=http://127.0.0.1:48177/callback?code=CODE^&state=STATE state=STATE
build\agentos.exe auth oauth-listen state=STATE port=48177 timeout_ms=120000
build\agentos.exe auth oauth-complete gemini callback_url=http://127.0.0.1:48177/callback?code=CODE^&state=STATE state=STATE code_verifier=VERIFIER redirect_uri=http://127.0.0.1:48177/callback client_id=CLIENT_ID profile=default
build\agentos.exe auth login gemini mode=browser_oauth callback_url=http://127.0.0.1:48177/callback?code=CODE^&state=STATE state=STATE code_verifier=VERIFIER redirect_uri=http://127.0.0.1:48177/callback client_id=CLIENT_ID profile=default
build\agentos.exe auth oauth-token-request token_endpoint=https://oauth2.googleapis.com/token client_id=CLIENT_ID redirect_uri=http://127.0.0.1:48177/callback code=CODE code_verifier=VERIFIER
build\agentos.exe auth oauth-refresh-request token_endpoint=https://oauth2.googleapis.com/token client_id=CLIENT_ID refresh_token=REFRESH_TOKEN
build\agentos.exe auth status
build\agentos.exe auth probe openai
build\agentos.exe auth login openai mode=cli-session
build\agentos.exe auth default-profile qwen profile=work
build\agentos.exe auth login qwen mode=api-key api_key_env=QWEN_API_KEY profile=default set_default=true
build\agentos.exe auth login gemini mode=api-key api_key_env=GEMINI_API_KEY profile=default
build\agentos.exe auth login gemini mode=browser_oauth profile=default
build\agentos.exe auth default-profile gemini profile=default
build\agentos.exe auth refresh qwen profile=default
build\agentos.exe run jq_transform filter=. path=runtime/data.json
build\agentos.exe run http_fetch url=https://example.com allow_network=true
build\agentos.exe run analysis target=gemini objective=Explain_the_current_workspace
build\agentos.exe run analysis target=gemini model=gemini-3.1-pro-preview objective=Run_a_provider_smoke_test timeout_ms=120000
build\agentos.exe run analysis target=qwen model=qwen-plus objective=Explain_the_current_workspace timeout_ms=120000
build\agentos.exe run analysis target=codex_cli objective=Review_the_project_structure
```

`runtime/auth_oauth_providers.tsv` 可覆盖或补充 OAuth defaults，并可用 `auth oauth-config-validate` 校验；字段为 `provider<TAB>authorization_endpoint<TAB>token_endpoint<TAB>scopes`，例如：

```text
gemini	https://accounts.example.test/custom-auth	https://accounts.example.test/custom-token	openid,email
```

审计日志默认写入：

```text
runtime/audit.log
runtime/auth_profiles.tsv
runtime/auth_oauth_providers.tsv
runtime/execution_cache.tsv
runtime/plugin_host.tsv
runtime/trust/identities.tsv
runtime/trust/allowlist.tsv
runtime/trust/roles.tsv
runtime/scheduler/tasks.tsv
runtime/scheduler/runs.tsv
runtime/memory/task_log.tsv
runtime/memory/step_log.tsv
runtime/memory/skill_stats.tsv
runtime/memory/agent_stats.tsv
runtime/memory/lessons.tsv
runtime/memory/workflow_candidates.tsv
runtime/memory/workflows.tsv
runtime/cli_specs/*.tsv
runtime/plugin_specs/*.tsv
runtime/plugin_specs/*.json
```

## 项目结构

```text
agentos/
├── README.md
├── CMakeLists.txt
├── src/
│   ├── core/
│   │   ├── orchestration/
│   ├── hosts/
│   ├── memory/
│   ├── scheduler/
│   ├── skills/
│   ├── trust/
│   └── utils/
├── docs/
│   ├── PRD.md
│   ├── ARCHITECTURE.md
│   ├── AUTH_PRD.md
│   ├── AUTH_DESIGN.md
│   ├── SKILL_SYSTEM.md
│   ├── AGENT_SYSTEM.md
│   ├── MEMORY_EVOLUTION.md
│   ├── CLI_INTEGRATION.md
│   ├── ROADMAP.md
│   └── CODING_GUIDE.md
```

## 推荐起步顺序

1. 先完成文档与接口定义
2. 先实现最小内核和 5 个基础 Skill
3. 接入 CLI Host
4. 接入 1 个二级代理
5. 接入认证子系统
6. 实现记忆与 Workflow 生成
7. 做路由评分与演化系统

## 适用场景

- AI Agent 框架研发
- 自动化开发平台
- 多工具 / 多模型协作系统
- 长期演化型个人或团队 Agent

## 命名

本项目名称可以正式使用：

**AgentOS**

## License

建议使用 MIT 或 Apache-2.0。
