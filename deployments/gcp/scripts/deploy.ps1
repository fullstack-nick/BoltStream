param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm",
  [Parameter(Mandatory = $true)][string]$Artifact,
  [string]$GitSha = "",
  [string]$Config = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$Gcloud = "gcloud.cmd"
$ActiveAccount = (& $Gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing to deploy. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}

if (-not (Test-Path $Artifact)) {
  throw "Artifact not found: $Artifact"
}

if ([string]::IsNullOrWhiteSpace($Config)) {
  $Config = Join-Path $PSScriptRoot "..\boltstream.yaml"
}
$Config = (Resolve-Path $Config).Path
if (-not (Test-Path $Config)) {
  throw "GCP broker config not found: $Config"
}

if ([string]::IsNullOrWhiteSpace($GitSha)) {
  $GitSha = (git rev-parse --short=12 HEAD).Trim()
}
if ([string]::IsNullOrWhiteSpace($GitSha)) {
  throw "GitSha is required."
}

$BrokerTokenOutput = & $Gcloud secrets versions access latest --secret "boltstream-broker-token" --project $ProjectId 2>$null
$BrokerToken = ($BrokerTokenOutput -join "`n").Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($BrokerToken)) {
  throw "Secret Manager secret 'boltstream-broker-token' must have a non-empty latest version before deploy."
}

$RemoteArtifact = "/tmp/boltstream-$GitSha.tar.gz"
$RemoteScript = "/tmp/boltstream-deploy-$GitSha.sh"
$RemoteEnv = "/tmp/boltstream-$GitSha.env"
$RemoteConfig = "/tmp/boltstream-$GitSha.yaml"
$LocalScript = Join-Path $env:TEMP "boltstream-deploy-$GitSha.sh"
$LocalEnv = Join-Path $env:TEMP "boltstream-$GitSha.env"

$EnvFile = @"
BOLTSTREAM_GIT_SHA=$GitSha
BOLTSTREAM_BROKER_TOKEN=$BrokerToken
"@
[System.IO.File]::WriteAllText($LocalEnv, $EnvFile.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)

$DeployScript = @"
#!/usr/bin/env bash
set -euo pipefail

GIT_SHA="$GitSha"
ARTIFACT="$RemoteArtifact"
ENV_FILE="$RemoteEnv"
CONFIG_FILE="$RemoteConfig"
RELEASE_DIR="/opt/boltstream/releases/$GitSha"

sudo useradd --system --home /var/lib/boltstream --shell /usr/sbin/nologin boltstream 2>/dev/null || true
sudo mkdir -p /opt/boltstream/releases /etc/boltstream /var/lib/boltstream
sudo rm -rf "`$RELEASE_DIR"
sudo mkdir -p "`$RELEASE_DIR"
sudo tar -xzf "`$ARTIFACT" -C "`$RELEASE_DIR"
sudo ln -sfn "`$RELEASE_DIR" /opt/boltstream/current
sudo chown -R root:root /opt/boltstream
sudo chown -R boltstream:boltstream /var/lib/boltstream

"`$RELEASE_DIR/bin/boltstream-server" --config "`$CONFIG_FILE" --check-config

sudo install -o root -g boltstream -m 0640 "`$ENV_FILE" /etc/boltstream/boltstream.env
sudo install -o root -g boltstream -m 0640 "`$CONFIG_FILE" /etc/boltstream/boltstream.yaml
sudo rm -f "`$ENV_FILE"
sudo rm -f "`$CONFIG_FILE"

sudo tee /etc/systemd/system/boltstream.service >/dev/null <<'EOF'
[Unit]
Description=BoltStream broker
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=boltstream
Group=boltstream
WorkingDirectory=/var/lib/boltstream
EnvironmentFile=-/etc/boltstream/boltstream.env
ExecStart=/opt/boltstream/current/bin/boltstream-server --config /etc/boltstream/boltstream.yaml
Restart=always
RestartSec=2
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ReadWritePaths=/var/lib/boltstream

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable boltstream.service
sudo systemctl restart boltstream.service
sleep 2
systemctl --no-pager --full status boltstream.service
curl -fsS http://127.0.0.1:9100/version
"@
[System.IO.File]::WriteAllText($LocalScript, $DeployScript.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)

try {
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $Artifact "${InstanceName}:$RemoteArtifact"
  if ($LASTEXITCODE -ne 0) { throw "Failed to copy artifact to $InstanceName." }
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $LocalEnv "${InstanceName}:$RemoteEnv"
  if ($LASTEXITCODE -ne 0) { throw "Failed to copy broker environment to $InstanceName." }
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $Config "${InstanceName}:$RemoteConfig"
  if ($LASTEXITCODE -ne 0) { throw "Failed to copy broker config to $InstanceName." }
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $LocalScript "${InstanceName}:$RemoteScript"
  if ($LASTEXITCODE -ne 0) { throw "Failed to copy deploy script to $InstanceName." }
  & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "bash $RemoteScript"
  if ($LASTEXITCODE -ne 0) { throw "Remote deployment failed on $InstanceName." }
} finally {
  Remove-Item -Force -LiteralPath $LocalEnv, $LocalScript -ErrorAction SilentlyContinue
}
