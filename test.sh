#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
JENV="$ROOT/jenv"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

assert_contains() {
  local haystack=$1
  local needle=$2
  if [[ "$haystack" != *"$needle"* ]]; then
    fail "expected output to contain: $needle"$'\n'"actual: $haystack"
  fi
}

assert_file_contains() {
  local file=$1
  local needle=$2
  grep -F "$needle" "$file" >/dev/null || fail "expected $file to contain: $needle"
}

assert_equals() {
  local actual=$1
  local expected=$2
  [[ "$actual" == "$expected" ]] || fail "expected [$expected], got [$actual]"
}

run_in_home() {
  local home=$1
  shift
  HOME="$home" "$@"
}

test_conda_set_and_status() {
  local tmp
  local output
  tmp=$(mktemp -d)
  mkdir -p "$tmp/project"

  output=$(cd "$tmp/project" && HOME="$tmp" CONDA_DEFAULT_ENV=test LD_LIBRARY_PATH=/opt/lib CUDA_HOME=/cuda "$JENV" set --env)
  assert_contains "$output" "Pinned directory: $tmp/project"
  assert_contains "$output" "Env type: conda"
  assert_contains "$output" "Conda env: test"

  output=$(run_in_home "$tmp" "$JENV" status)
  assert_contains "$output" "Pinned directory: $tmp/project"
  assert_contains "$output" "Env type: conda"
  assert_contains "$output" "Conda env: test"
  assert_contains "$output" "LD_LIBRARY_PATH"
  assert_contains "$output" "CUDA_HOME"
  assert_file_contains "$tmp/.config/jenv/env" "export LD_LIBRARY_PATH='/opt/lib'"
  assert_file_contains "$tmp/.config/jenv/env" "export CUDA_HOME='/cuda'"
  assert_equals "$(grep -c "# >>> jenv hook >>>" "$tmp/.bashrc")" "1"
}

test_uv_set_and_interactive_shell() {
  local tmp
  local output
  tmp=$(mktemp -d)
  mkdir -p "$tmp/work/.venv/bin" "$tmp/bin"

  cat >"$tmp/work/.venv/bin/activate" <<'EOF'
export VIRTUAL_ENV="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export JENV_TEST_UV=active
EOF

  cat >"$tmp/bin/conda" <<'EOF'
#!/bin/sh
if [ "$1" = shell.bash ] && [ "$2" = hook ]; then
  cat <<'HOOK'
conda() {
  if [ "$1" = activate ] && [ -n "$2" ]; then
    export CONDA_DEFAULT_ENV="$2"
  fi
}
HOOK
fi
EOF
  chmod +x "$tmp/bin/conda"

  output=$(cd "$tmp/work" && HOME="$tmp" LD_LIBRARY_PATH=/uv/lib CUDA_HOME=/uv/cuda PATH="$tmp/bin:$PATH" "$JENV" set --uv --env)
  assert_contains "$output" "Env type: uv"
  assert_contains "$output" "uv venv: $tmp/work/.venv"

  output=$(HOME="$tmp" PATH="$tmp/bin:$PATH" bash -ic 'pwd; printf "%s\n" "$LD_LIBRARY_PATH"; printf "%s\n" "$CUDA_HOME"; printf "%s\n" "$JENV_TEST_UV"')
  assert_contains "$output" "$tmp/work"
  assert_contains "$output" "/uv/lib"
  assert_contains "$output" "/uv/cuda"
  assert_contains "$output" "active"
}

test_env_subcommands() {
  local tmp
  local output
  tmp=$(mktemp -d)

  output=$(HOME="$tmp" WANDB_PROJECT=demo "$JENV" env add WANDB_PROJECT)
  assert_contains "$output" "Captured env var: WANDB_PROJECT"
  output=$(run_in_home "$tmp" "$JENV" env list)
  assert_contains "$output" "WANDB_PROJECT=demo"

  HOME="$tmp" WANDB_PROJECT=changed "$JENV" env add WANDB_PROJECT >/dev/null
  output=$(run_in_home "$tmp" "$JENV" env list)
  assert_contains "$output" "WANDB_PROJECT=changed"

  output=$(run_in_home "$tmp" "$JENV" env remove WANDB_PROJECT)
  assert_contains "$output" "Removed env var: WANDB_PROJECT"
  output=$(run_in_home "$tmp" "$JENV" env list)
  assert_contains "$output" "No env vars captured."

  HOME="$tmp" HF_HOME=/hf "$JENV" env add HF_HOME >/dev/null
  output=$(run_in_home "$tmp" "$JENV" env clear)
  assert_contains "$output" "Cleared captured env vars."
  output=$(run_in_home "$tmp" "$JENV" env list)
  assert_contains "$output" "No env vars captured."

  if HOME="$tmp" API_TOKEN=secret "$JENV" env add API_TOKEN >/tmp/jenv-test-stderr 2>&1; then
    fail "expected secret-looking env var add to fail"
  fi
  assert_file_contains /tmp/jenv-test-stderr "Refusing to capture secret-looking environment variable"
}

test_env_all_filters() {
  local tmp
  tmp=$(mktemp -d)
  mkdir -p "$tmp/project"

  cd "$tmp/project"
  HOME="$tmp" CONDA_DEFAULT_ENV=base LD_LIBRARY_PATH=/all/lib MY_FLAG=1 PWD=bad API_TOKEN=skip "$JENV" set --env-all >/dev/null
  assert_file_contains "$tmp/.config/jenv/env" "export LD_LIBRARY_PATH='/all/lib'"
  assert_file_contains "$tmp/.config/jenv/env" "export MY_FLAG='1'"
  if grep -F "PWD" "$tmp/.config/jenv/env" >/dev/null; then
    fail "PWD should have been filtered from --env-all"
  fi
  if grep -F "API_TOKEN" "$tmp/.config/jenv/env" >/dev/null; then
    fail "secret-looking env var should have been filtered from --env-all"
  fi
}

test_purge_and_uninstall() {
  local tmp
  tmp=$(mktemp -d)
  mkdir -p "$tmp/project"

  cd "$tmp/project"
  HOME="$tmp" CONDA_DEFAULT_ENV=test HF_HOME=/hf "$JENV" set --env >/dev/null
  HOME="$tmp" "$JENV" purge >/dev/null
  [[ ! -e "$tmp/.config/jenv/config" ]] || fail "config should be removed by purge"
  [[ ! -e "$tmp/.config/jenv/env" ]] || fail "env file should be removed by purge"
  HOME="$tmp" "$JENV" uninstall >/dev/null
  if [[ -s "$tmp/.bashrc" ]]; then
    fail ".bashrc should be empty after uninstall in this test"
  fi
}

test_conda_set_and_status
test_uv_set_and_interactive_shell
test_env_subcommands
test_env_all_filters
test_purge_and_uninstall

echo "All tests passed."
