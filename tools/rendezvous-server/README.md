# 3SX rendezvous server

## Overview

Single-purpose UDP endpoint-exchange service that pairs two peers behind
symmetric NATs so they can run a bilateral STUN hole punch. It does NOT do
matchmaking, presence, lobby, chat, ranking, persistence, TLS, or anything
else.

## Wire protocol

REGISTER / POLL (client to server) and DELIVER (server to client). Magic
`'3SXR'` (`0x33535852`), version 1, big-endian fields. Authoritative spec:
[`docs/plan-bilateral-hole-punch.md` Decision 2](../../docs/plan-bilateral-hole-punch.md#2-protocol-for-endpoint-exchange).

## Run locally

```
node rendezvous-server.js 3478
```

Pass `0` to bind an ephemeral port (the bound port is logged).

## Test

```
node __test_protocol.js
```

The test spins up an in-process server on an ephemeral port and drives it
with mock UDP clients. Exits 0 with `protocol test passed` on success.

## Deploy

```
./deploy.sh user@host:/opt/rendezvous-server
```

Then on the remote:

```
sudo useradd -r -s /usr/sbin/nologin rendezvous   # if it does not exist
sudo cp /opt/rendezvous-server/rendezvous-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rendezvous-server
```

## Operator notes

- Rate limit: 10 packets per source IP per second. Excess is silently dropped.
- Session TTL: 60 seconds since last REGISTER/POLL on that key.
- No persistent state. Restarting the service drops live sessions; clients
  retry transparently.
- Bind is `udp4` only; IPv6 is out of scope.
- Logs: stdout (info) and stderr (warn). Use `journalctl -u rendezvous-server`
  on the deployed host.
