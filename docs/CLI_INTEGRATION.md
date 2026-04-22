# AgentOS CLI Integration 设计文档

## 1. 目标

AgentOS 希望最大化复用现有工具生态，因此需要一个 CLI Integration 层，把常见命令行工具统一包装成可管理的 Skill。

该层的价值是：

- 不重复造轮子
- 快速接入现有强工具
- 统一纳管安全、timeout、解析、审计
- 为 Workflow 和 Agent 提供丰富执行手段

---

## 2. 适合接入的 CLI 类型

### 2.1 通用开发工具
- git
- rg
- jq
- curl
- sqlite3

### 2.2 媒体与文档工具
- ffmpeg
- pandoc
- imagemagick

### 2.3 云与基础设施工具
- docker
- kubectl
- terraform（需慎重）
- ssh（高风险）

### 2.4 AI / Coding CLI
- codex
- claude
- gemini
- qwen code
- 其他未来 CLI

---

## 3. 设计原则

### 3.1 CLI 不直接裸露给 LLM
CLI 必须包装成 Skill，而不是让模型任意拼 shell。

### 3.2 配置驱动
尽量通过 manifest/spec 接入，而不是硬编码。

### 3.3 受控执行
必须具备：

- timeout
- cwd 限制
- env 白名单
- stdout/stderr 捕获
- exit code 检查
- 输出结构化

### 3.4 高危命令默认限制
例如：
- rm
- git push
- docker exec
- kubectl apply
- ssh

应默认限制或要求显式策略批准。

---

## 4. 架构设计

建议实现一个 `CliHost`，负责：

- 载入 CLI Spec
- 渲染命令模板
- 校验参数
- 受控启动进程
- 收集输出
- 结构化解析结果

每个 CLI Skill 由一个 spec 描述。

---

## 5. CLI Spec 示例

```json
{
  "name": "rg_search",
  "binary": "rg",
  "args_template": ["--json", "{{pattern}}", "{{path}}"],
  "input_schema": {
    "type": "object",
    "properties": {
      "pattern": {"type": "string"},
      "path": {"type": "string"}
    },
    "required": ["pattern", "path"]
  },
  "parse_mode": "json_lines",
  "risk_level": "low",
  "permissions": ["filesystem.read", "process.spawn"],
  "timeout_ms": 3000
}
```

---

## 6. CLI 分类

## 6.1 结构化 CLI
适合输出 JSON 或稳定结构文本的工具。

示例：
- rg --json
- jq
- git status --porcelain

## 6.2 普通命令型 CLI
输出为普通文本，需要解析器或原样输出。

示例：
- git diff
- curl
- ffmpeg

## 6.3 会话型 CLI
需要长期会话与多轮输入。

示例：
- 某些 AI CLI
- REPL
- 调试器

这些不要直接当成普通一次性命令，而应转入 SessionHost 或 AgentAdapter 层。

---

## 7. 核心接口建议

```cpp
class ICliAdapter : public ISkillAdapter {
public:
    virtual std::vector<std::string> command_template() const = 0;
    virtual int timeout_ms() const = 0;
};
```

或者实现一个统一 `CliSkillInvoker`：

```cpp
class CliSkillInvoker : public ISkillAdapter {
public:
    SkillManifest manifest() const override;
    SkillResult execute(const SkillCall& call) override;
};
```

---

## 8. 安全设计

### 8.1 cwd 限制
默认只能在当前 workspace 下执行。

### 8.2 env 白名单
只传入必要环境变量。

### 8.3 输出限流
防止 stdout/stderr 爆量输出。

### 8.4 timeout
每个命令都必须有 timeout。

### 8.5 资源限制
长期建议增加：
- CPU 限制
- 内存限制
- 文件句柄限制

---

## 9. 审计要求

每次 CLI 调用必须记录：

- 调用时间
- 调用人 / workspace
- 使用的 spec
- 最终命令（敏感字段脱敏）
- exit code
- duration
- stdout/stderr 摘要

---

## 10. 第一阶段建议接入工具

- rg_search
- git_status
- git_diff
- jq_transform
- curl_fetch

这些工具足以覆盖很多基础开发与检索任务。
