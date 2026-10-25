#!/usr/bin/env bash
# End-to-end test of a built emberdb image: health, authentication, metrics, running as a
# non-root user, persistence across a restart and a hard kill, and a prompt graceful stop.
# Usage: scripts/smoke-test.sh IMAGE
set -euo pipefail

IMAGE="${1:?usage: scripts/smoke-test.sh IMAGE}"
PASSWORD="smoke-test-password-12345"
NAME="emberdb-smoke-$$"
VOLUME="$NAME-data"
WORK="$(mktemp -d)"
REDIS_IMAGE="redis:7.4-alpine"
BUSYBOX_IMAGE="busybox:1.37"
FAILURES=0

cleanup() {
  docker rm -f "$NAME" >/dev/null 2>&1 || true
  docker volume rm -f "$VOLUME" >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

pass() { printf '  ok    %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; FAILURES=$((FAILURES + 1)); }
check_eq() {  # description expected actual
  if [[ "$3" == "$2" ]]; then pass "$1"; else fail "$1 (wanted '$2', got '$3')"; fi
}
check_has() {  # description expected-substring actual
  if [[ "$3" == *"$2"* ]]; then pass "$1"; else fail "$1 (wanted '$2' in '$3')"; fi
}

# These run inside the container's network namespace, so no ports need publishing.
cli() { docker run --rm --network "container:$NAME" "$REDIS_IMAGE" \
          redis-cli -p 6380 --no-auth-warning -a "$PASSWORD" "$@"; }
cli_noauth() { docker run --rm --network "container:$NAME" "$REDIS_IMAGE" redis-cli -p 6380 "$@"; }
http_get() { docker run --rm --network "container:$NAME" "$BUSYBOX_IMAGE" wget -qO- "$1"; }

wait_healthy() {
  local status="unknown"
  for _ in $(seq 1 60); do
    status="$(docker inspect -f '{{.State.Health.Status}}' "$NAME" 2>/dev/null || echo missing)"
    if [[ "$status" == "healthy" ]]; then
      return 0
    fi
    sleep 1
  done
  echo "the container never became healthy (last status: $status)"
  docker logs "$NAME" 2>&1 | tail -20
  return 1
}

echo "image: $IMAGE"

echo "without a password:"
set +e
refusal="$(docker run --rm "$IMAGE" 2>&1)"
refusal_code=$?
set -e
check_eq "the image refuses to start open" "1" "$refusal_code"
check_has "and says why" "refusing to start" "$refusal"

echo "with a password:"
printf '%s\n' "$PASSWORD" > "$WORK/pw"
chmod 444 "$WORK/pw"
docker volume create "$VOLUME" >/dev/null
docker run -d --name "$NAME" -v "$WORK/pw:/run/secrets/pw:ro" -v "$VOLUME:/data" \
  "$IMAGE" --requirepass-file /run/secrets/pw >/dev/null
wait_healthy
pass "the container is healthy"

check_eq "PING with the password" "PONG" "$(cli PING)"
check_has "PING without it is refused" "NOAUTH" "$(cli_noauth PING)"
check_eq "SET" "OK" "$(cli SET name ember)"
check_eq "GET" "ember" "$(cli GET name)"
check_eq "INCR" "2" "$( cli INCR visits >/dev/null; cli INCR visits )"
check_eq "runs as uid 10001" "10001" "$(docker exec "$NAME" id -u)"

metrics="$(http_get http://127.0.0.1:9390/metrics)"
check_has "metrics count the SET" 'emberdb_commands_processed_total{command="SET"} 1' "$metrics"
check_has "metrics show authentication is on" "emberdb_auth_required 1" "$metrics"
check_has "/readyz answers" "ready" "$(http_get http://127.0.0.1:9390/readyz)"

echo "persistence:"
docker restart -t 10 "$NAME" >/dev/null
wait_healthy
check_eq "data survives a graceful restart" "ember" "$(cli GET name)"
check_eq "the counter survives too" "2" "$(cli GET visits)"

sleep 2  # the default fsync policy writes to disk once a second
docker kill "$NAME" >/dev/null
docker start "$NAME" >/dev/null
wait_healthy
check_eq "data survives SIGKILL" "ember" "$(cli GET name)"

echo "shutdown:"
started=$SECONDS
docker stop -t 10 "$NAME" >/dev/null
elapsed=$((SECONDS - started))
check_eq "docker stop ends with exit code 0" "0" "$(docker inspect -f '{{.State.ExitCode}}' "$NAME")"
if (( elapsed < 5 )); then pass "and quickly (${elapsed}s)"; else fail "docker stop took ${elapsed}s"; fi

if (( FAILURES > 0 )); then
  echo "$FAILURES check(s) failed"
  exit 1
fi
echo "all checks passed"
