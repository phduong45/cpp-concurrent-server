#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"

echo "GET /health"
curl -sS "$BASE_URL/health"
echo

echo "POST /echo"
curl -sS -X POST "$BASE_URL/echo" -d "hello"
echo

echo "GET /metrics"
curl -sS "$BASE_URL/metrics"
echo

echo "8 concurrent /slow requests"
export BASE_URL
time bash -c '
for i in $(seq 1 8); do
  curl -sS "$BASE_URL/slow" >/dev/null &
done
wait
'
