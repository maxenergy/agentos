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
当前支持：

- API Key
- Codex CLI session passthrough
- Browser OAuth (PKCE via `auth.openai.com`)

`OpenAiAgent` 通过 `api.openai.com/v1/chat/completions` REST API 直接调用模型。

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

> 注意：当前 Browser OAuth 已支持 `oauth-login` 单命令编排 PKCE start、可选系统浏览器打开、一次性 loopback callback、token exchange 和 managed session 持久化；底层 `oauth-start` / `oauth-listen` / `oauth-complete` 仍可脚本化单独使用。`runtime/auth_oauth_providers.tsv` 可覆盖或补充 provider OAuth defaults。它仍不是完整产品级多 provider 登录 UX。`SecureTokenStore` 已在 Windows / macOS / Linux 上接入系统 keychain（Windows Credential Manager、macOS Keychain via Security framework、Linux Secret Service via libsecret 可选依赖）；当宿主缺少 libsecret 时仍退化为 `env-ref-only` dev fallback。

已实现：

- AuthManager / ProviderAdapter / SessionStore / SecureTokenStore / CredentialBroker
- OpenAI / Anthropic / Gemini / Qwen provider descriptor
- API key mode，当前以环境变量引用保存，不落明文 key
- `auth credential-store` 明确展示当前 credential backend：Windows = Credential Manager，macOS = Keychain (Security framework)，Linux = Secret Service via libsecret（可选依赖；缺失时回退至 `env-ref-only` dev fallback）。`SecureTokenStore` 内部使用 `ISecureTokenBackend` 抽象，并提供 in-memory backend 供测试注入，CI 中绝不触达真实 keychain。
- workspace 默认 profile 映射
- `set_default=true` 可在 login / OAuth completion 成功后立即把 profile 设为 provider 默认值
- Codex CLI / Claude CLI session probe 与导入 fixture 测试
- `agentos auth providers/profiles/status/oauth-defaults/oauth-config-validate/oauth-start/oauth-login/oauth-callback/oauth-listen/oauth-complete/oauth-token-request/oauth-refresh-request/login/logout/refresh/probe`
- Gemini browser OAuth passthrough 已通过 Gemini CLI OAuth 文件导入；无可导入会话时返回 `BrowserOAuthUnavailable`
- Browser OAuth PKCE 基础设施：S256 challenge、authorization URL 构建、`oauth-defaults` provider 默认 OAuth 配置查询（含 origin/note 元数据，区分 builtin / config / stub，并输出 `endpoint_status=available|deferred|missing`）、repo-local `runtime/auth_oauth_providers.tsv` OAuth defaults 覆盖与 `oauth-config-validate [--all]` 全 provider 诊断、`oauth-start open_browser=true` 系统默认浏览器启动尝试、`oauth-login` 单命令 start/listen/token-exchange/session-persist 编排、一次性 localhost callback listener、callback URL query 解析、callback state/code/error 校验、authorization-code / refresh-token request form-body 构建、curl-backed HTTP exchange helper、token response 解析、managed AuthSession 持久化 helper、scriptable `oauth-complete` 编排桥接、provider adapter 参数化原生 login/refresh 编排、Gemini Google OAuth 默认 endpoint/scope、OpenAI PKCE 默认 endpoint/scope (`auth.openai.com/authorize` + `/oauth/token`)；`anthropic` / `qwen` 仍作为 stub provider 并以 `endpoint_status=deferred` 标记
- `auth login-interactive [provider=<id>]` 交互式 stdin UX：列出已注册 provider、读取 `OAuthDefaultsForProvider` 的 `origin` / `note` 元数据并附带在提示中，按 provider 支持的 mode 选择 cli-session / api-key / browser_oauth，最终回到现有 `auth login` 路径执行；`browser_oauth` 选项指引用户改走 `auth oauth-login` 命令完成 PKCE 全流程
- 凭据安全硬化：OAuth body 和 bearer token 经 `src/utils/curl_secret.{hpp,cpp}` 短生命周期临时文件中转给 curl（`--data @file` / `-H @file`），不再经 argv 暴露；callback 路径校验与 Host 头 loopback 检查在 `src/auth/oauth_pkce.cpp` 加固

未实现或仍需补齐：

- 更完整的多 provider 交互式登录 UX（Anthropic / Qwen 公开 PKCE endpoints 仍未公开，已注册为 `stub` provider 并报告 `endpoint_status=deferred`；OpenAI 已升级为 `builtin` provider）
- 全 provider 的实际公网 OAuth 互操作性测试（目前 Gemini 和 OpenAI 有 builtin defaults；其他 provider 需通过 `runtime/auth_oauth_providers.tsv` 手动配置 authorization/token endpoints，配置完整后 discovery 状态变为 `endpoint_status=available`）
- Native cloud provider token exchange beyond Google ADC/gcloud passthrough
- 更完整的多账号 profile 选择策略
- 更完整的状态测试和失败路径测试
