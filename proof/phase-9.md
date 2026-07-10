# Phase 9 Proof - Metrics and Operations

Date: 2026-07-10

Status: complete for deployed runtime commit `5876fefa0719`.

## Scope

Phase 9 adds the operational surface required to configure, observe, deploy,
and diagnose BoltStream without changing the Phase 8 protocol or storage
formats.

Implemented behavior:

- Strict YAML configuration with duplicate-key, unknown-key, type, and range
  validation.
- Deterministic precedence: compiled defaults, YAML, then explicit CLI flags.
- Secret-free effective-config output; the broker token remains environment-only.
- Structured JSON logs with timestamp, level, event, component, Git SHA, typed
  fields, microsecond durations, and token redaction.
- Prometheus text-format metrics on the existing loopback-only admin listener.
- Broker, request, storage, recovery, retention, overload, consumer-group, lag,
  filesystem, and metrics-render instrumentation.
- Pinned Prometheus and Grafana images, six tested alert rules, and a provisioned
  12-panel `BoltStream Operations` dashboard.
- Checked-in Compose, GCP YAML, SSH metrics tunnel, deployment, inspection, and
  live Phase 9 smoke workflows.

## Local Verification

Formatting and diff hygiene passed:

```text
.\scripts\format.ps1 -Fix
.\scripts\format.ps1
git diff --check
```

Windows GCC tests passed after the final HTTP framing regression was added:

```text
Test project: build/windows-gcc-debug
77/77 tests passed
100% tests passed, 0 tests failed
```

The test set includes strict YAML parsing, precedence and secret-free output,
metrics counters and histograms, label escaping, concurrent updates, disabled
metrics, traffic/storage exposition, concurrent scrapes during writes, and a
fragmented admin HTTP request.

Phase 3 through Phase 9 smoke scripts passed with the Windows GCC build. The
final focused result was:

```text
Phase 9 metrics and operations smoke passed.
```

The Windows Clang and MSVC builds and tests also passed. GitHub's final MSVC job
ran both the Phase 7 and Phase 9 smoke gates.

Prometheus validation passed for:

```text
deployments/metrics/prometheus.yml
deployments/metrics/prometheus-live.yml
deployments/metrics/alerts.yml
deployments/metrics/alerts.test.yml
```

`promtool test rules` passed all six alert-rule tests, and a real BoltStream
`/metrics` response passed `promtool check metrics`.

## Compose Operations Proof

A clean `docker compose up --build` run proved the full local operations stack:

```text
one-shot topic init: exited 0
one-shot producer: exited 0 after 25 records
one-shot consumer: exited 0 after consuming and committing 10 records
boltstream build Git SHA: local-compose
Prometheus target up: 1
consumer group lag: 15
Prometheus rule groups: 1
loaded alert rules: 6
Grafana database: ok
Grafana version: 13.1.0
dashboard uid: boltstream-operations
```

The broker, Prometheus, and Grafana host ports were bound to loopback. The demo
token did not appear in broker logs. `docker compose down -v` removed the stack
and its volumes after proof.

## GitHub CI And Artifact

The runtime implementation started at:

```text
ae9f7de Implement Phase 9 metrics and operations
```

Two live-smoke portability fixes were then committed, ending at:

```text
5876fef Harden Phase 9 metrics tunnel automation
```

Final CI run `29057272798` passed for full commit
`5876fefa07190fc5bbdecf86762f2ad0c83a7791`:

```text
Linux Debug: success
Linux Release: success
Windows MSVC smoke: success
```

The exact Linux release artifact deployed to GCP was:

```text
artifact: artifacts/phase9-5876fef/boltstream-linux-x86_64-5876fefa0719.tar.gz
bytes: 1084018
sha256: FE2CD3D4C18401ADA627F99CB10FF41F3BFF7C6BE662F0B174696EA9B1AAA13E
```

## GCP Deploy And Live Proof

The existing Terraform-managed target was used:

```text
project: boltstream-r7m5o9ld
zone: us-central1-a
instance: boltstream-vm
service: boltstream.service
```

No VM address or operator CIDR is recorded in this public proof.

The final deploy installed the checked-in YAML and exact CI artifact:

```text
configuration valid: /tmp/boltstream-5876fefa0719.yaml
ExecStart=/opt/boltstream/current/bin/boltstream-server --config /etc/boltstream/boltstream.yaml
Active: active (running)
```

The deployed service reported:

```json
{"service":"boltstream","version":"0.1.0","git_sha":"5876fefa0719","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"4","storage_format_version":"2","startup_time_utc":"2026-07-09T23:38:01Z"}
```

The final Phase 9 live smoke passed against that exact SHA:

```text
Phase 9 live metrics, lag, retention, overload, log, and cleanup proof passed.
```

The smoke verified all of the following through the private SSH metrics tunnel:

- Build metadata, readiness, filesystem capacity, and filesystem availability.
- 150 produced records and the corresponding traffic counters.
- A committed consumer batch of 100 records, observable lag of 50, a second
  committed batch of 50, and final lag of zero.
- Topic deletion and removal of the deleted topic's metric series.
- Three tiny retention segments, deletion of at least two inactive segments,
  and increasing retention segment and byte counters.
- An intentionally saturated append queue, the `overloaded` request error,
  append-queue rejection metrics, and matching structured journal events.
- No broker-token disclosure in the live journal.
- Removal of temporary systemd overrides and restoration of the normal YAML
  command after the test.

The pre-existing authenticated live regression also passed on the final
artifact. It covered public broker reachability, produce/fetch, durable group
commit and resume, coordinated group split and timeout takeover, retention,
offset-out-of-range behavior, group offset reset, topic deletion, and restart
recovery. Its focused retention proof was:

```text
segments_deleted=2
bytes_deleted=162
error_code=offset_out_of_range
topic delete removed the remaining segment and committed group offset
```

Client endpoint fields are intentionally omitted from this proof.

## Prometheus And Grafana Against Live GCP

The checked-in `prometheus-live.yml` was run locally against the private SSH
tunnel. Grafana used the same provisioned datasource and dashboard files as the
Compose demo:

```text
prometheus_target: up
source: gcp-ssh-tunnel
git_sha: 5876fefa0719
grafana_database: ok
grafana_version: 13.1.0
dashboard_uid: boltstream-operations
```

The temporary Prometheus and Grafana containers, Docker network, and SSH tunnel
were removed after proof. No listener remained on the proof ports.

## Final Inspection And Cleanup

The checked-in configuration passed validation as the `boltstream` service
account and remained protected:

```text
root boltstream 640 /etc/boltstream/boltstream.yaml
root boltstream 640 /etc/boltstream/boltstream.env
configuration valid: /etc/boltstream/boltstream.yaml
admin_listen: 127.0.0.1:9100
metrics.enabled: true
logging.level: info
logging.format: json
```

Final live state:

```text
systemd: active
boltstream_ready 1
boltstream_build_info git_sha: 5876fefa0719
data filesystem: 20G, 1% used
temporary phase9-smoke systemd override: absent
```

The final Terraform detailed plan returned exit code `0`:

```text
No changes. Your infrastructure matches the configuration.
```

Phase 9 is therefore complete locally, in CI, in the packaged Linux artifact,
and on the live GCP deployment.
