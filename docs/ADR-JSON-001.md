# ADR-JSON-001: Structured JSON Library Adoption

- **Status**: Accepted (Phase 1 partial — see §9)
- **Date**: 2026-04-26 (proposed); 2026-04-27 (accepted with Phase 1 progress)
- **Owners**: AgentOS core / runtime
- **Related**: `completion_review.md` §6 (structured JSON dependency phase 1
  landed; phases 2-N still open), `docs/CODING_GUIDE.md` §1.3 (recommends
  `nlohmann/json`), `CLAUDE.md` (no SQLite without explicit decision — same
  posture for any third-party dependency).

---

## 1. Context

AgentOS currently has **no structured JSON library**. Every JSON value the
runtime touches is produced by hand-rolled string concatenation
(`src/utils/json_utils.{hpp,cpp}`) and consumed by either substring search or
ad-hoc parsing inside the call site that needs the field. This was tolerable for
the MVP because the JSON the kernel emits is shallow and the JSON the kernel
*reads* is mostly ignored (treated as opaque text passed through
`structured_output_json`, `context_json`, `input_schema_json`, …).

Two pressures are now pushing the decision:

1. **Schema validation surface is growing.** `SkillManifest.input_schema_json`
   and `output_schema_json` are stored as raw JSON strings on every skill
   (`file_read`, `file_write`, `file_patch`, `http_fetch`, `workflow_run`,
   every CLI skill registered through `runtime/cli_specs/*.tsv`). The
   `completion_review.md` also describes a planned `SchemaValidator` and a
   Plugin Host that ships JSON-RPC and stdio-JSON protocols
   (`plugin.v1`, `stdio-json-v0`, `json-rpc-v0`). Each of those needs robust
   `parse → validate → project` rather than substring matching.
2. **Provider responses are opaque.**
   `src/auth/provider_adapters.cpp` already reaches for fragments like
   `result.stdout_text.find("\"loggedIn\": true") != std::string::npos`. As more
   providers (Anthropic, Gemini, Qwen) and OAuth response shapes
   (`access_token`, `refresh_token`, `expires_in`) come online, this idiom
   will silently break on whitespace, key order, or unicode escapes.

### 1.1 JSON inventory (current branch)

| File | LOC | What it does with JSON | Mode |
| --- | --- | --- | --- |
| `src/utils/json_utils.{hpp,cpp}` | 19 + 88 | `EscapeJson`, `QuoteJson`, `BoolAsJson`, `NumberAsJson`, `MakeJsonObject`. The single source of write-side helpers. | **Build only** |
| `src/core/audit/audit_logger.cpp` | 133 | Emits one JSONL line per audit event (`task_start`, `route`, `policy`, `step`, `task_end`, `trust`). Six call sites of `MakeJsonObject`. | **Build only** |
| `src/skills/builtin/file_read_skill.cpp` | 60 | Builds `{path, content}` result object. | Build |
| `src/skills/builtin/file_write_skill.cpp` | 71 | Builds `{path, bytes_written}` result object. | Build |
| `src/skills/builtin/file_patch_skill.cpp` | 98 | Builds `{path, replacements}` result object. | Build |
| `src/skills/builtin/http_fetch_skill.cpp` | 72 | Builds `{url, body, stderr, exit_code, timed_out}`. | Build |
| `src/skills/builtin/workflow_run_skill.cpp` | 215 | Builds `{workflow, steps, final_output}` and `{workflow, source, steps, final_output}` for built-in and stored workflows. | Build |
| `src/hosts/cli/cli_skill_invoker.cpp` | 134 | Builds `{command, exit_code, timed_out, stdout, stderr}`. Defines five `CliSpec`s with embedded `input_schema_json` / `output_schema_json` raw strings. | Build + carry-through |
| `src/hosts/cli/cli_spec_loader.cpp` | 111 | Loads TSV cli specs whose columns 9 and 10 are raw `input_schema_json` / `output_schema_json` strings. Falls back to `{"type":"object"}` when missing. | **Carry-through** (no parse) |
| `src/hosts/agents/codex_cli_agent.cpp` | 205 | Builds `{agent, command, exit_code, stdout, stderr, last_message_file}` and an artifact metadata object. Reads provider stdout as plain text. | Build |
| `src/hosts/agents/mock_planning_agent.cpp` | 74 | Builds `{task_type, objective, step_1..3}`. | Build |
| `src/core/orchestration/subagent_manager.cpp` | 350 | Builds `{agents, success_count, total_count, estimated_cost}` aggregate output; passes `context_json` / `constraints_json` through as opaque text. | Build + opaque |
| `src/auth/provider_adapters.cpp` | 379 | **Substring search** (`"loggedIn": true`, `"auth_mode"`) inside CLI probe stdout to decide if a provider session exists. No real parsing. Will need to consume real OAuth `token` / `refresh_token` / `expires_in` responses once Phase B finishes. | **Parse (badly)** |

**Surface totals on this branch:** ~14 files, ~1.95 kLOC of code that touches
JSON, of which `json_utils` itself is 107 LOC. Production-leaning paths that
the `completion_review.md` already promises but are **not yet on this branch**
add at least: `src/core/schema/schema_validator.{cpp,hpp}` (JSON Schema
keyword evaluator), `src/hosts/plugin/*` (stdio-json + json-rpc protocols),
`src/auth/oauth_pkce.cpp` (PKCE + token-exchange JSON parsing). Realistically
the post-Phase-K surface will be 25–30 files.

### 1.2 What is broken or fragile today

- `provider_adapters.cpp` recognises CLI sessions via substring match on
  formatted stdout. Whitespace or locale changes silently break it.
