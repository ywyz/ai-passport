#!/usr/bin/env bash
# provisioning/web/deploy.sh -- upload the static provisioning page to the
# server and reload Caddy. Idempotent: running it multiple times is safe.
#
# Usage:
#   ./deploy.sh
# Overrides:
#   DEPLOY_HOST=root@example.com REMOTE_DIR=/var/www/passport ./deploy.sh
set -euo pipefail

HOST="${DEPLOY_HOST:-root@bwh.ywyz.tech}"
REMOTE_DIR="${REMOTE_DIR:-/var/www/passport}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FILES="index.html app.js style.css"

echo "==> Ensuring ${HOST}:${REMOTE_DIR}"
ssh "${HOST}" "mkdir -p '${REMOTE_DIR}'"

echo "==> Uploading files"
for f in ${FILES}; do
  scp -q "${SCRIPT_DIR}/${f}" "${HOST}:${REMOTE_DIR}/${f}"
  echo "    uploaded ${f}"
done

# index.html must not be cached by browsers; assets may be cached.
echo "==> Reloading Caddy"
if ssh "${HOST}" "command -v systemctl >/dev/null 2>&1 && systemctl list-unit-files caddy.service >/dev/null 2>&1"; then
  ssh "${HOST}" "systemctl reload caddy"
else
  ssh "${HOST}" "caddy reload --config /etc/caddy/Caddyfile"
fi

echo "==> Done. Open https://passport.ywyz.tech/"
