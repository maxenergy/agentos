# AgentOS 编码规范与开发指南

## 1. 技术选型建议

### 1.1 语言与标准
- C++20

### 1.2 构建系统
- CMake

### 1.3 推荐依赖
- nlohmann/json：JSON 序列化
- SQLite：本地结构化存储
- spdlog：日志
- GoogleTest / Catch2：测试
- asio 或 libuv：异步能力（按需）

---

## 2. 基本原则

### 2.1 接口优先
先定义接口，再写实现。

### 2.2 核心保持轻量
Core 只做调度、策略、审计、记忆触发，不要堆业务逻辑。

### 2.3 不在核心写 provider-specific 逻辑
不同 provider 的差异应隔离在 adapter 中。

### 2.4 明确边界
文件、进程、网络、凭据、代理都要通过明确边界接入。

### 2.5 结构化优先
日志、结果、记忆优先结构化，而不是靠长文本拼接。

---

## 3. 建议目录结构

```text
src/
  main.cpp
  core/
    loop/
    router/
    registry/
    policy/
    audit/
  auth/
  skills/
    builtin/
    workflow/
  hosts/
    cli/
    plugin/
    agents/
  memory/
  storage/
  utils/
```

---

## 4. 接口规范

建议优先冻结这些接口：

- ISkillAdapter
- IWorkflowAdapter
- IAgentAdapter
- IAuthProviderAdapter
- ICredentialBroker

实现代码时要做到：
- 纯接口可单测
- 具体实现不污染抽象层
- 参数命名清晰
- 错误类型明确

---

## 5. 错误处理

### 5.1 不建议
- 到处抛裸字符串异常
- 用魔法数字表示错误
- stderr 文本作为唯一错误来源

### 5.2 建议
定义明确错误码或错误对象：

- ProviderNotRegistered
- SkillNotFound
- PolicyDenied
- Timeout
- InvalidArguments
- ExternalProcessFailed

---

## 6. 日志规范

日志必须：

- 有 level
- 有 component
- 有 trace/task id
- 敏感字段脱敏

### 不可打印
- access token
- refresh token
- API key
- 明文凭据

---

## 7. 测试建议

优先写：
- Registry 测试
- Router 测试
- Auth adapter 的 mock 测试
- Skill 执行的单测
- Workflow 生成逻辑测试

对外部 CLI/Agent：
- 尽量使用 mock adapter 做主流程测试
- 真实集成测试独立出来

---

## 8. 开发顺序建议

1. 先建工程和接口
2. 先写最小主流程
3. 再写基础 Skill
4. 再写 CLI Host
5. 再接 Agent Adapter
6. 再接 Auth
7. 再做 Memory / Evolution

---

## 9. 推荐给 Codex 的开发方式

把 docs 全部放进仓库后，可以直接让 Codex：

- 先生成 CMakeLists.txt
- 再生成 headers/interfaces
- 再生成 main.cpp demo
- 再逐模块实现

建议每个阶段都先：
- 建文件
- 写接口
- 写最小实现
- 写一个 demo 或单测

不要一口气让 Codex 生成整个复杂系统。
