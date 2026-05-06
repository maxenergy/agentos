# Repository Guidelines

## Project Structure & Module Organization

AgentOS is a C++20/CMake project. Core implementation lives under `src/`, grouped by responsibility: `core/` for routing, policy, loop, orchestration, and registry logic; `hosts/` for CLI, plugin, and agent adapters; `skills/` for built-in skills; `auth/`, `trust/`, `memory/`, `scheduler/`, and `storage/` for their named subsystems. Tests are in `tests/` and are registered from `CMakeLists.txt`. Runtime specs and generated local state live under `runtime/`; documentation is in `docs/`; examples and helper tools are in `examples/` and `tools/`.

## Build, Test, and Development Commands

Use an out-of-tree build directory:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows without Ninja, CMake may generate Visual Studio projects; build with `cmake --build build --config Release`. Run the CLI with `build\agentos.exe` or `build\agentos.exe interactive`. Start the REST server with `build\agentos.exe serve port=18080 host=127.0.0.1`.

## Coding Style & Naming Conventions

Follow the existing C++ style: 4-space indentation, braces on control blocks, `snake_case` for functions and variables, `PascalCase` for types, and clear subsystem-oriented filenames such as `permission_model.cpp`. Keep headers paired with implementations where practical. Prefer standard library facilities and existing local helpers before adding dependencies. Avoid committing generated `build/`, `out/`, or `dist/` directories.

## Testing Guidelines

Tests use CTest with small executable targets declared in `CMakeLists.txt`. Add new tests under `tests/` using the existing `*_tests.cpp` naming pattern. Keep coverage focused on the changed subsystem and include policy, routing, storage, or CLI integration cases when behavior crosses boundaries. Always run `ctest --test-dir build --output-on-failure` before opening a PR.

## Commit & Pull Request Guidelines

Recent history uses Conventional Commit style, for example `feat(main-agent): ...` and `fix(repl): ...`. Keep commits scoped and imperative. Pull requests should include a concise summary, affected subsystems, test results, linked issues when relevant, and CLI/API examples for user-visible behavior changes. Update `README.md`, `docs/`, or `plan.md` when capabilities or workflows change.

## Security & Configuration Tips

Do not commit credentials, OAuth tokens, runtime auth profiles, or local audit/state files. API key profiles should reference environment variables rather than storing secrets directly. Treat changes to trust, approval, sandbox, and permission code as high-risk and back them with targeted tests.
