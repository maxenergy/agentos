# AgentOS 认证子系统设计文档（AUTH DESIGN）

## 1. 设计原则

认证系统需要满足：

- 对上层统一
- 对下层可扩展
- 安全与刷新独立管理
- Provider 差异被 Adapter 隔离
- 官方 CLI 登录态与 AgentOS 自己管理的 session 清晰区分

---

## 2. 模块设计

```text
AuthManager
  ├── Provider Adapters
  ├── SessionStore
  ├── SecureTokenStore
  └── CredentialBroker
```

### 2.1 AuthManager
负责：

- 登录入口
- 调用 provider adapter
- 保存 session
- 调用刷新逻辑
- 对外暴露 status / login / logout / refresh

### 2.2 Provider Adapters
每个 provider 一个 adapter，负责：

- login
- refresh
- logout
- status
- probe_external_session

### 2.3 SessionStore
负责保存 session 元数据。

建议使用 SQLite 保存：

- profile
- provider
- mode
- account label
- expires_at
- metadata

### 2.4 SecureTokenStore
负责保存：

- access_token
- refresh_token
- api_key

优先级：
1. 系统 Keychain / Credential Store
   - Windows: Credential Manager (`wincred:` ref 前缀)
   - macOS: Keychain via Security framework, generic password items (`keychain:` ref 前缀)
   - Linux: Secret Service via libsecret/D-Bus (`secret-service:` ref 前缀, optional dependency)
2. `env:` 引用（始终可用；不持久化明文）
3. `env-ref-only` dev fallback（无系统 keychain 时）

实现细节：
- 内部使用 `ISecureTokenBackend` 抽象，平台特定后端在编译期通过 `_WIN32` / `__APPLE__` / `AGENTOS_HAVE_LIBSECRET` 选择。
- 测试通过 `SecureTokenStore::MakeInMemoryBackendForTesting()` 注入 in-memory 后端，CI 中绝不触达真实 keychain。
- `read_ref` 永远走 `env:` 解析路径或当前后端，不会回退到 plaintext 文件。

### 2.5 CredentialBroker
对上层统一暴露凭据获取接口：

- get_session(provider, profile)
- get_access_token(provider, profile)

并自动处理刷新和错误回退。

---

## 3. 核心数据模型

## 3.1 AuthProviderId

```cpp
enum class AuthProviderId {
    OpenAI,
    Gemini,
    Anthropic,
    Qwen
};
```

## 3.2 AuthMode

```cpp
enum class AuthMode {
    BrowserOAuth,
    ApiKey,
    CliSessionPassthrough,
    CloudADC,
    CloudBearerToken
};
```

## 3.3 AuthSession

```cpp
struct AuthSession {
    std::string session_id;
    AuthProviderId provider;
    AuthMode mode;

    std::string profile_name;
    std::string account_label;

    bool managed_by_agentos;
    bool managed_by_external_cli;
    bool refresh_supported;
    bool headless_compatible;

    std::string access_token_ref;
    std::string refresh_token_ref;

    std::chrono::system_clock::time_point expires_at;
    std::map<std::string, std::string> metadata;
};
```

## 3.4 AuthStatus

```cpp
struct AuthStatus {
    bool authenticated;
    bool expired;
    bool refreshable;
    std::string provider_name;
    std::string profile_name;
    std::string account_label;
    std::string mode;
    std::string message;
};
```

---

## 4. 核心接口

## 4.1 IAuthProviderAdapter

```cpp
class IAuthProviderAdapter {
public:
    virtual AuthProviderId provider_id() const = 0;
    virtual std::vector<AuthMode> supported_modes() const = 0;

    virtual AuthSession login(
        AuthMode mode,
        const std::map<std::string, std::string>& options) = 0;

    virtual AuthStatus status(const std::string& profile_name) = 0;

    virtual AuthSession refresh(const AuthSession& session) = 0;

    virtual void logout(const std::string& profile_name) = 0;

    virtual std::optional<AuthSession> probe_external_session() = 0;

    virtual ~IAuthProviderAdapter() = default;
};
```

## 4.2 ICredentialBroker

```cpp
class ICredentialBroker {
public:
    virtual std::optional<AuthSession> get_session(
        AuthProviderId provider,
        const std::string& profile_name) = 0;

    virtual std::string get_access_token(
        AuthProviderId provider,
        const std::string& profile_name) = 0;

    virtual ~ICredentialBroker() = default;
};
```

---

## 5. Provider 策略

## 5.1 OpenAI / Codex
第一版支持：

- API Key
- Codex CLI session passthrough

设计建议：

