# GCP Deployment

BoltStream's GCP deployment is intentionally low-level and controlled: Terraform provisions infrastructure, SSH deploys the exact CI artifact, and systemd runs the broker.

## Phase 10 Live Benchmark

Phase 10 uses the existing VM and ports. The benchmark runner temporarily replaces
the service command with a loopback-only checked-in profile, stores data under an
isolated `/var/lib/boltstream/phase10-*` directory, runs the packaged benchmark
client on the VM, and restores the normal service in a trap. It does not create a
second VM or open a benchmark port.

After the exact Release artifact is deployed and `/version` reports its 12-character
SHA, run:

```powershell
.\deployments\gcp\scripts\benchmark-phase10-live.ps1 `
  -GitSha <deployed-sha> `
  -OutputDir .\artifacts\benchmarks\gcp
```

The script fails closed unless the active Google account is
`nickaccturk@gmail.com`, rotates profile order across five rounds, downloads every
raw JSON result, removes benchmark data and overrides, then requires the normal
service to be active, ready, and still report the deployed SHA. Publish only after
`terraform plan -detailed-exitcode` returns zero and the full authenticated live
regression passes.

If the operator process is interrupted, repeat the same command with `-Resume`.
Resume keeps only complete three-workload profile results whose JSON is zero-error
and matches the requested SHA/profile/workload. A partial profile is rerun, while
completed samples are never overwritten.

For fetch throughput, the 250,000-record preload is untimed setup. The runner creates
the disposable topic through the authenticated broker, stops only the isolated
benchmark service, uses `boltstream-bench prepare-fetch` to append deterministic
records in storage batches of 1,024, and restarts the exact target profile. Four
authenticated partition-specific consumers then time and verify the complete read.
Raw JSON labels the setup method and batch size; direct storage preparation is never
reported as broker produce throughput.

## Locked Target

- GCP account: `nickaccturk@gmail.com`
- Project: `boltstream-r7m5o9ld`
- Billing account: `010A7B-134BD2-8CB391`
- Region: `us-central1`
- Zone: `us-central1-a`
- VM: `e2-micro`
- Boot disk: 10 GB standard persistent disk
- Data disk: 20 GB standard persistent disk mounted at `/var/lib/boltstream`
- Broker port: `9000`, restricted to the operator `/32`
- Admin port: `9100`, bound to localhost only
- Broker auth token: latest version of Secret Manager secret `boltstream-broker-token`

The operator source IP is written only to ignored local Terraform variables. Do not commit concrete personal IP addresses to public docs.

## References To Re-Check Before Mutating GCP

- [Google Cloud Free Program](https://docs.cloud.google.com/free/docs/free-cloud-features)
- [Compute Engine Terraform docs](https://docs.cloud.google.com/compute/docs/terraform)
- [Terraform GCS backend](https://developer.hashicorp.com/terraform/language/backend/gcs)

## Bootstrap

```powershell
.\deployments\gcp\scripts\bootstrap.ps1
```

Bootstrap performs these guarded steps:

- Verifies the active account is `nickaccturk@gmail.com`.
- Creates/selects project `boltstream-r7m5o9ld`.
- Links billing account `010A7B-134BD2-8CB391`.
- Enables required APIs.
- Creates the GCS Terraform state bucket.
- Detects the operator public IP and writes it to ignored `terraform.tfvars`.
- Runs `terraform init`, `terraform fmt -check`, and `terraform validate`.

## Provision

```powershell
Push-Location .\deployments\gcp\terraform
terraform plan
terraform apply
terraform plan -detailed-exitcode
Pop-Location
```

The final `terraform plan -detailed-exitcode` must report no changes before Phase 1 is considered stable.

## Deploy

Download or build the CI Linux artifact, then deploy it by exact Git SHA:

```powershell
.\deployments\gcp\scripts\deploy.ps1 `
  -Artifact .\artifacts\boltstream-linux-x86_64-<sha>.tar.gz `
  -GitSha <sha>
```

The deploy script installs to `/opt/boltstream/releases/<git-sha>`, validates the
checked-in `deployments/gcp/boltstream.yaml` with that exact release binary, installs
it as root-owned `/etc/boltstream/boltstream.yaml` mode `0640`, updates
`/opt/boltstream/current`, writes a config-driven systemd unit, restarts
`boltstream.service`, and checks `/version` on the VM.
From Phase 4 onward, deploy also reads the latest `boltstream-broker-token` Secret Manager version and writes it to `/etc/boltstream/boltstream.env`; deploy fails if that secret payload is missing.

## Live Smoke

```powershell
.\deployments\gcp\scripts\smoke-live.ps1 -ExpectedGitSha <sha>
.\deployments\gcp\scripts\smoke-phase9-live.ps1 -ExpectedGitSha <sha>
.\deployments\gcp\scripts\inspect-live.ps1
```

Live proof must verify:

- TCP broker port `9000` is reachable from the operator machine.
- Authenticated producer and consumer calls succeed against the live broker.
- `/health/live`, `/health/ready`, and `/version` succeed over SSH on localhost.
- `/metrics` succeeds through a guarded SSH tunnel and reports the deployed Git SHA,
  readiness, traffic, latency, storage, consumer lag, retention, and filesystem state.
- `/version` reports the deployed Git SHA.
- `systemctl status boltstream` is active.
- `journalctl -u boltstream` shows clean startup and no errors after live calls.
- `/var/lib/boltstream` is mounted and writable by the service user, with topic segment/index files present after live produce.

## Private Metrics Tunnel

The admin and metrics port is never opened publicly. Start the guarded tunnel from a
second operator terminal:

```powershell
.\deployments\gcp\scripts\metrics-tunnel.ps1
```

Then inspect the live service locally:

```powershell
curl.exe -fsS http://127.0.0.1:19100/health/ready
curl.exe -fsS http://127.0.0.1:19100/version
curl.exe -fsS http://127.0.0.1:19100/metrics
```

The helper fails closed unless the active account is `nickaccturk@gmail.com` and the
active project is `boltstream-r7m5o9ld`. The local Prometheus/Grafana stack can scrape
the same tunnel with `deployments/metrics/prometheus-live.yml`; the VM remains a lean
broker-only `e2-micro` deployment.
