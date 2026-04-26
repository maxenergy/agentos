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
- input_equals
- input_number_gte
- input_number_lte
- input_bool
- input_regex
- input_any
- input_expr
- source
- use_count
- success_count
- failure_count
- success_rate
- avg_duration_ms
- score

当前可通过 `agentos memory promote-workflow <candidate_name> required_inputs=path,content input_equals=mode=workflow input_number_gte=priority=5 input_number_lte=size=10 input_bool=approved=true input_regex=branch=release/.* input_any=equals:mode=workflow|equals:mode=automated input_expr="equals:mode=workflow&&(exists:ticket||regex:branch=release/.*)"` 将候选固化为启用的 workflow 定义，并通过 `agentos run workflow_run workflow=<name> ...` 执行持久定义。Router 也会在没有显式 `target` 时，对 enabled 且 `trigger_task_type` / `required_inputs` / `input_equals` / 数值范围 / 布尔输入 / 正则输入 / `input_any` 复合 OR 条件 / `input_expr` 嵌套布尔表达式匹配的 workflow 按 score 优先选择并路由到 `workflow_run`。

`agentos memory validate-workflows` 会校验已持久化 workflow 定义的基础字段、条件格式、数值/布尔条件、正则表达式、`input_any` atom 和 `input_expr` 语法；`promote-workflow` 保存前也会执行同一套校验，避免错误定义进入 `workflows.tsv`。

`agentos memory stored-workflows` 可列出持久化 workflow，并支持 `enabled`、`trigger_task_type` / `trigger`、`source`、`name_contains` 过滤。`agentos memory show-workflow <workflow_name>` 会输出单个持久化 workflow 的启用状态、trigger、步骤、统计数据和所有 applicability 条件；`agentos memory update-workflow <workflow_name> ...` 可原地修改名称、trigger、步骤、启用状态和 applicability 条件，传入空列表值可清空对应条件，并在保存前运行同一套 workflow 定义校验；`agentos memory clone-workflow <workflow_name> new_name=<stored_name>` 可复制已有 workflow 作为后续编辑基础；`agentos memory set-workflow-enabled <workflow_name> enabled=true|false` 可切换 workflow 是否参与 Router 自动选择；`agentos memory remove-workflow <workflow_name>` 可删除持久化 workflow 定义。`agentos memory explain-workflow <workflow_name> task_type=<task_type> key=value ...` 会输出 workflow 级 `applicable=true|false`，并逐项列出 trigger、required input、条件组和表达式的匹配结果，用于调试某个 workflow 为什么会被 Router 选中或跳过。

当前实现还提供 `runtime/memory/lessons.tsv` LessonStore 骨架，用于按 `task_type` / `target_name` / `error_code` 聚合重复失败：

- lesson_id
- enabled
- task_type
- target_name
- error_code
- occurrence_count
- last_task_id
- summary

可通过 `agentos memory lessons` 查看。Router 当前会使用 LessonStore 作为软信号：当某个 task_type 的自动 `workflow_run` 重复失败时，后续会回退到基础 skill；当某个 agent 对同类任务重复失败时，会降低该 agent 的路由优先级。PolicyDenied 当前会附加已有 lesson hint，帮助解释重复失败模式，但不会改变硬性 allow/deny 规则。

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
