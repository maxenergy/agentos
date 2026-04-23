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

- AgentLoop / Router / PolicyEngine / PermissionModel / AuditLogger
- SkillRegistry / AgentRegistry
- MemoryManager 基础任务记录与评分骨架
- 内建 Skill：file_read / file_write / file_patch / http_fetch / workflow_run
- CLI Host：受控 cwd、timeout、stdout/stderr 捕获、输出限流、env 白名单
- CLI Skill：rg_search / git_status / git_diff / curl_fetch
- Mock 二级代理：mock_planner
- Codex CLI 二级代理适配器：codex_cli（显式 target 调用）
- Auth 子系统：AuthManager / Provider Adapter / SessionStore / SecureTokenStore / CredentialBroker
- Auth Provider：OpenAI/Codex、Anthropic/Claude、Gemini、Qwen
- CLI session passthrough：Codex CLI / Claude CLI 登录态探测与导入
- API key profile：通过环境变量引用保存，不落明文 key
- Identity / Trust：IdentityManager、远程触发身份字段、PairingManager、TrustPolicy、AllowlistStore
- Remote Trigger Policy：远程任务默认拒绝，必须先 pairing 并具备 `task.submit`
- Idempotent Execution：`idempotency_key` + `runtime/execution_cache.tsv`
- Persistent Memory：TaskLog / StepLog / Skill-Agent Stats / Workflow Candidates
- Workflow Generator / Scoring：基于历史 Task/Step 生成候选 workflow，并计算成功率、失败数、耗时与综合评分
- Agent Scoring：Router 可基于历史 success_rate / latency 选择 agent
- Scheduler：一次性 / interval 任务持久化，`schedule run-due` 复用 AgentLoop 执行
- Subagent Orchestration：显式 agent 列表的 sequential / parallel 编排，复用 Policy / Audit / Memory
- CTest smoke test：覆盖核心 loop、策略拒绝、权限模型、远程 pairing、workflow、scheduler、subagent 编排

## 开发计划

当前完成度审查与后续 TODO 维护在 [plan.md](plan.md)。每完成一项能力，应同步更新该文件和相关 docs。

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

常用命令：

```powershell
build\agentos.exe run read_file path=README.md
build\agentos.exe run workflow_run workflow=write_patch_read path=runtime/wf.txt content=hello find=hello replace=done
build\agentos.exe run write_file path=runtime/note.txt content=hello idempotency_key=demo-write-1
build\agentos.exe cli-demo
build\agentos.exe agents
build\agentos.exe memory summary
build\agentos.exe memory stats
build\agentos.exe memory workflows
build\agentos.exe trust identity-add identity=phone user=local-user label=dev-phone
build\agentos.exe trust identities
build\agentos.exe trust pair identity=phone device=device1 label=dev-phone permissions=task.submit
build\agentos.exe trust list
build\agentos.exe run read_file path=README.md remote=true origin_identity=phone origin_device=device1
build\agentos.exe trust block identity=phone device=device1
build\agentos.exe trust remove identity=phone device=device1
build\agentos.exe trust identity-remove identity=phone
build\agentos.exe schedule add id=demo-once task=write_file due=now path=runtime/scheduled.txt content=hello
build\agentos.exe schedule run-due
build\agentos.exe schedule list
build\agentos.exe schedule remove id=demo-once
build\agentos.exe subagents run agents=mock_planner mode=sequential objective=Plan_the_next_phase
build\agentos.exe subagents run agents=mock_planner mode=parallel objective=Parallel_plan_smoke
build\agentos.exe auth providers
build\agentos.exe auth status
build\agentos.exe auth probe openai
build\agentos.exe auth login openai mode=cli-session
build\agentos.exe auth login qwen mode=api-key api_key_env=QWEN_API_KEY profile=default
build\agentos.exe run http_fetch url=https://example.com allow_network=true
build\agentos.exe run analysis target=codex_cli objective=Review_the_project_structure
```

审计日志默认写入：

```text
runtime/audit.log
runtime/execution_cache.tsv
runtime/trust/identities.tsv
runtime/trust/allowlist.tsv
runtime/scheduler/tasks.tsv
runtime/memory/task_log.tsv
runtime/memory/step_log.tsv
runtime/memory/skill_stats.tsv
runtime/memory/agent_stats.tsv
runtime/memory/workflow_candidates.tsv
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
