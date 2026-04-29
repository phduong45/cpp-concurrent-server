#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
PATH_TO_TEST="${1:-/health}"
TOTAL_REQUESTS="${2:-100}"
CONCURRENCY="${3:-10}"

if ! [[ "$TOTAL_REQUESTS" =~ ^[0-9]+$ ]] || ! [[ "$CONCURRENCY" =~ ^[0-9]+$ ]]; then
  echo "usage: $0 [path] [total_requests] [concurrency]" >&2
  exit 1
fi

if (( TOTAL_REQUESTS < 1 || CONCURRENCY < 1 )); then
  echo "total_requests and concurrency must be positive" >&2
  exit 1
fi

run_one() {
  curl -sS -o /dev/null "$BASE_URL$PATH_TO_TEST"
}

start_ns=$(date +%s%N)
running=0

for i in $(seq 1 "$TOTAL_REQUESTS"); do
  run_one &
  running=$((running + 1))

  if (( running >= CONCURRENCY )); then
    wait -n
    running=$((running - 1))
  fi
done

wait
end_ns=$(date +%s%N)

elapsed_ns=$((end_ns - start_ns))
elapsed_ms=$((elapsed_ns / 1000000))

if (( elapsed_ms == 0 )); then
  rps="n/a"
else
  rps=$((TOTAL_REQUESTS * 1000 / elapsed_ms))
fi

echo "path: $PATH_TO_TEST"
echo "total_requests: $TOTAL_REQUESTS"
echo "concurrency: $CONCURRENCY"
echo "elapsed_ms: $elapsed_ms"
echo "requests_per_second: $rps"
