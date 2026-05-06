# maxenergy/skills 接入记录

## 结论

`https://github.com/maxenergy/skills.git` 当前是一套 `SKILL.md` 型 agent skills 仓库，适合安装到 AgentOS 仓库级代理技能库：

- `.agents/skills/<skill-name>/SKILL.md`

这些技能会被 Codex/Claude 等代理读取，用来约束调研、澄清、拆任务、执行和验收流程。它们不是 AgentOS runtime `SkillManifest`，因此不会通过 `run <skill_name>` 直接执行，也不会出现在 runtime skill 表的同一层语义里。

AgentOS 的 `skills` 命令现在会分两段显示：

- runtime skills：可通过 `run <skill_name> ...` 调用。
- repo-level agent skills：位于 `.agents/skills`，供代理会话读取。

## 已安装的仓库级 agent skills

本仓库已安装：

- `00-understand-system`
- `01-grill-requirements`
- `02-spec-freeze`
- `03-impact-analysis`
- `04-task-slice`
- `05-goal-pack`
- `06-codex-controller`
- `07-verify-loop`
- `08-goal-review`

本仓库原有的 `autonomous-dev` 保留。

## 安装命令

使用仓库脚本安装或补齐：

```bash
tools/install_maxenergy_skills.sh --repo-scope
```

安装指定技能：

```bash
tools/install_maxenergy_skills.sh --repo-scope 00-understand-system 07-verify-loop
```

脚本会优先使用 Codex 自带的 `skill-installer`。如果目标目录已存在同名技能，脚本会跳过，避免覆盖本地修改。

## 为什么不是 runtime skill

这些 `SKILL.md` 是流程说明，不是可执行工具。AgentOS runtime skill 需要：

- `SkillManifest`
- 输入/输出 schema
- 权限声明
- 风险等级
- timeout
- 审计语义

如果未来要把其中某个 agent skill 升级成 runtime capability，需要单独设计 adapter，而不是直接把 `SKILL.md` 转成 runtime skill。
