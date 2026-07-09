# Operations

## Local Native Commands

```powershell
.\scripts\toolchain-check.ps1
.\scripts\build.ps1 -Preset windows-gcc-debug
.\scripts\test.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase9.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase8.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase7.ps1 -Preset windows-gcc-debug
.\scripts\format.ps1
```

Use `windows-msvc-debug` for the MSVC path and `windows-clang-debug` for the LLVM Clang path.

## Server Runtime

```powershell
.\build\windows-gcc-debug\boltstream-server.exe `
  --config .\config\boltstream.example.yaml `
  --listen 127.0.0.1:9000 `
  --admin-listen 127.0.0.1:9100 `
  --data .\data
```

Configuration precedence is compiled defaults, YAML, then explicitly supplied CLI
flags. Validate before starting or deploying:

```powershell
.\build\windows-gcc-debug\boltstream-server.exe `
  --config .\config\boltstream.example.yaml --check-config

.\build\windows-gcc-debug\boltstream-server.exe `
  --config .\config\boltstream.example.yaml `
  --print-effective-config --port 9900
```

Unknown, duplicate, malformed, and out-of-range YAML values fail with exit code `2`
before a socket is bound or the data directory is touched. The broker token remains
environment-only and effective output contains only `auth_required: true|false`.

Admin endpoints:

- `GET /health/live`
- `GET /health/ready`
- `GET /version`
- `GET /metrics`

Broker protocol smoke:

```powershell
.\build\windows-gcc-debug\boltstream-producer.exe `
  --host 127.0.0.1 --port 9000 `
  --topic trades --key AAPL --message "AAPL,100,192.41"

.\build\windows-gcc-debug\boltstream-consumer.exe `
  --host 127.0.0.1 --port 9000 `
  --topic trades --from beginning

.\build\windows-gcc-debug\boltstream-consumer.exe `
  --host 127.0.0.1 --port 9000 `
  --topic trades --group dashboard --commit --coordinated

.\build\windows-gcc-debug\boltstream-admin.exe topics describe `
  --host 127.0.0.1 --port 9000 `
  --topic trades

.\build\windows-gcc-debug\boltstream-admin.exe retention run `
  --host 127.0.0.1 --port 9000 `
  --topic trades

.\build\windows-gcc-debug\boltstream-admin.exe groups describe `
  --host 127.0.0.1 --port 9000 `
  --group dashboard --topic trades

.\build\windows-gcc-debug\boltstream-logtool.exe append `
  --data .\data `
  --topic trades `
  --key AAPL `
  --message "AAPL,100,192.41"

.\build\windows-gcc-debug\boltstream-logtool.exe read `
  --data .\data `
  --topic trades `
  --from 0 `
  --max-records 10
```

Producer and consumer should return exit code `0` with structured JSON. If
`BOLTSTREAM_BROKER_TOKEN` is set on the broker, pass `--token` or set the same
environment variable for the CLI before calling produce/fetch.

Repeatable storage smoke:

```powershell
.\scripts\smoke-phase6.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase7.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase8.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase4.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase3.ps1 -Preset windows-gcc-debug
```

Backpressure behavior:

- Append overload returns protocol error `overloaded`, `"retryable": true`, and CLI exit code `5`.
- `--max-append-queue-depth 0` rejects produces without appending records.
- `--max-long-poll-waiters 0` rejects waiting fetches while immediate fetches still work.
- Broker runtime logs are JSON lines with event names, correlation ids, retryable flags, queue/waiter state, and request duration.

Coordinated consumer group behavior:

- `boltstream-consumer --coordinated --group GROUP --commit` joins one topic-scoped group coordinator.
- Members heartbeat with broker-assigned member ids and generation ids.
- The broker assigns contiguous partition ranges across sorted member ids and rebalances on join, leave, and session timeout.
- Coordinated offset commits are fenced by member id, generation id, and partition ownership.
- Runtime logs include `group_member_joined`, `group_rebalanced`, `group_member_expired`, `group_heartbeat`, `group_member_left`, `group_offset_committed`, and `group_offset_commit_rejected`.

Retention and lifecycle behavior:

- Retention deletes only inactive complete segments and preserves the active segment.
- `beginning` fetches from the partition low watermark after retention.
- Explicit or committed offsets below the low watermark return `offset_out_of_range`.
- Topic deletion removes topic manifests, partition logs, indexes, and inactive group offsets.
- Topic deletion and group offset reset reject active coordinated groups with `group_active`.

## Metrics and PromQL

The admin listener emits Prometheus text format `0.0.4`. On GCP it remains private
on VM loopback; use the SSH tunnel documented below instead of opening a firewall
rule.

```powershell
curl.exe -fsS http://127.0.0.1:9100/metrics
```

Operational queries used by the checked-in dashboard:

```promql
rate(boltstream_records_produced_total[1m])
rate(boltstream_records_fetched_total[1m])
histogram_quantile(0.95, sum by (le, operation) (rate(boltstream_request_duration_seconds_bucket[5m])))
sum by (operation, error_code) (rate(boltstream_request_errors_total[5m]))
max by (topic, partition) (boltstream_partition_append_queue_depth)
max by (group, topic, partition) (boltstream_consumer_group_lag_records)
sum by (topic) (boltstream_partition_log_bytes)
boltstream_storage_available_bytes / boltstream_storage_capacity_bytes
increase(boltstream_retention_failures_total[5m])
```

Metric labels never contain tokens, record keys/values, correlation ids, member ids,
client endpoints, or free-form errors. Dynamic `topic`, `partition`, and `group`
labels exist only on current resource gauges and disappear after resource deletion.
Counters reset after a broker restart; use `rate` or `increase` so Prometheus handles
the reset.

## Compose Operations Demo

```powershell
docker compose config --quiet
docker compose up --build -d
docker compose ps -a
curl.exe -fsS "http://127.0.0.1:9090/api/v1/query?query=up%7Bjob%3D%22boltstream%22%7D"
curl.exe -fsS "http://127.0.0.1:9090/api/v1/query?query=boltstream_consumer_group_lag_records"
curl.exe -fsS http://127.0.0.1:3000/api/health
curl.exe -fsS "http://127.0.0.1:3000/api/search?query=BoltStream"
```

Prometheus is available at `http://127.0.0.1:9090`; the provisioned Grafana
dashboard is at `http://127.0.0.1:3000/d/boltstream-operations/boltstream-operations`.
The one-shot demo producer writes 25 records and the consumer commits 10, leaving 15
records of visible `dashboard` group lag. Catch up by running:

