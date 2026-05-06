#!/usr/bin/env bash
set -euo pipefail
TARGET="${1:?Usage: ./deploy.sh user@host:/opt/rendezvous-server}"
DIR="$(cd "$(dirname "$0")" && pwd)"
rsync -avz --delete \
  "$DIR/rendezvous-server.js" \
  "$DIR/rendezvous-server.service" \
  "$DIR/package.json" \
  "$DIR/README.md" \
  "$TARGET/"
echo "Now on the remote, run:"
echo "  sudo cp $TARGET/rendezvous-server.service /etc/systemd/system/"
echo "  sudo systemctl daemon-reload"
echo "  sudo systemctl restart rendezvous-server"
