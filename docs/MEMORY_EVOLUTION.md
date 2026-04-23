# AgentOS 记忆与演化系统设计文档

## 1. 目标

AgentOS 的长期价值不在于“能调用多少工具”，而在于：

- 能否记住做过的事情
- 能否总结哪些方法更有效
- 能否把高频成功模式沉淀为 Workflow
- 能否根据历史结果优化后续决策

因此，记忆与演化系统必须是一等公民。

---

## 2. 记忆层次

建议至少拆分为四层：

## 2.1 短期上下文（Working Context）
当前任务的即时状态：

- 当前目标
- 当前步骤
- 关键中间结果
- 当前失败原因

## 2.2 情节记忆（Episodic Memory）
记录一次任务是怎么完成的：

- 任务目标
- 步骤顺序
- 调用了哪些 Skill / Agent
- 结果如何
- 成本如何
- 用户是否满意

## 2.3 语义记忆（Semantic Memory）
沉淀较稳定的规则、经验和知识：

- 常见约束
- 适用场景
- 成功经验
- 失败教训

## 2.4 程序性记忆（Procedural Memory）
形成可复用的执行套路，即 Workflow 或 SOP。

---

## 3. 任务记录

每次任务执行结束后，系统至少应记录：

- task_id
- task_type
- objective
- steps
- selected_skills
- selected_agents
- success/failure
- duration
- cost
- user_feedback
- lessons

示例：

```json
{
  "task_type": "fix_flaky_test",
  "steps": [
    {"skill": "rg_search", "status": "ok"},
    {"agent": "codex_cli", "status": "ok"},
    {"skill": "file_patch", "status": "ok"}
  ],
  "result": "success",
  "lessons": [
    "Prefer repo-scoped search before patch generation"
  ]
}
```

---

## 4. Workflow 生成

系统应从高频、稳定、成功率高的任务中抽取 Workflow 候选。

Workflow 不是简单回放，而是带有可解释结构的程序性记忆。

建议 Workflow 至少包含：

- name
- trigger_conditions
- ordered_steps
- optional fallback
- expected outcome
- score

示例：

```json
{
  "name": "fix_flaky_test_workflow",
  "steps": [
    "rg_search",
    "codex_reason",
    "file_patch",
    "run_targeted_tests"
  ]
}
```

---

## 5. 评分系统

## 5.1 Skill 评分
每个 Skill 记录：

- total_calls
- success_rate
- avg_latency
- avg_cost
- rollback_rate
- acceptance_rate

## 5.2 Agent 评分
每个代理记录：

- total_runs
- pass_rate
- avg_cost
- avg_latency
- user_preference
- patch_accept_rate

## 5.3 Workflow 评分
每个 Workflow 记录：

- use_count
- success_rate
- failure_count
- avg_duration_ms
- average_time_saved
- applicability_score

当前实现已在 `runtime/memory/workflow_candidates.tsv` 中持久化：

- name
- trigger_task_type
- ordered_steps
- use_count
- success_count
- failure_count
- success_rate
- avg_duration_ms
- score

当前 score 为基础综合分：成功次数与成功率提高分数，失败次数与平均耗时降低分数。后续可继续加入 `average_time_saved`、用户接受率和适用条件。

当前实现也提供独立的 `runtime/memory/workflows.tsv` WorkflowStore 骨架，用于持久化稳定 workflow 定义：

- name
- enabled
- trigger_task_type
- ordered_steps
- required_inputs
- source
- use_count
- success_count
- failure_count
- success_rate
- avg_duration_ms
- score

当前可通过 `agentos memory promote-workflow <candidate_name> required_inputs=path,content` 将候选固化为启用的 workflow 定义，并通过 `agentos run workflow_run workflow=<name> ...` 执行持久定义。Router 也会在没有显式 `target` 时，对 enabled 且 `trigger_task_type` / `required_inputs` 匹配的 workflow 按 score 优先选择并路由到 `workflow_run`。

---

## 6. 演化机制

建议按以下层级演化：

### 第 1 层：学会记录
所有执行都要结构化记录。

### 第 2 层：学会总结
从成功和失败中提炼 lesson。

### 第 3 层：学会形成 Workflow
将稳定模式抽取为可复用能力。

### 第 4 层：学会优化路由
根据运行效果动态调整 Skill/Agent 选择。

### 第 5 层：再考虑代码固化
只在某些 Workflow 稳定且高频时，才考虑进一步固化为更高性能实现。

---

## 7. 存储设计建议

建议最初使用 SQLite。

### 表建议
- tasks
- task_steps
- skill_stats
- agent_stats
- workflows
- lessons
- memory_embeddings（可选后续）

### 原则
- 先结构化
- 后向量化
- 不要一开始把所有内容都丢向量库

---

## 8. 与路由的关系

记忆系统的一个核心输出是：改善路由。

例如：

- 某任务类型下 Codex 成功率更高，则提高其优先级
- 某 CLI Skill 常超时，则降低路由权重
- 某 Workflow 已被证明稳定，则优先尝试 Workflow

---

## 9. 失败经验的重要性

失败记录不是噪音，而是演化资产。

建议专门记录：

- 常见失败前置条件
- 常见错误组合
- 哪些代理不适合什么任务
- 哪些 Skill 在什么条件下容易出错

这些内容可转化为：
- 路由规则
- 策略限制
- Workflow 约束
- 用户提示

---

## 10. 第一阶段建议

第一版只做：

- Task Log
- Step Log
- Skill/Agent 评分
- 基础 Workflow 生成骨架

这就足以让系统从“能做事”迈向“会成长”。