```powershell
docker compose run --rm consumer
```

Validate checked-in configuration and alert behavior:

```powershell
docker run --rm --entrypoint /bin/promtool `
  -v "${PWD}\deployments\metrics:/etc/prometheus:ro" `
  prom/prometheus@sha256:c6b27ea434f8389bfe233fbc7be381cf50587c286e871bc842008f5a1b1908a7 `
  check config /etc/prometheus/prometheus.yml

docker run --rm --entrypoint /bin/promtool `
  -v "${PWD}\deployments\metrics:/etc/prometheus:ro" `
  -w /etc/prometheus `
  prom/prometheus@sha256:c6b27ea434f8389bfe233fbc7be381cf50587c286e871bc842008f5a1b1908a7 `
  test rules alerts.test.yml
```

Stop the local stack with `docker compose down`. Add `-v` only when the demo broker,
Prometheus, and Grafana volumes should also be erased.

## Incident Runbooks

Append overload:

1. Query `boltstream_rejected_requests_total{operation="produce",reason="append_queue"}`.
2. Compare each queue depth with `boltstream_partition_append_queue_capacity`.
3. Check produce p95/p99 duration and `append_overloaded` journal events.
4. Reduce producer concurrency or batch pressure before increasing queue limits;
   larger limits consume memory and delay overload feedback.

Slow consumer:

1. Find the highest `boltstream_consumer_group_lag_records` series.
2. Check `boltstream_consumer_group_members` and generation/rebalance counters.
3. Inspect `group_member_expired`, `group_rebalanced`, and commit rejection events.
4. Restore the consumer, let it catch up, and verify lag returns to zero.

Disk growth or low free space:

1. Compare partition log bytes and segment counts by topic.
2. Check the filesystem available/capacity ratio and the retention failure counter.
3. Run `boltstream-admin retention run`, inspect its deleted bytes/segments, and
   verify only inactive segments were removed.
4. Delete disposable topics through `boltstream-admin`; never remove active segment
   files manually.

Not ready or recovery failure:

1. Compare `/health/live` and `/health/ready`; live `200` plus ready `503` isolates a
   startup/storage problem.
2. Inspect `storage_recovery` and error events from the current systemd activation.
3. Verify config, `/var/lib/boltstream` ownership, mount state, free space, and the
   current release symlink.
4. Restart only after correcting the cause, then verify readiness, recovery metrics,
   live produce/fetch, and the deployed SHA.

## Linux Service Layout

On GCP, deployment uses:

- Service user: `boltstream`
- Unit: `boltstream.service`
- Binary symlink: `/opt/boltstream/current`
- Release path: `/opt/boltstream/releases/<git-sha>`
- Data path: `/var/lib/boltstream`
- Config/env path: `/etc/boltstream`

Useful inspection commands:

```bash
systemctl --no-pager --full status boltstream.service
journalctl -u boltstream.service -n 80 --no-pager
curl -fsS http://127.0.0.1:9100/version
curl -fsS http://127.0.0.1:9100/metrics
/opt/boltstream/current/bin/boltstream-server --config /etc/boltstream/boltstream.yaml --check-config
stat -c '%U %G %a %n' /etc/boltstream/boltstream.yaml /etc/boltstream/boltstream.env
df -h /var/lib/boltstream
find /var/lib/boltstream/topics -maxdepth 4 -type f -print 2>/dev/null
readlink -f /opt/boltstream/current
```

For live metrics on the operator machine:

```powershell
.\deployments\gcp\scripts\metrics-tunnel.ps1
curl.exe -fsS http://127.0.0.1:19100/metrics
```

To point the local Prometheus container at the live tunnel, replace the mounted
Prometheus config with `deployments/metrics/prometheus-live.yml` and keep the tunnel
process active. No Prometheus or Grafana process is installed on the `e2-micro` VM.

## Phase Gate Rule

Local success is necessary but not sufficient. A phase is complete only when the pushed commit is built by CI, deployed to GCP, live-called, SSH-inspected, and recorded in the phase proof file.