- Every `json_output` field that contains user content is just
  `QuoteJson(result.stdout_text)` — the stdout is escaped once, embedded in a
  JSON string, then often passed back through the kernel as
  `final_output = QuoteJson(other.json_output)`, producing **double-escaped**
  strings that downstream consumers must un-escape by hand.
- `NumberAsJson(double)` is hard-coded to `std::fixed << std::setprecision(2)`,
  losing precision and round-tripping incorrectly for any real numeric output
  (estimated cost, durations).
- Schema JSON strings are never validated at registration time: a malformed
  manifest only fails at the moment a Plugin Host or SchemaValidator tries to
  parse it (which today never happens).
- There is no symmetric *read* helper, so any future feature that needs to
  consume structured output from a CLI / plugin / agent has to write a
  one-off parser. This is the failure mode `completion_review.md` §6 is
  warning about.

---

## 2. Decision Drivers

1. **Symmetric build + parse + validate.** The library must support all three
   without a second dependency.
2. **Single-header or trivially vendored.** AgentOS already builds with Ninja
   on Windows + Ubuntu and is intentionally light on dependencies. The CI
   matrix in `.github/workflows/ci.yml` must keep working without new system
   packages.
3. **MSVC `/W4 /permissive-` clean** and GCC/Clang `-Wall -Wextra -Wpedantic`
   clean (per `CLAUDE.md` build flags).
4. **Permissive license** compatible with the existing repo posture.
5. **Migration cost.** The library must be drop-in-able next to `json_utils`
   so we can migrate per-subsystem rather than in one big bang.
6. **Familiarity / docs.** The `docs/CODING_GUIDE.md` already names
   `nlohmann/json` as the recommended choice — diverging needs justification.

---

## 3. Options

### 3.1 Keep hand-rolled `json_utils`

| Aspect | Verdict |
| --- | --- |
| Build-system impact | None. |
| Compile-time impact | None (107 LOC). |
| License | Project-internal. |
| Surface fit | **Build-only.** No parse, no validate. |
| Portability | Fine. |
| Migration cost | Zero — but the next four planned subsystems (SchemaValidator, Plugin Host json-rpc/stdio, OAuth token exchange, provider response parsing) all need real parse + validate, which means we will write them by hand or end up writing a third-party-quality library inside `utils/`. |

### 3.2 nlohmann/json (`json.hpp`, single-header)

| Aspect | Verdict |
| --- | --- |
| Build-system impact | One file: vendor `json.hpp` under `third_party/nlohmann/` or use `FetchContent`. No CMake target additions beyond an `include_directories`. |
| Compile-time impact | Heavy (~25 kLOC header). Mitigated by including only in `.cpp` files and behind a thin internal façade (`utils/json.hpp`). PCH not required. |
| License | MIT. |
| Surface fit | **Full.** Parse, build, validate (via `nlohmann::json_schema_validator` companion or hand-written walker over `nlohmann::json`), pretty/compact print, ordered keys, comments-tolerant variant. |
| Portability | Windows + Linux + macOS. Used in millions of projects. MSVC `/W4 /permissive-` clean since 3.11. |
| Migration cost | Low. The API is value-semantic; we can wrap the pieces we need behind `agentos::JsonValue` and migrate the ~14 files in 2–3 PRs. |

### 3.3 simdjson

| Aspect | Verdict |
| --- | --- |
| Build-system impact | Two files (`simdjson.h` + `simdjson.cpp`) or a CMake submodule. |
| Compile-time impact | Moderate (the `.cpp` is non-trivial). Runtime parse speed is best-in-class. |
| License | Apache-2.0 / MIT dual. |
| Surface fit | **Parse-only.** Build (DOM mutation, serialization) is awkward — we would still need a writer. Schema validation: nothing built-in. |
| Portability | Fine on x86-64 + ARM64. CI works on Windows + Linux. |
| Migration cost | High. Two libraries (simdjson + a writer + a validator) defeat the goal of replacing `json_utils` with one dependency. |

### 3.4 RapidJSON

| Aspect | Verdict |
| --- | --- |
| Build-system impact | Header-only, vendored as a directory. |
| Compile-time impact | Lighter than nlohmann. |
| License | MIT-style (BSD with Tencent attribution). |
| Surface fit | **Full.** Parse, build, JSON Schema validation built in (`rapidjson/schema.h`). |
| Portability | Windows + Linux + macOS. MSVC clean but warnings under `/W4` historically required suppression pragmas. |
| Migration cost | Medium. The `Document` / `Value` / `Allocator` model is more verbose and less ergonomic than nlohmann; every helper we write is more code. RapidJSON also drifted into low-maintenance status (long stretches with no release), which is a risk for a long-lived dependency. |

### 3.5 Boost.JSON

| Aspect | Verdict |
| --- | --- |
| Build-system impact | Either pull in Boost (large) or use the standalone `boost/json` distribution. Either way we acquire a Boost dependency footprint we have so far avoided. |
| Compile-time impact | Moderate; with `BOOST_JSON_STANDALONE` it is reasonable but still a Boost build. |
| License | Boost Software License. |
| Surface fit | **Parse + build.** No JSON Schema validator in Boost.JSON itself. |
| Portability | Excellent. |
| Migration cost | Medium-high. Adopting any Boost component for the first time is a directional choice for the whole repo, and we still need a separate validator. |

### 3.6 Comparison summary

| Option | Parse | Build | Validate | Single-header | Compile cost | License | Migration cost |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Hand-rolled | ✗ | ⚠ string-concat | ✗ | n/a | none | n/a | will pay it later |
| **nlohmann/json** | ✓ | ✓ | ✓ (companion) | ✓ | high | MIT | **low** |
| simdjson | ✓✓ | ✗ | ✗ | ✗ | medium | Apache-2.0 | high |
| RapidJSON | ✓ | ✓ | ✓ | ✓ (dir) | medium | MIT-like | medium |
| Boost.JSON | ✓ | ✓ | ✗ | ✗ | medium | BSL | medium-high |

