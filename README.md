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

## 项目结构

```text
agentos/
├── README.md
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