- AgentOS 不自行实现 ChatGPT 登录协议
- 如果用户已经通过官方 Codex CLI 登录，则复用其 session 状态
- 对程序化使用场景优先推荐 API key

## 5.2 Gemini
第一版支持：

- Browser OAuth
- API Key
- Cloud ADC

设计建议：

- Gemini 作为第一阶段自己实现标准 OAuth 的主路径
- 使用 PKCE + localhost callback server

## 5.3 Claude / Anthropic
第一版支持：

- API Key
- Claude CLI session passthrough

未来支持：

- Bedrock
- Vertex
- Foundry

设计建议：

- 对 claude.ai 登录复用官方 CLI 会话
- 对 API 调用使用 API key 或云认证

## 5.4 Qwen
第一版支持：

- API Key

不支持：

- Qwen OAuth

---

## 6. 登录流程设计

## 6.1 Browser OAuth
适用于 Gemini 等标准开放 OAuth provider。

流程：

1. 生成 state 和 PKCE verifier（`auth/oauth_pkce` 已提供 S256 challenge 与 authorization URL 构建）
2. 启动 localhost callback server（已提供一次性 loopback listener）
3. 打开浏览器
4. 用户登录授权
5. callback 收到 code（已提供一次性 listener 与 callback URL query 解析）
6. 校验 state（`auth/oauth_pkce` 已提供 callback URL / state / code / error 校验）
7. 交换 access/refresh token（已提供 authorization-code 与 refresh-token request 构建、curl-backed HTTP exchange helper、token response 解析）
8. 写入 SecureTokenStore 和 SessionStore（已提供 token response 到 managed AuthSession 的持久化 helper；provider 级 login/refresh 编排待接入）

## 6.2 API Key
流程：

1. 用户输入 key
2. 执行一次基础健康检查
3. 安全保存 key
4. 生成 session

## 6.3 CLI Session Passthrough
适用于 Codex / Claude 等官方 CLI 已登录场景。

流程：

1. 探测官方 CLI 登录状态
2. 若已登录，生成一个 `managed_by_external_cli=true` 的 session
3. 若未登录，可引导用户使用官方 CLI 登录
4. AgentOS 使用该 session 作为二级代理可用依据

---

## 7. Session Store 设计

建议表：

### auth_profiles
- profile_id
- provider
- profile_name
- mode
- account_label
- managed_by_agentos
- managed_by_external_cli
- refresh_supported
- headless_compatible
- created_at
- updated_at

### auth_tokens
- profile_id
- access_token_ref
- refresh_token_ref
- expires_at
- metadata_json

说明：
- token 不建议明文存入 SQLite
- SQLite 只存引用和元数据

---

## 8. CLI 设计

建议命令：

```bash
agentos auth providers
agentos auth login gemini
agentos auth login openai --mode api-key
agentos auth login claude --mode cli-session
agentos auth login qwen --mode api-key
agentos auth status
agentos auth refresh gemini
agentos auth logout gemini
```

---

## 9. 错误处理

认证系统必须区分以下错误：

- ProviderNotRegistered
- UnsupportedAuthMode
- InvalidCredential
- TokenExpired
- RefreshFailed
- CliSessionUnavailable
- SecureStoreUnavailable
- CallbackTimeout

---

## 10. 开发顺序建议

1. AuthManager / Adapter / Store / Broker 抽象
2. Gemini OAuth 骨架
3. OpenAI API key
4. Anthropic API key
5. Codex CLI session passthrough
6. Claude CLI session passthrough
7. Qwen API key
8. 统一命令行入口

---

## 11. 当前实现状态与缺口

> 注意：当前原生 OAuth 已支持 `oauth-login` 单命令编排 PKCE start、可选系统浏览器打开、loopback callback、token exchange 与 managed session 持久化；调用方仍可显式使用 `oauth-defaults`、`oauth-start`、`oauth-listen` / `oauth-callback`、`oauth-complete` 或在 `auth login ... mode=browser_oauth` 中传入 callback/token 参数。`runtime/auth_oauth_providers.tsv` 可覆盖或补充 provider OAuth defaults。它仍不是完整产品级多 provider 登录流；更多 provider discovery、配置校验诊断与非 Windows 系统 credential store 仍是开放项。

已实现：

