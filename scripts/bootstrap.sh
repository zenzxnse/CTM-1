#!/usr/bin/env bash

set -Eeuo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"

load_local_environment() {
  local env_file="$project_root/.env"
  local name
  local value
  [[ -f "$env_file" ]] || return 0
  while IFS='=' read -r name value || [[ -n "$name" ]]; do
    name="${name%$'\r'}"
    value="${value%$'\r'}"
    case "$name" in
      GROQ_API_KEY|GROQ_ENDPOINT|GROQ_MODEL|GROQ_MAX_OUTPUT_TOKENS|CONTEXT_HMI_INFERENCE|CONTEXT_HMI_PORT|CONTEXT_HMI_SEED_TELEMETRY|CONTEXT_HMI_STALE_AFTER_MS|CONTEXT_HMI_SKIP_BUILD|CONTEXT_HMI_OPEN_BROWSER)
        if [[ "$value" == \"*\" && "$value" == *\" ]]; then
          value="${value:1:${#value}-2}"
        elif [[ "$value" == \'*\' && "$value" == *\' ]]; then
          value="${value:1:${#value}-2}"
        fi
        if [[ ! -v "$name" ]]; then
          export "$name=$value"
        fi
        ;;
      ''|'#'*) ;;
    esac
  done < "$env_file"
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'Missing required command: %s\n' "$1" >&2
    exit 2
  fi
}

load_local_environment
require_command cmake
require_command curl
require_command node
require_command npm
require_command python3

cmake_version="$(cmake --version | awk 'NR == 1 {print $3}')"
cmake_major="${cmake_version%%.*}"
if [[ ! "$cmake_major" =~ ^[0-9]+$ ]] || (( cmake_major < 4 )); then
  printf 'CMake 4.0 or newer is required; found %s.\n' "$cmake_version" >&2
  exit 2
fi

inference_mode="${CONTEXT_HMI_INFERENCE:-rules}"
port="${CONTEXT_HMI_PORT:-8080}"
seed_telemetry="${CONTEXT_HMI_SEED_TELEMETRY:-1}"
stale_after_ms="${CONTEXT_HMI_STALE_AFTER_MS:-300000}"
skip_build="${CONTEXT_HMI_SKIP_BUILD:-0}"
open_browser="${CONTEXT_HMI_OPEN_BROWSER:-0}"

if [[ ! "$port" =~ ^[0-9]+$ ]] || (( port < 1 || port > 65535 )); then
  printf 'CONTEXT_HMI_PORT must be from 1 through 65535.\n' >&2
  exit 2
fi
if [[ "$inference_mode" != "rules" && "$inference_mode" != "groq" ]]; then
  printf 'CONTEXT_HMI_INFERENCE must be rules or groq.\n' >&2
  exit 2
fi
if [[ ! "$stale_after_ms" =~ ^[0-9]+$ ]] || (( stale_after_ms < 100 || stale_after_ms > 3600000 )); then
  printf 'CONTEXT_HMI_STALE_AFTER_MS must be from 100 through 3600000.\n' >&2
  exit 2
fi

build_preset="release"
provider_arguments=()
if [[ "$inference_mode" == "groq" ]]; then
  if [[ -z "${GROQ_API_KEY:-}" ]]; then
    printf 'Groq mode requires GROQ_API_KEY in the ignored .env file.\n' >&2
    exit 2
  fi
  groq_endpoint="${GROQ_ENDPOINT:-https://api.groq.com/openai/v1/chat/completions}"
  groq_model="${GROQ_MODEL:-openai/gpt-oss-20b}"
  groq_max_output_tokens="${GROQ_MAX_OUTPUT_TOKENS:-512}"
  if [[ "$groq_endpoint" != https://* ]]; then
    printf 'GROQ_ENDPOINT must use HTTPS.\n' >&2
    exit 2
  fi
  if [[ ! "$groq_max_output_tokens" =~ ^[0-9]+$ ]] ||
    (( groq_max_output_tokens < 64 || groq_max_output_tokens > 2048 )); then
    printf 'GROQ_MAX_OUTPUT_TOKENS must be from 64 through 2048.\n' >&2
    exit 2
  fi
  build_preset="linux-release-tls"
  provider_arguments=(
    --provider-url "$groq_endpoint"
    --provider-model "$groq_model"
    --provider-max-output-tokens "$groq_max_output_tokens"
    --provider-key-env GROQ_API_KEY
    --allow-remote-inference
  )
fi

build_dir="$project_root/build/linux-release"
if [[ "$build_preset" == "linux-release-tls" ]]; then
  build_dir="$project_root/build/linux-release-tls"
fi

if [[ "$skip_build" != "1" ]]; then
  if [[ ! -d "$project_root/workbench/node_modules" ]]; then
    npm --prefix workbench ci
  fi
  npm --prefix workbench run check
  npm --prefix workbench run build
  cmake --preset "$build_preset"
  cmake --build --preset "$build_preset" --parallel
fi

service_binary="$build_dir/context-hmi"
import_binary="$build_dir/context-hmi-context"
if [[ ! -x "$service_binary" || ! -x "$import_binary" ]]; then
  printf 'Required binaries are missing from %s. Run without CONTEXT_HMI_SKIP_BUILD=1.\n' \
    "$build_dir" >&2
  exit 2
fi

runtime_dir="$project_root/.runtime"
mkdir -p "$runtime_dir"
chmod 700 "$runtime_dir"
pid_file="$runtime_dir/context-hmi.pid"
if [[ -f "$pid_file" ]]; then
  existing_pid="$(<"$pid_file")"
  if [[ "$existing_pid" =~ ^[0-9]+$ ]] && kill -0 "$existing_pid" 2>/dev/null; then
    printf 'Context HMI is already running as process %s.\n' "$existing_pid" >&2
    exit 2
  fi
fi

import_result="$runtime_dir/machine-context.json"
"$import_binary" --bundle "$project_root/examples/artifacts" --output "$import_result"

service_stdout="$runtime_dir/service.stdout.log"
service_stderr="$runtime_dir/service.stderr.log"
"$service_binary" \
  --config "$project_root/config/context-hmi.json" \
  --host 127.0.0.1 \
  --port "$port" \
  --model "$import_result" \
  --models "$project_root/examples/machines" \
  --web-dir "$project_root/workbench/build" \
  --store-file "$runtime_dir/operations.chj" \
  --log-file "$runtime_dir/diagnostics.jsonl" \
  --telemetry-stale-after-ms "$stale_after_ms" \
  "${provider_arguments[@]}" \
  >"$service_stdout" 2>"$service_stderr" &
service_pid=$!
printf '%s\n' "$service_pid" > "$pid_file"

cleanup() {
  local exit_status=$?
  trap - EXIT INT TERM
  if kill -0 "$service_pid" 2>/dev/null; then
    kill "$service_pid" 2>/dev/null || true
    wait "$service_pid" 2>/dev/null || true
  fi
  rm -f -- "$pid_file"
  exit "$exit_status"
}
trap cleanup EXIT INT TERM

service_url="http://127.0.0.1:$port"
healthy=0
for _ in $(seq 1 200); do
  if curl --fail --silent "$service_url/api/v1/health" >/dev/null; then
    healthy=1
    break
  fi
  if ! kill -0 "$service_pid" 2>/dev/null; then
    printf 'Context HMI exited during startup.\n' >&2
    tail -n 40 "$service_stderr" >&2 || true
    exit 1
  fi
  sleep 0.05
done
if [[ "$healthy" != "1" ]]; then
  printf 'Context HMI did not become healthy at %s.\n' "$service_url" >&2
  tail -n 40 "$service_stderr" >&2 || true
  exit 1
fi

workbench_markup="$(curl --fail --silent "$service_url/")"
if [[ "$workbench_markup" != *'http-equiv="content-security-policy"'* ]] ||
  [[ "$workbench_markup" != *"sha256-"* ]]; then
  printf 'The workbench is missing its hash-based content security policy.\n' >&2
  exit 1
fi
workbench_headers="$(curl --fail --silent --dump-header - --output /dev/null "$service_url/")"
if [[ "${workbench_headers,,}" == *"script-src"* ]]; then
  printf 'The HTTP content security policy overrides the workbench script hash.\n' >&2
  exit 1
fi

if [[ "$seed_telemetry" == "1" ]]; then
  context_generation="$(curl --fail --silent "$service_url/api/v1/model" | \
    python3 -c 'import json,sys; print(json.load(sys.stdin)["context_generation"])')"
  python3 -c '
import json
import sys
import time

source_path, generation = sys.argv[1], int(sys.argv[2])
with open(source_path, encoding="utf-8") as source:
    batch = json.load(source)
batch["context_generation"] = generation
timestamp = time.time_ns() // 1_000_000
for sample in batch["values"].values():
    sample["timestamp_ms"] = timestamp
json.dump(batch, sys.stdout, separators=(",", ":"))
' "$project_root/examples/telemetry/assembly-line.json" "$context_generation" | \
    curl --fail --silent \
      --header 'Content-Type: application/json' \
      --data-binary @- \
      "$service_url/api/v1/telemetry" >/dev/null
fi

printf '\nContext HMI is ready.\n'
printf 'Workbench:  %s/\n' "$service_url"
printf 'Health:     %s/api/v1/health\n' "$service_url"
printf 'Inference:  %s\n' "$inference_mode"
printf 'Context:    %s\n' "$import_result"
printf 'Diagnostics: %s\n' "$runtime_dir/diagnostics.jsonl"
printf 'Records:    %s\n' "$runtime_dir/operations.chj"
printf 'Stop:       press Ctrl+C\n\n'

if [[ "$open_browser" == "1" ]] && command -v xdg-open >/dev/null 2>&1; then
  xdg-open "$service_url/" >/dev/null 2>&1 || true
fi

wait "$service_pid"
