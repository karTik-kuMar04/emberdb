#!/usr/bin/env bash
# Creates random passwords for the demo stack in ./secrets (never committed).
# The directory is private to you (0700). The files are readable (0444) because the container
# user is a different uid and bind-mounted files keep their host permissions.
set -eu
cd "$(dirname "$0")/.."
mkdir -p secrets
chmod 700 secrets
for name in emberdb_password grafana_password; do
  if [ ! -e "secrets/$name" ]; then
    python3 -c 'import secrets; print(secrets.token_urlsafe(24))' > "secrets/$name"
    chmod 444 "secrets/$name"
    echo "created secrets/$name"
  fi
done
