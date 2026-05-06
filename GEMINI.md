# AgentOS - AI Agent Operating System Kernel

AgentOS is a C++20-based kernel designed to provide a stable foundation for AI Agents. It focuses on a minimal core, managed tool execution (Skills/CLI), collaboration with external "expert" sub-agents, and a continuous learning system that evolves through execution reviews.

## Core Design Principles
- **Minimal Stable Kernel:** Responsible for task loops, routing, policy enforcement, memory, and auditing.
- **Externalized Capabilities:** Tools are connected via Skills, CLI Hosts, Plugin Hosts, and Sub-agent adapters.
- **Learning from Execution:** Records tasks and outcomes to generate Workflows and refine routing strategies.
- **Security First:** Built-in policy engine, permission model, and sensitive data redaction.

## Project Structure
- `src/core/`: The kernel's heart (AgentLoop, Router, PolicyEngine, Registry, etc.).
- `src/auth/`: Authentication management, provider adapters (OpenAI, Gemini, Claude, Qwen), and secure token storage.
- `src/hosts/`: Execution environments for CLI tools (`cli/`), external agents (`agents/`), and multi-language plugins (`plugin/`).
- `src/memory/`: Persistent storage for task logs, lessons, and automatically generated workflows.
- `src/skills/`: Built-in capabilities like `file_read`, `file_write`, `http_fetch`, and `workflow_run`.
- `src/trust/`: Identity management and pairing system for remote task triggers.
- `src/scheduler/`: Management of one-time and recurring scheduled tasks.
- `docs/`: Extensive documentation covering architecture, PRDs, and sub-system designs.

## Technologies
- **Language:** C++20
- **Build System:** CMake (3.20+)
- **Standard Library:** Heavy use of `<filesystem>`, `<chrono>`, and modern STL containers.
- **Data Format:** TSV for persistent storage, JSON for structured communication.

## Building and Running

### Build Commands
```powershell
# Configure with Ninja (recommended)
cmake -S . -B build -G Ninja

# Build the project
cmake --build build
```

### Running Tests
```powershell
# Run all tests using CTest
ctest --test-dir build --output-on-failure
```

### Key Execution Commands
```powershell
# Run a demo of kernel capabilities
build\agentos.exe demo

# Run a specific skill
build\agentos.exe run read_file path=README.md

# List registered agents
build\agentos.exe agents

# Check memory stats
build\agentos.exe memory stats
```

## Development Conventions

### Coding Style
- **Interface First:** Define interfaces in headers before implementing logic.
- **Modern C++:** Adhere to C++20 standards; use smart pointers and RAII.
- **Minimal Core:** Avoid bloating the core with provider-specific or high-risk logic.
- **Structured Data:** Prefer structured output (JSON/TSV) over plain text for logs and results.

### Error Handling & Logging
- **Explicit Error Codes:** Use named error codes (e.g., `PolicyDenied`, `SkillNotFound`) instead of magic numbers.
- **Redaction:** Never log raw API keys, tokens, or credentials. Use `SecretRedactor` for sensitive values.
- **Audit Trails:** All significant actions must be recorded in the `AuditLogger`.

### Testing
- **Modular Tests:** Add new tests to the `tests/` directory and register them in `CMakeLists.txt`.
- **Mocking:** Use mock adapters (e.g., `MockPlanningAgent`) for main flow testing to avoid external dependencies.
- **Integration Tests:** Use `agentos_cli_integration_tests` to verify end-to-end CLI behavior.

## Key Files & Directories
- `GEMINI.md`: This file (Project instructions).
- `README.md`: High-level overview and usage examples.
- `docs/ARCHITECTURE.md`: Detailed architectural design.
- `docs/CODING_GUIDE.md`: Specific coding standards and development tips.
- `runtime/`: Default directory for logs and persistent storage (audit logs, memory, etc.).
