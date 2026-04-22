# AgentOS Roadmap

## Phase 0：文档与接口冻结
目标：
- 名称确定为 AgentOS
- 冻结 PRD / 架构 / 认证 / Skill / Agent / Memory 文档
- 冻结核心接口

交付物：
- docs/*.md
- 初版代码目录规划

---

## Phase 1：最小内核
目标：
- 实现 AgentLoop
- 实现 SkillRegistry
- 实现 AgentRegistry
- 实现 PolicyEngine 基础版
- 实现 AuditLogger 基础版

交付物：
- 可编译运行的最小 C++ 工程
- 一个 demo task 能跑通

---

## Phase 2：基础 Skill
目标：
- file_read
- file_write
- file_patch
- http_fetch
- workflow_run

交付物：
- 基本 Skill 执行链路
- 输入输出 schema 骨架

---

## Phase 3：CLI Host
目标：
- 实现 CliHost
- 接入常用 CLI spec
- 完成 timeout / cwd / stdout-stderr 管理

交付物：
- rg_search
- git_status
- git_diff
- curl_fetch
- jq_transform

---

## Phase 4：首个二级代理
目标：
- 实现 IAgentAdapter
- 接入一个代理（Codex CLI 或 Claude Code）
- 结果标准化
- Agent Registry / Router 骨架跑通

交付物：
- 一个外部专家型代理可被系统调用

---

## Phase 5：认证子系统
目标：
- AuthManager
- SessionStore
- SecureTokenStore
- CredentialBroker
- Gemini OAuth 骨架
- OpenAI/Anthropic/Qwen key 管理
- Codex/Claude CLI session probe

交付物：
- `agentos auth ...` 命令组
- session 状态查看与刷新

---

## Phase 6：记忆与 Workflow
目标：
- Task Log
- Step Log
- Skill/Agent 评分
- Workflow 候选生成

交付物：
- 基础成长闭环
- 可复用 Workflow 雏形

---

## Phase 7：多代理路由
目标：
- 接入 3~4 个代理
- 实现静态角色 + 动态评分路由
- 支持辅助代理模式

交付物：
- 多代理协作初版

---

## Phase 8：演化优化
目标：
- Workflow 评分
- 动态权重调整
- 降级与灰度禁用
- 经验 lessons 提炼

交付物：
- 系统从“能做”迈向“会成长”

---

## Phase 9：生态化扩展
目标：
- 多语言 Plugin Host
- 更多 CLI spec
- 更强的审计和策略系统
- UI / 观测面板（可选）

交付物：
- 可扩展平台雏形
