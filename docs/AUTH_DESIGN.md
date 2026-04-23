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
2. 本地加密文件

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

1. 生成 state 和 PKCE verifier
2. 启动 localhost callback server
3. 打开浏览器
4. 用户登录授权
5. callback 收到 code
6. 校验 state
7. 交换 access/refresh token
8. 写入 SecureTokenStore 和 SessionStore

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

已实现：

- `AuthManager`
- `AuthProviderAdapter`
- `SessionStore`
- `SecureTokenStore` MVP
- `CredentialBroker`
- OpenAI / Anthropic / Gemini / Qwen provider adapter
- API key env-ref 模式
- credential store status 明确标记当前为 `env-ref-only` dev fallback，未接入系统 Keychain
- workspace default profile mapping (`runtime/auth_profiles.tsv`)
- Codex / Claude CLI session passthrough probe
- `auth refresh` 命令、AuthManager refresh 入口与 Adapter refresh 覆盖已接入
- Browser OAuth / PKCE 在 MVP 中显式 defer，调用会返回 `BrowserOAuthNotImplemented`

关键偏差：

- `SecureTokenStore` 当前不是系统 Keychain，只解析 env ref；CLI 会通过 `auth credential-store` 明确标记为 dev fallback。
- OAuth / PKCE、真实 refresh token 交换、cloud credentials 仍是设计目标，不是当前实现。
- workspace profile 选择已支持 provider 默认 profile 映射，但更完整的多账号策略仍需补齐。
- CLI session passthrough 只做探测与导入，不直接读取或复制外部 CLI token。

下一步以 `plan.md` 的 Auth Completion 阶段为准。