- `AuthManager`
- `AuthProviderAdapter`
- `SessionStore`
- `SecureTokenStore` MVP
- `CredentialBroker`
- OpenAI / Anthropic / Gemini / Qwen provider adapter
- API key env-ref 模式
- credential store status 明确标记当前后端：Windows = Credential Manager，macOS = Keychain (Security framework)，Linux = Secret Service via libsecret（可选依赖；缺失时回退至 `env-ref-only` dev fallback）
- workspace default profile mapping (`runtime/auth_profiles.tsv`)
- profile listing through `auth profiles [provider]`
- `set_default=true` on login / OAuth completion commands for one-step default profile selection
- repo-local OAuth defaults mapping (`runtime/auth_oauth_providers.tsv`) with `auth oauth-config-validate` diagnostics
- `auth oauth-defaults` and `auth oauth-config-validate --all` emit `endpoint_status=available|deferred|missing`: `available` means the provider has usable authorization/token endpoints, `deferred` means the provider is intentionally registered as a stub until stable public PKCE endpoints are available, and `missing` means neither builtin defaults nor workspace overrides currently provide endpoints.
- `auth oauth-start`, `auth oauth-login`, and `auth oauth-complete` emit a structured `oauth_input_error` line before failing on missing PKCE inputs, including `missing_fields`, provider `origin`, `endpoint_status`, and the workspace OAuth config path.
- `auth oauth-callback`, `auth oauth-listen`, `auth oauth-token-request`, and `auth oauth-refresh-request` also emit `oauth_input_error missing_fields="..."` before returning the existing first missing-field message, so scripts can see the full input gap in one call.
- Codex / Claude CLI session passthrough probe 与导入 fixture 测试
- `auth refresh` 命令、AuthManager refresh 入口与 Adapter refresh 覆盖已接入
- Gemini browser OAuth passthrough 已通过 Gemini CLI OAuth 文件导入；无可导入会话时返回 `BrowserOAuthUnavailable`
- `agentos auth login-interactive [provider=<id>]` 子命令通过 stdin 提示驱动 provider 选择、mode 选择、API key env / profile / set_default 输入；提示文本会回显 `OAuthDefaultsForProvider` 的 `origin` / `note` 元数据。代码位于 `src/cli/auth_interactive.{hpp,cpp}`，复用现有 `auth login` 路径，不重复 AuthManager / SessionStore 逻辑。
- OAuth body 与 bearer token 通过 `src/utils/curl_secret.{hpp,cpp}` 临时文件中转传给 curl（`--data @file` / `-H @file`），不再经 argv；callback 路径校验和 Host 头 loopback 检查在 `src/auth/oauth_pkce.cpp` 完成。

关键偏差：

- `SecureTokenStore` 通过 `ISecureTokenBackend` 抽象为多平台后端：Windows Credential Manager（`wincred:`）、macOS Keychain（`keychain:`）、Linux Secret Service via libsecret（`secret-service:`，可选依赖）。所有后端继续支持 `env:` 引用；缺失系统 keychain 时退化为 `env-ref-only` dev fallback。`auth credential-store` 命令打印当前后端标识，便于 ops 验证生产环境 token 落点。`SecureTokenStore::MakeInMemoryBackendForTesting()` 提供单元测试用的内存后端，CI 中绝不触达真实 keychain。
- 原生 OAuth 的 PKCE start/callback URL 解析校验、一次性 localhost callback listener、authorization-code/refresh-token request 构建、curl-backed HTTP exchange helper、token response 解析、managed AuthSession 持久化 helper、scriptable `oauth-complete` 编排桥接、single-command `oauth-login`、`oauth-defaults` provider discovery（含 origin/note 元数据与 endpoint_status）、repo-local OAuth defaults 覆盖、`oauth-config-validate [--all]` 配置诊断与全 provider 审计、`oauth-start open_browser=true` 系统默认浏览器启动、Gemini Google OAuth 默认 endpoint/scope，以及 provider adapter 在给定 callback/token 参数时的原生 login/refresh 编排已落地。`OAuthProviderDefaults` 现在包含 `origin`(builtin/config/stub/none) 与 `note` 字段，`openai`/`anthropic`/`qwen` 已注册为 `stub` provider 并输出 `endpoint_status=deferred`，表示 AgentOS 不会伪造未公开的授权 / token endpoint；如需提前测试这些 provider，必须通过 `runtime/auth_oauth_providers.tsv` 提供 workspace override，届时状态变为 `available`。Google ADC 已支持通过 gcloud passthrough mint bearer token。
- workspace profile 选择已支持 provider 默认 profile 映射、`auth profiles [provider]` profile 发现，以及 login / OAuth completion 的 `set_default=true` 一步式默认 profile 选择；更完整的多账号策略仍需补齐。
- CLI session passthrough 只做探测与导入，不直接读取或复制外部 CLI token。

下一步以 `plan.md` 的 Auth Completion 阶段为准。
