param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$BillingAccount = "010A7B-134BD2-8CB391",
  [string]$Region = "us-central1",
  [string]$Zone = "us-central1-a",
  [string]$StateBucket = "boltstream-r7m5o9ld-tfstate"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$ActiveAccount = (gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing to mutate GCP. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$TerraformDir = Join-Path $RepoRoot "deployments\gcp\terraform"

Write-Host "Using account $ActiveAccount"
Write-Host "Ensuring project $ProjectId"
gcloud projects describe $ProjectId --format="value(projectId)" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
  gcloud projects create $ProjectId --name="BoltStream" --set-as-default
}

gcloud config set project $ProjectId | Out-Null
gcloud billing projects link $ProjectId --billing-account $BillingAccount | Out-Null

gcloud services enable `
  cloudbilling.googleapis.com `
  cloudresourcemanager.googleapis.com `
  compute.googleapis.com `
  iam.googleapis.com `
  secretmanager.googleapis.com `
  serviceusage.googleapis.com `
  storage.googleapis.com `
  --project $ProjectId | Out-Null

$BucketUri = "gs://$StateBucket"
gcloud storage buckets describe $BucketUri --project $ProjectId 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
  gcloud storage buckets create $BucketUri --project $ProjectId --location=US --uniform-bucket-level-access
}
gcloud storage buckets update $BucketUri --versioning --project $ProjectId | Out-Null

$Lifecycle = Join-Path $env:TEMP "boltstream-gcs-lifecycle.json"
@"
{
  "rule": [
    {
      "action": { "type": "Delete" },
      "condition": { "age": 30, "isLive": false }
    }
  ]
}
"@ | Set-Content -Path $Lifecycle -Encoding ASCII
gcloud storage buckets update $BucketUri --lifecycle-file=$Lifecycle --project $ProjectId | Out-Null

$OperatorIp = (curl.exe -fsS https://api.ipify.org).Trim()
if (-not $OperatorIp) {
  throw "Could not determine operator public IP."
}

$Tfvars = Join-Path $TerraformDir "terraform.tfvars"
@"
project_id    = "$ProjectId"
region        = "$Region"
zone          = "$Zone"
operator_cidr = "$OperatorIp/32"
"@ | Set-Content -Path $Tfvars -Encoding ASCII

Write-Host "Wrote ignored local Terraform vars to $Tfvars"
Write-Host "Operator CIDR is stored locally only and must not be committed."

Push-Location $TerraformDir
try {
  terraform init -upgrade
  terraform fmt -check
  terraform validate
} finally {
  Pop-Location
}

