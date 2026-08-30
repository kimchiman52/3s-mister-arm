#!/usr/bin/env bash
set -euo pipefail
TARGET="${1:?Usage: ./deploy.sh user@host:/opt/rendezvous-server}"
DIR="$(cd "$(dirname "$0")" && pwd)"

# The remote-side path, with any user@host: prefix stripped. The instructions
# printed at the end run ON the remote, so they must not carry rsync's
# host-qualified form (a literal "host:/opt/..." is not a usable cp source).
REMOTE_DIR="${TARGET##*:}"

# --no-owner --no-group: the deployed tree is owned by the unprivileged
# `rendezvous` service user. Plain -a maps owner/group by NAME, and the
# developer's local uid/gid (e.g. 501:20 on macOS, where gid 20 is `staff`
# locally but `dialout` on the Ubuntu host) does not exist remotely, so -a
# would silently rewrite the deployed files to a bogus 501:dialout.
#
# This suppresses that bogus mapping but does NOT fully preserve ownership:
# rsync writes a temp file and renames over the target, so any file it
# actually REPLACES ends up owned by the transferring ssh user (root), not
# by `rendezvous`. Untouched files keep their ownership. That is why the
# chown below is printed on EVERY deploy, not just a fresh install --
# it is what puts the tree back to a single known owner.
#
# No --delete: every source here is a plain file, and rsync only deletes
# within directories it actually transfers, so --delete was always a no-op --
# but it would become live and destructive the moment a directory was added
# to the source list.
rsync -avz --no-owner --no-group \
  "$DIR/rendezvous-server.js" \
  "$DIR/rendezvous-server.service" \
  "$DIR/package.json" \
  "$DIR/README.md" \
  "$TARGET/"

echo
echo "Now on the remote, run:"
echo "  sudo chown -R rendezvous:rendezvous $REMOTE_DIR"
echo "  sudo cp $REMOTE_DIR/rendezvous-server.service /etc/systemd/system/"
echo "  sudo systemctl daemon-reload"
echo "  sudo systemctl restart rendezvous-server"
echo
echo "(The chown is not optional: rsync renames over replaced files, so they"
echo " land owned by the ssh user rather than the service user. On a first"
echo " install the account must exist first:"
echo "   sudo useradd -r -s /usr/sbin/nologin rendezvous)"
