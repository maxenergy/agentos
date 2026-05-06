#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Install selected skills from https://github.com/maxenergy/skills.git.

Usage:
  tools/install_maxenergy_skills.sh [--repo-scope|--user-scope] [skill ...]

Defaults:
  scope: --repo-scope
  skills: all current top-level skills in maxenergy/skills

Notes:
  - Repo scope installs into .agents/skills.
  - User scope installs into ${CODEX_HOME:-$HOME/.codex}/skills.
  - Restart Codex after installing or updating skills.
USAGE
}

scope="repo"
repo="maxenergy/skills"
ref="main"
skills=()

while (($#)); do
  case "$1" in
    --repo-scope)
      scope="repo"
      shift
      ;;
    --user-scope)
      scope="user"
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      skills+=("$1")
      shift
      ;;
  esac
done

if ((${#skills[@]} == 0)); then
  skills=(
    00-understand-system
    01-grill-requirements
    02-spec-freeze
    03-impact-analysis
    04-task-slice
    05-goal-pack
    06-codex-controller
    07-verify-loop
    08-goal-review
  )
fi

if [[ "$scope" == "repo" ]]; then
  dest=".agents/skills"
else
  dest="${CODEX_HOME:-$HOME/.codex}/skills"
fi

mkdir -p "$dest"

installer="${CODEX_HOME:-$HOME/.codex}/skills/.system/skill-installer/scripts/install-skill-from-github.py"

if [[ -f "$installer" ]]; then
  echo "Installing with Codex skill-installer into $dest"
  for skill in "${skills[@]}"; do
    if [[ -d "$dest/$skill" ]]; then
      echo "Skipping $skill; $dest/$skill already exists"
      continue
    fi
    python3 "$installer" --repo "$repo" --ref "$ref" --dest "$dest" --path "skills/$skill"
  done
else
  echo "Codex skill-installer not found at $installer; falling back to npx skills." >&2
  echo "The fallback installer may prompt for agent targets." >&2
  args=(npx skills@latest add "$repo")
  for skill in "${skills[@]}"; do
    args+=(--skill "$skill")
  done
  "${args[@]}"
fi

echo "Installed or updated requested skills in $dest"
echo "Restart Codex to pick up new skills."
