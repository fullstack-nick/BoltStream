# GCP Deployment

BoltStream's GCP deployment is intentionally low-level and controlled: Terraform provisions infrastructure, SSH deploys the exact CI artifact, and systemd runs the broker.

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

The deploy script installs to `/opt/boltstream/releases/<git-sha>`, updates `/opt/boltstream/current`, writes a systemd unit, restarts `boltstream.service`, and checks `/version` on the VM.

## Live Smoke

```powershell
.\deployments\gcp\scripts\smoke-live.ps1 -ExpectedGitSha <sha>
.\deployments\gcp\scripts\inspect-live.ps1
```

Live proof must verify:

- TCP broker port `9000` is reachable from the operator machine.
- `/health/live`, `/health/ready`, and `/version` succeed over SSH on localhost.
- `/version` reports the deployed Git SHA.
- `systemctl status boltstream` is active.
- `journalctl -u boltstream` shows clean startup and no errors after live calls.
- `/var/lib/boltstream` is mounted and writable by the service user.

