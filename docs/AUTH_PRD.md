# AgentOS 认证子系统需求文档（AUTH PRD）

## 1. 背景

AgentOS 需要接入多个外部能力提供方与二级代理。  
这些提供方的认证机制并不统一，因此系统必须具备一个独立、统一、可扩展的认证层。

该层负责：

- 登录
- 凭据保存
- 凭据刷新
- 会话状态查询
- 多账号支持
- 官方 CLI 登录态复用
- API key 管理

---

## 2. 产品目标

AgentOS 的认证子系统需要做到：

- 统一抽象不同 provider 的认证差异
- 上层 Skill / AgentAdapter 不直接处理 token 细节
- 支持本地开发场景与无头自动化场景
- 支持安全存储与可控刷新
- 允许部分 provider 走 AgentOS 自己管理的 OAuth
- 允许部分 provider 复用官方 CLI 登录态

---

## 3. 使用场景

### 3.1 本机交互式开发
用户在桌面电脑上运行：

```bash
agentos auth login gemini
```

系统打开浏览器登录，成功后保存 session。

### 3.2 官方 CLI 登录态复用
用户已经用 `codex` 或 `claude` 登录成功，希望 AgentOS 直接复用，而不是重新实现官方登录流程。

### 3.3 CI / 无头环境
用户在服务器或 CI 中，希望系统明确区分：

- 哪些 provider 可用 API key
- 哪些 provider 可用云凭据
- 哪些 provider 不适合无头 OAuth

### 3.4 多账号切换
用户需要：

- default profile
- work profile
- personal profile

并且按 workspace 指定默认凭据。

---

## 4. 需求范围

第一阶段支持以下 provider：

- OpenAI / Codex
- Gemini
- Claude / Anthropic
- Qwen

第一阶段支持以下 auth mode：

- Browser OAuth
- API Key
- CLI Session Passthrough
- Cloud ADC / Cloud Credentials

---

## 5. 功能需求

## 5.1 Provider 管理
系统必须能声明每个 provider 的：

- provider_id
- 支持的 auth modes
- 是否支持交互式浏览器登录
- 是否支持 headless 模式
- 是否支持 refresh token
- 是否支持复用官方 CLI session

## 5.2 登录
系统必须支持：

- `agentos auth login <provider>`
- 可指定 mode
- 可指定 profile 名称
- 登录结果持久化
- 登录失败给出明确错误原因

## 5.3 状态查看
系统必须支持：

- `agentos auth status`
- `agentos auth status <provider>`
- 显示当前 profile
- 显示是否已过期
- 显示是否可刷新
- 显示是否为 CLI 托管 session

## 5.4 刷新
系统必须支持：

- 自动刷新
- 手动刷新
- 刷新失败回退至重新登录

## 5.5 登出
系统必须支持：

- 删除本地 session
- 删除缓存状态
- 从默认 profile 列表中移除

## 5.6 多账号 / 多 profile
系统必须支持：

- 同一 provider 多个 profile
- profile 命名
- workspace 指定默认 profile

## 5.7 安全存储
系统必须支持：

- token 与 key 的安全存储
- 优先使用系统 Keychain/Credential Store
- 无系统密钥仓时使用本地加密存储
- 日志中不得打印敏感信息

## 5.8 外部 CLI 会话复用
系统必须支持：

- 探测官方 CLI 是否已登录
- 允许将官方 CLI session 作为外部托管 session 纳入系统
- 区分“AgentOS 管理的 session”和“外部 CLI 托管 session”

---

## 6. Provider 级需求

## 6.1 OpenAI / Codex
第一阶段支持：

- API Key
- Codex CLI session passthrough

第一阶段不要求：
- 自己重写 ChatGPT 登录协议

## 6.2 Gemini
第一阶段支持：

- Browser OAuth
- API Key
- ADC / 云认证

Gemini 是第一阶段最适合自己实现标准 OAuth 的 provider。

## 6.3 Claude / Anthropic
第一阶段支持：

- Anthropic API Key
- Claude CLI session passthrough
- 未来支持 Bedrock / Vertex / Foundry

## 6.4 Qwen
第一阶段支持：

- API Key

第一阶段不支持：
- Qwen OAuth

---

## 7. 非功能需求

### 7.1 安全
- 敏感字段不可明文打印
- token 存储必须加密或进入系统密钥链

### 7.2 可扩展
- 新 provider 通过 adapter 新增
- 新 auth mode 不应破坏既有接口

### 7.3 稳定性
- 单个 provider 的认证异常不影响整个系统
- 登录流程不应阻塞核心 Agent Loop

### 7.4 可观察性
- 认证状态必须清晰
- 可明确看到会话来源与有效性

---

## 8. MVP 范围

### 第一版必须完成
- AuthManager 抽象
- Provider Adapter 抽象
- SessionStore
- CredentialBroker
- Gemini OAuth 基础骨架
- OpenAI API key
- Anthropic API key
- Qwen API key
- Codex CLI session probe
- Claude CLI session probe

---

## 9. 当前实现状态与缺口

已实现：

- AuthManager / ProviderAdapter / SessionStore / SecureTokenStore / CredentialBroker
- OpenAI / Anthropic / Gemini / Qwen provider descriptor
- API key mode，当前以环境变量引用保存，不落明文 key
- `auth credential-store` 明确展示当前 env-ref-only dev fallback
- workspace 默认 profile 映射
- Codex CLI / Claude CLI session probe
- `agentos auth providers/status/login/logout/refresh/probe`

未实现或仍需补齐：

- Browser OAuth / PKCE
- Cloud ADC / cloud credentials
- 真实 OAuth token refresh 交换
- 系统 Keychain / Credential Store 集成
- 多账号 profile 的完整选择策略
- 更完整的状态测试和失败路径测试