---

## 4. Decision

**Adopt `nlohmann/json` v3.11+ as the single JSON dependency.**

Reasons:

1. It is the only option that gives us *parse + build + validate* with one
   header and no runtime deps, which exactly matches the surface area
   (`json_utils` builders, future `SchemaValidator`, plugin protocols, OAuth
   responses, provider probes).
2. `docs/CODING_GUIDE.md` §1.3 already names it as the recommended choice; a
   different decision would need a stronger reason than "the build is
   slightly faster".
3. Migration can be **incremental**: a thin façade (`src/utils/json.hpp` =
   `using JsonValue = nlohmann::json;` plus a few helpers) lets us replace
   `MakeJsonObject` call sites file-by-file without breaking the rest of the
   tree.

**Vendoring vs FetchContent:** vendor a single `third_party/nlohmann/json.hpp`
checked into the repo. This avoids a network requirement during CMake
configure, keeps offline builds working (the project explicitly avoids that
in `CLAUDE.md`'s build instructions), and keeps the dependency tree empty.
Pin the version (`v3.11.x`) and document the upgrade procedure in the
migration plan below.

**What we explicitly do not adopt now:**

- `nlohmann::json_schema_validator` is a separate repository; we will revisit
  it in Phase 3 of the migration. The first cut of `SchemaValidator` will be
  written *against* `nlohmann::json` so the validator can be swapped later
  without touching call sites.
- No SIMD parsing (simdjson). Parse throughput is not on the critical path
  for the kernel; provider/plugin responses are bounded by the network and
  process boundary.

---

## 5. Consequences

### Positive

- One canonical type (`nlohmann::json`) for build, parse, and validate, so
  future features (Plugin Host, OAuth, schema-driven CLI validation) inherit
  it for free.
- `provider_adapters.cpp` substring matches go away; CLI probes parse the
  status JSON properly.
- Skill `json_output` stops double-escaping when one skill embeds another's
  output (the value can be inserted as a sub-object instead of as a
  `QuoteJson(...)` string).
- `NumberAsJson(double)` precision bug disappears.
- `cli_spec_loader.cpp` can validate that columns 9/10 contain *parseable*
  schema strings at load time.

### Negative

- ~25 kLOC header included in roughly 14 translation units. Estimated total
  build time hit on a clean Ninja build: **+15–25 s wall** on a modern
  laptop, less with `ccache` / sccache. This is the single biggest cost;
  acceptable given the scope.
- One vendored file in the repo (`third_party/nlohmann/json.hpp`). The
  contributing guide should mention not editing it locally.
- The team gains an "official" way to do JSON, which means existing call
  sites that work *will* be touched during the migration; we accept that as
  the cost of removing the silent-substring-match category of bug.

### Neutral

- License (MIT) is compatible with everything in the repo.
- Windows / Linux / macOS portability is unchanged; the CI matrix does not
  need new packages.

---

## 6. Migration Plan

The migration is sequenced so each step is independently mergeable, leaves the
tree green, and can be reverted without unwinding earlier steps.

### Phase 0 — Decision and vendoring (separate PR, no semantic changes)

1. Vendor `third_party/nlohmann/json.hpp` (v3.11.x) and add a single
   `target_include_directories(agentos_core SYSTEM PRIVATE third_party)`
   line in `CMakeLists.txt` (deliberately `SYSTEM` so its headers do not
   produce `/W4 /permissive-` warnings against our flags).
2. Introduce `src/utils/json.hpp` as the project-local façade:
   `namespace agentos { using JsonValue = nlohmann::json; }` plus tiny
   helpers (`ParseJsonOrEmpty`, `DumpCompact`, `DumpPretty`).
3. **No call-site changes.** `json_utils` continues to exist alongside the
   new façade.
4. CI must stay green; the build-time delta is the gate.

### Phase 1 — Plugin host first (highest-value carrier)

When the Plugin Host (`stdio-json-v0`, `json-rpc-v0`) lands per
`completion_review.md` Phase K, write it directly against
`agentos::JsonValue`. This is the place where parse + validate + project is
load-bearing and where hand-rolled JSON would do the most damage. Doing this
*first* means we never grow a hand-rolled parser inside `hosts/plugin/`.

### Phase 2 — Schema validator

Implement `src/core/schema/schema_validator.{cpp,hpp}` against
`nlohmann::json`. Cover the keyword set the project actually uses
(`type`, `required`, `properties`, `items`, `enum`, `pattern`, numeric
ranges, `additionalProperties`). Wire it into:

- `SkillRegistry` (validate manifests at registration time).
- `cli_spec_loader.cpp` (validate columns 9/10 at load time).
- Plugin Host (validate request/response against `output_schema_json`).

This is the step that retires the recurring `R"({"type":"object",...})"`
inline strings: keep them, but assert they parse.

### Phase 3 — Auth and provider adapters

1. Replace `provider_adapters.cpp`'s substring matches with proper parsing of
   the CLI probe stdout (`claude auth status`, `codex login status`'s
   structured forms, the Codex `auth.json` file).
2. When OAuth PKCE / token exchange lands, parse `access_token`,
   `refresh_token`, `expires_in`, `id_token` through `JsonValue` and round-
   trip them through `SecureTokenStore` instead of regex.

### Phase 4 — Audit logger and skill outputs

1. Migrate `audit_logger.cpp` to build a `JsonValue` and call `dump()`.
   Dropping `MakeJsonObject` here also eliminates the per-line
   `QuoteJson(CurrentTimestamp())` boilerplate.
2. Migrate the five builtin skills, `cli_skill_invoker.cpp`, and the agent
   adapters in alphabetical order. Each becomes a small, mechanical PR.
3. Stop double-escaping: where one skill currently embeds another's output as
   `QuoteJson(other.json_output)`, switch to embedding the parsed value.

### Phase 5 — Retire `json_utils`

1. Once no `.cpp` includes `utils/json_utils.hpp`, delete it.
2. Update `docs/CODING_GUIDE.md` §1.3 to reflect that the recommendation is
   now realised.
3. Record the migration in `plan.md` Progress Log and flip the
   `completion_review.md` §6 ⚠️ row to ✅.

### Phase 6 — Optional: schema-validator companion

If the keyword set we hand-rolled in Phase 2 starts to drift toward full JSON
Schema Draft 7, evaluate `pboettch/json-schema-validator` (separate
single-header companion to nlohmann/json). Out of scope for this ADR.

### Effort estimate

| Phase | Estimated effort |
| --- | --- |
| 0 — Vendor + façade | 0.5 person-day |
| 1 — Plugin host (lands with Phase K, no extra cost) | 0 person-days direct |
| 2 — SchemaValidator + wiring | 2.0 person-days |
| 3 — Auth / provider parsing | 1.5 person-days |
| 4 — Audit logger + 5 skills + 2 agents + 1 invoker + subagent manager | 2.0 person-days |
| 5 — Retire `json_utils`, docs, CI | 0.5 person-day |
| **Total** | **~6.5 person-days** of focused work, spread across 4–6 PRs. |

Effort excludes Phase K (Plugin Host) itself, since that work is on the
roadmap independently of this ADR — Phase 1 above only constrains *how* it
will be implemented.

---

## 7. Rejected Alternatives — One-line Summary

- **Keep `json_utils`**: defers the cost; each new JSON feature pays it again.
- **simdjson**: parse-only; we still need a writer and a validator.
- **RapidJSON**: viable backup, but ergonomics and maintenance cadence are
  weaker than nlohmann/json. Reconsider only if compile-time becomes a real
  blocker.
- **Boost.JSON**: introduces a Boost surface we have no other reason to take
  on, and still leaves validation unsolved.

---

## 9. Implementation Progress (added 2026-04-27)

This ADR was promoted from Proposed to Accepted after Phase 1 landed. The
actual implementation diverges from the migration plan in §6 in two ways: the
dependency is fetched via `FetchContent` rather than vendored, and the first
migration targets were chosen by call-site risk (substring-match auth probes,
per-step plan extraction in subagent orchestration, V2 streaming adapters)
rather than by Plugin Host first.

### 9.1 What landed (Phase 1)

| Subsystem | File | What changed |
| --- | --- | --- |
| Build system | `CMakeLists.txt` | `FetchContent_Declare(nlohmann_json URL .../v3.11.3.tar.gz)` + `target_link_libraries(agentos_core PUBLIC nlohmann_json::nlohmann_json)`. No vendoring. |
| Retired build helpers | `src/utils/json_utils.{hpp,cpp}` | The temporary compatibility helpers were removed from the build after all production and test call sites moved to `nlohmann::json` / `nlohmann::ordered_json`. The fixed `setprecision(2)` truncation bug from §1.2 is gone with the helper module. |
| Subagent orchestration | `src/core/orchestration/subagent_manager.cpp` | `ExtractPlanActions` parses `plan_steps` as `nlohmann::json`. Substring-style action injection no longer accepted; `plan_steps[].action` must be a string field on an object. The aggregate orchestration output (`agents`, `roles`, counts, `estimated_cost`, `agent_outputs[]`, optional `decomposition_agent`) plus decomposition/per-agent context JSON now uses `nlohmann::ordered_json`, removing the manager's dependency on `utils/json_utils.hpp`. |
| Auth provider probes | `src/auth/provider_adapters.cpp` | CLI probe stdout is now parsed (`nlohmann::json::parse`) inside a `try/catch (const nlohmann::json::exception&)` instead of `find("\"loggedIn\": true")`. Whitespace/key-order brittleness from §1.2 retired. |
| V2 streaming adapters | `src/hosts/agents/{anthropic,codex_cli,gemini,qwen}_agent.cpp` | SSE/NDJSON event parsing and request-body construction use `nlohmann::json`. Each adapter no longer hand-builds JSON strings for its own streaming protocol. |
| Gemini provider adapter | `src/hosts/agents/gemini_agent.cpp` | Legacy REST / CLI-session output envelopes and the Gemini `generateContent` request body now use `nlohmann::ordered_json`; REST response text extraction parses the response with `nlohmann::json` instead of scanning for `"text"`. This removes `utils/json_utils.hpp` from the Gemini adapter. |
| Anthropic provider adapter | `src/hosts/agents/anthropic_agent.cpp` | Legacy REST / Claude CLI / streaming output envelopes and the Messages request body now use `nlohmann::ordered_json`; REST response text extraction parses `content[].text` with `nlohmann::json`. This removes `utils/json_utils.hpp` from the Anthropic adapter. |
| Codex CLI provider adapter | `src/hosts/agents/codex_cli_agent.cpp` | Legacy CLI and V2 NDJSON streaming output envelopes plus artifact metadata now use `nlohmann::ordered_json`. This removes `utils/json_utils.hpp` from the Codex CLI adapter. |
| Qwen provider adapter | `src/hosts/agents/qwen_agent.cpp` | Legacy REST / V2 streaming output envelopes, Chat Completions request bodies, REST response content extraction, and V2 constraint projection now use `nlohmann::ordered_json` / `nlohmann::json`. This removes `utils/json_utils.hpp` from the Qwen adapter. |
| `workflow_run` skill | `src/skills/builtin/workflow_run_skill.cpp` | Built-in `write_patch_read` and stored-workflow paths now build the result object via `nlohmann::json` and embed each step's `json_output` as a parsed sub-object via `EmbedStepOutput()`. Closes the double-escaping bug from §1.2 — `final_output` is now `{"path":...,"content":...}` instead of `"{\"path\":...,\"content\":...}"`. Falls back to a JSON string when the child output does not parse, preserving backwards compatibility. |
| Builtin file skills | `src/skills/builtin/{file_read,file_write,file_patch,http_fetch}_skill.cpp` | All four MakeJsonObject + QuoteJson result construction paths replaced with `nlohmann::json` builders. `json_output` is now `dump()`'d so escaping happens once, deterministically. |
| CLI skill invoker | `src/hosts/cli/cli_skill_invoker.cpp` | The shared `{command, exit_code, timed_out, stdout, stderr}` result for every CLI skill now goes through `nlohmann::json`. Affects `rg_search`, `git_status`, `git_diff`, `curl_fetch`, `jq_transform`, and every external CLI spec loaded from `runtime/cli_specs/*.tsv`. |
| Local planning agent | `src/hosts/agents/local_planning_agent.cpp` | The structured plan output (`content`, `agent`, `provider`, `task_type`, `objective`, `plan_steps[]`) is built as a `nlohmann::json` value before normalization. Aligns the local planner with the four V2 streaming adapters which already built request/event JSON via nlohmann. |
| Agent loop | `src/core/loop/agent_loop.cpp` | Legacy `AgentTask.context_json` and `constraints_json` construction now uses `nlohmann::ordered_json`, removing the core loop's dependency on `utils/json_utils.hpp` while preserving compact JSON passed to legacy adapters. |
| Agent result normalizer | `src/core/orchestration/agent_result_normalizer.cpp` | `agent_result.v1` envelopes now parse provider output once and build the normalized `summary` / `content` / `model` / `artifacts` / `metrics` / `provider_metadata` / `raw_output` shape through `nlohmann::ordered_json`. Malformed provider output is embedded as a JSON string instead of being treated as object-shaped text. |
| Audit logger | `src/core/audit/audit_logger.cpp` | All seven `record_*` emission sites and all four compaction-recovery emission sites (task_start/route/step/task_end + scheduler_run rebuild) now build `nlohmann::ordered_json` and `dump()` it. Insertion-order preservation keeps the on-disk JSONL byte-identical to the previous `MakeJsonObject` output. |
| Diagnostics CLI | `src/cli/diagnostics_commands.cpp` | The `agentos diagnostics format=json` snapshot now builds a typed `nlohmann::ordered_json` object, including nested provider/agent arrays, instead of assembling the response through `MakeJsonObject` / `QuoteJson`. Text output remains unchanged. |
| OAuth token-exchange parsing | `src/auth/oauth_pkce.cpp` | `JsonStringField` and `JsonIntField` (used by `ParseOAuthTokenResponse` to extract `access_token`, `refresh_token`, `token_type`, `expires_in`, `error`, `error_description`) now go through `nlohmann::json::parse`. The legacy `SkipWhitespace`/`ParseJsonStringAt`/`FindTopLevelJsonValue` helpers were deleted after the typed parser landed. Whitespace, key order, unicode escapes, and nested objects in token responses now round-trip correctly. |
| CLI spec loader | `src/hosts/cli/cli_spec_loader.cpp` | `IsLikelyJsonObjectString` (bracket-shape check) replaced with `IsParseableJsonObject` (`nlohmann::json::parse` + `is_object()`). Manifest schema columns 9/10 now fail at load time on malformations the legacy check would have accepted (unterminated strings, mismatched braces inside content, invalid escape sequences). |
| Plugin manifest loader | `src/hosts/plugin/plugin_manifest_loader.cpp` | TSV schema columns use parse-time validation, and `*.json` plugin manifests are now parsed through `nlohmann::ordered_json` for all manifest fields. Object-valued `input_schema_json` / `output_schema_json` are preserved via `dump()`. Strict JSON parsing means non-standard trailing commas are rejected. |
| Shared schema companion | `src/core/schema/schema_validator.{hpp,cpp}` | The shared schema module now exposes `IsParseableJsonObjectSchema()`, and both CLI spec loading and plugin manifest loading use it for `input_schema_json` / `output_schema_json` parse-time object validation instead of carrying duplicate local helpers. It also owns runtime `required`, `properties.*.type`, `properties.*` string/numeric/array item-count and item value constraint, object-shape (`minProperties`, `maxProperties`, `propertyNames`), `additionalProperties:false`, `dependentRequired`, legacy array-valued `dependencies`, object-level `not.required`, basic `if` / `then` / `else`, and `allOf` / `anyOf` / `oneOf` required-branch checks for JSON-object output validation through the `JsonObject*ValidationError()` helpers. |
| Plugin JSON-RPC helpers | `src/hosts/plugin/plugin_json_rpc.cpp` | `JsonRpcRequestForPlugin()` constructs the JSON-RPC 2.0 request envelope and `params` object through `nlohmann::ordered_json` instead of `MakeJsonObject` / `QuoteJson`. `JsonRpcOutputError()` and `JsonRpcResultObject()` now parse responses through `nlohmann::ordered_json`, preserving the same public error messages while removing the helper-parser dependency from the JSON-RPC path. |
| Plugin output schema validator | `src/hosts/plugin/plugin_schema_validator.cpp` | `PluginOutputSchemaError()` delegates the existing supported keyword subset (`required`, `properties.*.type`, string constraints, array `minItems` / `maxItems` / `uniqueItems` / `prefixItems` / `contains` / `minContains` / `maxContains` / `items.type` / `items.const` / `items.enum` / `items.minLength` / `items.maxLength` / `items.pattern` / numeric `items` bounds / `items.multipleOf` / object-item required/property/property-name/dependency/not-required/shape constraints, numeric bounds / `multipleOf`, object/property constraints, dependencies, conditionals, and allOf/anyOf/oneOf) to shared `core/schema` helpers instead of carrying plugin-local schema parsing. |
| Plugin skill invoker | `src/hosts/plugin/plugin_skill_invoker.cpp` | The `{plugin, manifest_version, protocol, lifecycle_mode, lifecycle_event, plugin_output}` skill result envelope now uses `nlohmann::ordered_json`. `json-rpc-v0` skill output embeds the already-validated `JsonRpcResultObject()` instead of re-scanning stdout for a `"result"` substring and brace-counting by hand. |

### 9.2 Divergences from §6 migration plan

- **Phase 0 (vendor `third_party/nlohmann/json.hpp`)**: replaced with
  `FetchContent`. Reason: the v3.11.3 release is small, CMake handles the
  download once on first configure, and we keep a single source of truth for
  the version (`URL` line in `CMakeLists.txt`). Cost: first-time configure on
  an offline machine fails; the project-wide CI (Windows + Ubuntu) caches the
  tarball.
- **Phase 1 ordering**: §6 picks Plugin Host first as the highest-value
  carrier. The actual Phase 1 picked subagent_manager + provider_adapters +
  V2 adapters because those were active call sites whose substring/string-
  concat behavior was already manifesting in real bugs (§1.2). The Plugin
  Host schema validator landed later as a Phase 2 slice.

### 9.3 What is still hand-rolled (Phase 2-N open)

- ~~`src/utils/json_utils.{hpp,cpp}`~~ retired. No `.cpp` files include
  `utils/json_utils.hpp`; no `MakeJsonObject` / `QuoteJson` / `BoolAsJson` /
  `NumberAsJson` call sites remain under `src/` or `tests/`.
- ~~`src/core/audit/audit_logger.cpp`~~ migrated to `nlohmann::ordered_json`.
- ~~`src/cli/diagnostics_commands.cpp`~~ migrated to `nlohmann::ordered_json`.
- ~~`src/core/orchestration/agent_result_normalizer.cpp`~~ migrated to
  `nlohmann::ordered_json`.
- ~~`src/core/loop/agent_loop.cpp`~~ migrated to `nlohmann::ordered_json`
  for legacy agent context/constraint JSON.
- ~~`src/core/orchestration/subagent_manager.cpp`~~ migrated to
  `nlohmann::ordered_json` for aggregate orchestration output and legacy
  agent context JSON.
- ~~`src/hosts/plugin/plugin_schema_validator.cpp`~~ migrated to
  `nlohmann::ordered_json` while preserving the existing keyword subset and
  error strings.
- `src/core/schema/` — shared parse-time object-schema validation has started
  through `IsParseableJsonObjectSchema()`, and runtime JSON-object `required`
  / `properties.*.type` / `properties.*` string and numeric constraint /
  object-shape / `additionalProperties:false` / dependency / conditional /
  branch-combinator validation has started through
  `JsonObjectRequiredFieldValidationError()`,
  `JsonObjectPropertyTypeValidationError()`,
  `JsonObjectStringConstraintValidationError()`,
  `JsonObjectNumericConstraintValidationError()`,
  `JsonObjectShapeConstraintValidationError()`, and
  `JsonObjectAdditionalPropertiesValidationError()`,
  `JsonObjectDependentRequiredValidationError()`,
  `JsonObjectNotRequiredValidationError()`, and
  `JsonObjectIfThenElseRequiredValidationError()`, and
  `JsonObjectRequiredBranchValidationError()`. Skill input schema validation
  has started moving onto typed parsing for top-level `required`, `propertyNames`,
  `minProperties`, `maxProperties`, `dependentRequired`, and legacy array-valued
  `dependencies`, object-level `not.required`, basic `if` / `then` / `else`,
  `allOf` / `anyOf` / `oneOf` required-branch checks, and
  `additionalProperties:false`, plus `properties.*.type` and `properties.*`
  string constraints (`const`, `enum`, `minLength`, `maxLength`, `pattern`) and
  numeric constraints (`minimum`, `maximum`, `exclusiveMinimum`,
  `exclusiveMaximum`, `multipleOf`). `ValidateRequiredInputFields()` now parses
  `input_schema_json` once and passes the typed object through the input helper
  chain. The public `JsonObject*ValidationError()` helpers now share the same
  `ParseJsonObject()` / `ParseJsonObjectPair()` guards and
  `JsonObjectValidationErrorForParsedPair()` delegate wrapper for
  schema/object parsing instead of carrying per-helper parse/exception
  boilerplate. `JsonObjectSchemaValidationError()` now routes through the same
  parsed-pair delegate wrapper via `JsonObjectSchemaValidationErrorForParsed()`,
  and post-type keyword checks flow through
  `JsonObjectSchemaConstraintValidationErrorForParsed()` so the output path has
  a single constraint-ordering collector. Output parsed-validator chains now
  share `FirstJsonObjectParsedValidationError()` for stable first-error
  ordering across required, type, and post-type constraint groups.
  JSON-object output constraint helpers share
  `JsonObjectFieldConstraintError()` for the
  stable `<subject> field has invalid constraint: <field>:<constraint>`
  formatter and `JsonObjectFailedConstraintError()` for the stable
  `<subject> failed <constraint>` formatter. Output `required` and
  `properties.*.type` failures share `JsonObjectRequiredFieldError()` and
  `JsonObjectFieldTypeError()` formatters. Skill input constraint failures now
  share `JsonObjectInputFieldConstraintFailure()`,
  `JsonObjectInputSchemaConstraintFailure()`, and
  `JsonObjectInputSchemaFieldConstraintFailure()` formatters for stable
  `field:*` and `schema:*` tokens, and `CollectJsonObjectInputFailures()` /
  `FirstJsonObjectValidationError()` now adapt shared visitors for all-input
  failure collection vs first-output-error returns in dependency,
  `additionalProperties:false`, `not.required`, property-count,
  conditional-required, and branch-combinator checks.
  `CollectJsonObjectInputPropertyConstraintFailures()` and
  `FirstJsonObjectPropertyConstraintError()` now adapt present property-value
  visitors for string/numeric input collection vs output first-error formatting.
  String and numeric property constraint wrappers now also route through
  named input collection and output first-error adapters:
  `CollectJsonObjectInputStringConstraintFailures()`,
  `CollectJsonObjectInputNumericConstraintFailures()`,
  `FirstJsonObjectStringConstraintError()`, and
  `FirstJsonObjectNumericConstraintError()`.
  Top-level required-field, property-type mismatch, and `propertyNames`
  wrappers also use the shared input-collection / output-first-error adapter
  path: required-field input collection now goes through
  `CollectJsonObjectInputRequiredFieldFailures()` via
  `JsonObjectInputRequiredFieldFailures()`, output required/type checks go
  through `FirstJsonObjectRequiredFieldError()` and
  `FirstJsonObjectPropertyTypeError()`, property-type input collection goes
  through `CollectJsonObjectInputPropertyTypeFailures()`, and `propertyNames`
  field-list traversal shares `CollectJsonObjectInputPropertyNameConstraintFailures()` and
  `FirstJsonObjectPropertyNameConstraintError()`. Object property-count
  checks share `CollectJsonObjectInputPropertyCountFailures()` and
  `FirstJsonObjectPropertyCountError()` adapters. Dependency checks share
  `CollectJsonObjectInputDependentRequiredFailures()` and
  `FirstJsonObjectDependentRequiredError()` adapters.
  `additionalProperties:false` checks share
  `CollectJsonObjectInputAdditionalPropertyFailures()` and
  `FirstJsonObjectAdditionalPropertyError()` adapters.
  `not.required` checks share `CollectJsonObjectInputNotRequiredFailures()`
  and `FirstJsonObjectNotRequiredError()` adapters.
  Basic `if` / `then` / `else` required-field checks share
  `CollectJsonObjectInputIfThenElseRequiredFailures()` and
  `FirstJsonObjectIfThenElseRequiredError()` adapters.
  Branch-combinator checks share
  `CollectJsonObjectInputAllOfRequiredBranchFailures()`,
  `CollectJsonObjectInputRequiredBranchCountFailures()`,
  `FirstJsonObjectAllOfRequiredBranchError()`, and
  `FirstJsonObjectRequiredBranchCountError()` adapters.
  Top-level `required` missing-field traversal is shared across input and output validation, property type
  mismatch traversal is shared while preserving input/output type semantics,
  string `const` / `enum` value constraint evaluation is shared while preserving
  input/output ordering, string length/pattern constraint evaluation is shared, and numeric
  `minimum` / `maximum` / exclusive bound / `multipleOf` evaluation is shared.
  Both input/output property validators share typed `properties` and present
  property-value traversal helpers, and `additionalProperties:false` uses one declared-property
  comparison helper. Dependency checks and
  required-branch matching/counting also share typed helpers across input and
  output validators, and `propertyNames` constraint evaluation shares one typed
  helper while preserving input and output error formatting. Object property
  count checks (`minProperties` / `maxProperties`) also share typed evaluation
  across input and output validators. Object-level `not.required` branch
  matching, `allOf` required-branch traversal, `anyOf` / `oneOf` branch-count
  failure evaluation, and basic `if` / `then` / `else` required-field checks
  also share typed branch traversal while preserving input and output error
  formatting. Full consolidation of the remaining
  schema keyword evaluation duplication remains future work.
- ~~`src/hosts/plugin/plugin_json_rpc.cpp`~~ request rendering and response
  validation both parse/build through `nlohmann::ordered_json`.
- ~~`src/hosts/cli/cli_spec_loader.cpp`~~ schema columns now parse-time
  validated via `nlohmann::json::parse`.
- ~~`src/hosts/plugin/plugin_manifest_loader.cpp`~~ TSV schema columns and
  JSON manifests parse through `nlohmann::ordered_json`; the old
  `plugin_json_utils` helper module has been deleted.
- ~~`src/hosts/plugin/plugin_skill_invoker.cpp`~~ migrated to
  `nlohmann::ordered_json` for skill output envelopes and JSON-RPC result
  projection.
- ~~`src/auth/oauth_pkce.cpp`~~ token-exchange parsing migrated, and the
  legacy hand-rolled helpers (`SkipWhitespace`, `ParseJsonStringAt`,
  `FindTopLevelJsonValue`) have been deleted.
- `src/skills/builtin/*.cpp` — all five builtin skills migrated.
- ~~`src/hosts/cli/cli_skill_invoker.cpp`~~ migrated.
- ~~`src/hosts/agents/local_planning_agent.cpp`~~ migrated.
- ~~`src/hosts/agents/gemini_agent.cpp`~~ migrated to
  `nlohmann::ordered_json` for legacy provider output envelopes and
  `generateContent` request construction.
- ~~`src/hosts/agents/anthropic_agent.cpp`~~ migrated to
  `nlohmann::ordered_json` for legacy REST / Claude CLI / streaming output
  envelopes and Messages request construction.
- ~~`src/hosts/agents/codex_cli_agent.cpp`~~ migrated to
  `nlohmann::ordered_json` for legacy CLI / V2 streaming output envelopes and
  artifact metadata.
- ~~`src/hosts/agents/qwen_agent.cpp`~~ migrated to
  `nlohmann::ordered_json` / `nlohmann::json` for legacy REST / V2 streaming
  output envelopes, Chat Completions request bodies, response content
  extraction, and V2 constraint projection.

### 9.4 Effort remaining

| Phase | Status | Notes |
| --- | --- | --- |
| 0 — Vendor + façade | ✅ (modified — FetchContent instead of vendor) | No façade header; call sites include `<nlohmann/json.hpp>` directly. |
| 1 — Plugin host first | 🔄 Partial — Plugin Host parse path deferred; provider_adapters / subagent_manager / V2 adapters landed instead. | See §9.1. |
| 2 — Schema validator | ✅ for Plugin Host — `cli_spec_loader.cpp` and `plugin_manifest_loader.cpp` do parse-time validation of schema columns; JSON plugin manifests, `plugin_json_rpc.cpp` request rendering / response validation, `plugin_schema_validator.cpp` keyword evaluation, and `plugin_skill_invoker.cpp` skill output envelopes are migrated to `nlohmann::ordered_json`. | `src/hosts/plugin/plugin_json_utils.{hpp,cpp}` deleted, and `src/hosts/plugin/` no longer includes `utils/json_utils.hpp`. Shared `src/core/schema/` helpers now own the supported plugin output schema keyword checks. |
| 3 — Auth and provider adapters | ✅ — provider_adapters CLI probe parsing **and** OAuth token-exchange JSON (`oauth_pkce.cpp::ParseOAuthTokenResponse`) both go through `nlohmann::json` now. | |
| 4 — Audit logger and skill outputs | ✅ — `audit_logger.cpp` migrated to `nlohmann::ordered_json`; all five builtin skills migrated; `cli_skill_invoker` and `local_planning_agent` migrated. | |
| 5 — Retire `json_utils` | ✅ — all production and test callers migrated, stale include-only references removed, `src/utils/json_utils.{hpp,cpp}` deleted, and `CMakeLists.txt` no longer builds the helper module. | |
| 6 — Schema validator companion | 🔄 Partial — `IsParseableJsonObjectSchema()` now lives in `src/core/schema/schema_validator.{hpp,cpp}` and is reused by CLI spec loading plus plugin manifest loading. JSON-object output validation for `required`, `properties.*.type`, `properties.*` string/numeric/array item-count, uniqueness, tuple-prefix, contains-count, and scalar/object item value constraint, object-shape (`minProperties`, `maxProperties`, `propertyNames`), `additionalProperties:false`, `dependentRequired`, legacy array-valued `dependencies`, `not.required`, basic `if` / `then` / `else`, and `allOf` / `anyOf` / `oneOf` checks now lives in `core/schema` via the `JsonObject*ValidationError()` helpers, which share the same typed object parse guard, parsed-pair delegate wrapper, top-level required missing-field traversal, property type mismatch traversal, string `const` / `enum` value constraint evaluator, string length/pattern constraint evaluator, numeric range / `multipleOf` constraint evaluator, array `minItems` / `maxItems` / `uniqueItems` / `prefixItems` / `contains` / `minContains` / `maxContains` / `items.type` / `items.const` / `items.enum` / `items.minLength` / `items.maxLength` / `items.pattern` / numeric `items` bounds / `items.multipleOf` / object-item required/property/property-name/dependency/not-required/shape constraint evaluator, `properties` and present property-value traversal helpers, additional-property evaluator, dependency traversal, required-branch matching/counting helpers, property-name constraint evaluator, object property-count evaluator, not.required branch evaluator, allOf required-branch traversal, anyOf/oneOf branch-count failure evaluator, and conditional required-branch traversal. `JsonObjectSchemaValidationError()` now lets plugin output validation parse the schema/output pair once through that shared delegate path and run the same ordered keyword checks. Skill input top-level `required`, `propertyNames`, `minProperties`, `maxProperties`, `dependentRequired`, legacy array-valued `dependencies`, `not.required`, basic `if` / `then` / `else`, `allOf` / `anyOf` / `oneOf` required-branch extraction, `additionalProperties:false`, `properties.*.type`, and `properties.*` string/numeric constraints also use typed parsing; runtime skill input validation now parses the schema once per call instead of once per keyword helper, routes typed constraint aggregation through `JsonObjectInputConstraintFailures()`, shares formatter helpers for stable `field:*` and `schema:*` failure tokens, and uses shared visitor adapters for all-input failure collection vs first-output-error returns in required-field collection, property-type input collection, output required/type checks, propertyNames field-list traversal, object property-count checks, dependency checks, `additionalProperties:false` checks, `not.required` checks, conditional-required checks, branch-combinator checks, and present property-value string/numeric checks. | The older input keyword scanners have been retired; remaining work is reducing duplication between the input and JSON-object output helper families. |

### 9.5 Decision posture

This ADR is now **Accepted**. Reopen as `Superseded` only if a future ADR
proposes a different JSON dependency (e.g., a parse-throughput crisis pushes
us to simdjson + a writer). Phases 2-N remain open work items tracked in
`plan.md` / `completion_review.md` rather than this ADR.

---

## 8. Open Questions

1. Do we want JSON Schema validation strictly at registration time (fail
   fast) or also at execution time (defence in depth)? Suggested: both, with
   execution-time validation behind a `policy.validate_schemas=true` flag so
   it can be disabled in hot paths.
2. Pretty vs compact dump in `audit.log`? Current behaviour is
   single-line-per-event JSONL — we keep that; `dump()` (no argument) is the
   default.
3. Where does the JSON façade header live: `src/utils/json.hpp` or
   `src/core/json.hpp`? Suggested: `src/utils/json.hpp` matches the existing
   `json_utils.hpp` slot.
