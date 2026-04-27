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
  "timeout_ms": 3000,
  "output_limit_bytes": 1048576,
  "env_allowlist": [],
  "memory_limit_bytes": 0,
  "max_processes": 0,
  "cpu_time_limit_seconds": 0,
  "file_descriptor_limit": 0
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

当前实现中，`CliHost` 默认只传入进程启动必需的最小系统环境；额外变量必须通过 `CliSpec.env_allowlist` 显式声明。AI CLI 登录态探测只允许用户配置目录类变量，不透传 API key。

### 8.3 输出限流
防止 stdout/stderr 爆量输出。

### 8.4 timeout
每个命令都必须有 timeout。

### 8.5 当前实现状态

- `CliSpec.timeout_ms` 已用于强制进程超时
- `CliSpec.output_limit_bytes` 已用于 stdout/stderr 捕获限流
- `CliSpec.env_allowlist` 已用于子进程环境白名单
- Windows 下 `CliSpec.max_processes` / `CliSpec.memory_limit_bytes` / `CliSpec.cpu_time_limit_seconds` 会通过 Job Object 强制
- POSIX 平台下 `CliSpec.max_processes` / `CliSpec.memory_limit_bytes` / `CliSpec.cpu_time_limit_seconds` / `CliSpec.file_descriptor_limit` 会在子进程内通过 `setrlimit` 尽力强制
- `cwd` 参数必须解析到当前 workspace 内
- Windows batch / cmd 启动已通过受控 `CreateProcess` 路径执行
- `runtime/cli_specs/*.tsv` 可声明 repo-local 外部 CLI specs，并在启动时注册为 `CliSkillInvoker`
- TSV loader 会保留空字段，因此 `required_args` 等可选列为空时不会导致后续列错位
- `agentos cli-specs validate` 会输出已加载 spec 与 skipped spec 诊断，并在存在无效 spec 时返回非零
- `jq_transform` 已作为内置 CLI skill 接入，使用 `jq -c <filter> <path>` 转换 workspace 内 JSON 文件

### 8.6 资源限制
当前已实现：
- Windows Job Object `max_processes`
- Windows Job Object `memory_limit_bytes`
- Windows Job Object `cpu_time_limit_seconds`
- POSIX `setrlimit(RLIMIT_NPROC)` `max_processes`
- POSIX `setrlimit(RLIMIT_AS)` `memory_limit_bytes`
- POSIX `setrlimit(RLIMIT_CPU)` `cpu_time_limit_seconds`
- POSIX `setrlimit(RLIMIT_NOFILE)` `file_descriptor_limit`

后续仍建议补充：
- Windows 文件句柄限制
- 更细粒度的跨平台 sandbox / cgroup / job lifecycle 策略

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

---

## 11. Plugin Host 管理子命令

`agentos plugins` 子命令在 CLI 层封装 `PluginHost` 与 `runtime/plugin_specs/*` 的运维入口：

| 命令 | 用途 |
|------|------|
| `agentos plugins` | 列出已加载的 plugin specs，附带 lifecycle/sandbox/pool_size 等 manifest 信息及 health 状态 |
| `agentos plugins validate` | 仅做静态校验，不发起 health probe |
| `agentos plugins health` | 列出加载结果并执行声明的 health probe（含 persistent JSON-RPC round-trip） |
| `agentos plugins lifecycle` | 列出 lifecycle 信息汇总：oneshot/persistent 计数、`max_persistent_sessions`、每条 spec 的 `pool_size` |
| `agentos plugins inspect name=<plugin> [health=true]` | 打印单个 spec 的所有字段，可选附加 health probe |
| `agentos plugins sessions [name=<plugin>]` | 列出当前进程内 PluginHost 已经长驻的 persistent sessions（plugin 名、pid、started_at、last_used_at、idle_for_ms、idle_timeout_ms、idle_expired、request_count、alive）；带 `name=` 时只统计该 plugin，并在 summary 输出 `matched=true|false`、`idle_expired=<n>`、`dead=<n>`、`scope=process`、`persistence=none` |
| `agentos plugins session-prune [name=<plugin>] [dry_run=true]` | 清理当前进程内 idle-expired 或 dead 的 persistent sessions；不带 `name=` 时扫描全部 sessions；`dry_run=true` 只输出 `would_prune=<n>` 不修改 session 状态；输出 `pruned=<n>`、`matched=true|false`、`dry_run=true|false`、`reason=idle_expired_or_dead`、`scope=process`、`persistence=none` |
| `agentos plugins session-restart name=<plugin>` | 强制重启某个 plugin 名下的所有 persistent sessions，输出 `matched=true|false` 区分是否命中活跃 session |
| `agentos plugins session-close name=<plugin>` | 优雅关闭某个 plugin 名下的所有 persistent sessions，输出 `matched=true|false` 区分是否命中活跃 session |

Session admin commands accept `scope=process` explicitly and default to it. `scope=daemon` currently returns `plugin_sessions_unavailable scope=daemon supported_scope=process` until cross-process daemon session inspection is implemented.

manifest 关键字段（TSV 列顺序与 JSON 字段名相同）：

- `lifecycle_mode=oneshot|persistent`
- `startup_timeout_ms`、`idle_timeout_ms`、`pool_size`
- `runtime/plugin_host.tsv` 中的 `max_persistent_sessions`

### 11.1 进程池策略

- `pool_size=N` 限制单个 plugin 在 PluginHost 中可同时存在的 persistent session 数量；
- 实际生效值是 `min(pool_size, max_persistent_sessions)`；
- 当超过 `pool_size` 时优先驱逐该 plugin 最旧的 session；当超过全局 `max_persistent_sessions`
  时按 LRU 驱逐任意 plugin 的最旧 session；
- 默认 `pool_size=1`，与既有单 session 行为兼容。

这些工具足以覆盖很多基础开发与检索任务。
