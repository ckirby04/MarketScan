<#
.SYNOPSIS
    Builds the research service on Cloud Build and deploys it to Cloud Run.
.DESCRIPTION
    Idempotent: safe to run again after a change, and each step skips what
    already exists. Needs the gcloud CLI, logged in (`gcloud auth login`), and a
    Google Cloud project with billing enabled.

    What it sets up:
      - an Artifact Registry repository for the image
      - a Cloud Storage bucket the service keeps finished runs in
      - a Secret Manager secret holding the service's API key (generated once)
      - the Cloud Run service itself

    Cloud Run settings, and why:
      --max-instances 1      job status lives in the instance's memory while a
                             job runs; one instance means every poll reaches it
      --no-cpu-throttling    jobs run after the request that started them has
                             returned, and need the CPU then
      --timeout 3600         a long sweep is one request's worth of work at most
      --allow-unauthenticated  the service checks its own bearer key; the web
                             front end calls it from its server with that key
.EXAMPLE
    service\deploy.ps1 -Project my-project
    service\deploy.ps1 -Project my-project -Region europe-west1
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [string]$Region = "us-central1",
    [string]$Service = "marketscan-research",
    [string]$Bucket = ""
)
$ErrorActionPreference = "Stop"
if (-not $Bucket) { $Bucket = "$Project-marketscan-runs" }

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$repo = "marketscan"
$tag = (git -C $root rev-parse --short HEAD).Trim()
$image = "$Region-docker.pkg.dev/$Project/$repo/research:$tag"

function Invoke-Gcloud {
    & gcloud @args
    if ($LASTEXITCODE -ne 0) { throw "gcloud $($args -join ' ') failed" }
}

Write-Host "project $Project, region $Region, image $image"
Invoke-Gcloud config set project $Project

Invoke-Gcloud services enable run.googleapis.com cloudbuild.googleapis.com `
    artifactregistry.googleapis.com storage.googleapis.com secretmanager.googleapis.com

& gcloud artifacts repositories describe $repo --location $Region *> $null
if ($LASTEXITCODE -ne 0) {
    Invoke-Gcloud artifacts repositories create $repo --repository-format docker --location $Region
}

& gcloud storage buckets describe "gs://$Bucket" *> $null
if ($LASTEXITCODE -ne 0) {
    Invoke-Gcloud storage buckets create "gs://$Bucket" --location $Region --uniform-bucket-level-access
}

& gcloud secrets describe ms-api-key *> $null
if ($LASTEXITCODE -ne 0) {
    $bytes = New-Object byte[] 32
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $key = [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
    $file = New-TemporaryFile
    try {
        [IO.File]::WriteAllText($file, $key)
        Invoke-Gcloud secrets create ms-api-key --data-file $file
    } finally {
        Remove-Item $file
    }
    Write-Host "created secret ms-api-key. Read it with: gcloud secrets versions access latest --secret ms-api-key"
}

# The runtime service account reads the secret and writes the bucket.
$number = (gcloud projects describe $Project --format "value(projectNumber)").Trim()
$account = "$number-compute@developer.gserviceaccount.com"
Invoke-Gcloud secrets add-iam-policy-binding ms-api-key `
    --member "serviceAccount:$account" --role roles/secretmanager.secretAccessor
Invoke-Gcloud storage buckets add-iam-policy-binding "gs://$Bucket" `
    --member "serviceAccount:$account" --role roles/storage.objectAdmin

Push-Location $root
try {
    Invoke-Gcloud builds submit --config service/cloudbuild.yaml --substitutions "_IMAGE=$image" .
} finally {
    Pop-Location
}

Invoke-Gcloud run deploy $Service `
    --image $image `
    --region $Region `
    --allow-unauthenticated `
    --max-instances 1 `
    --min-instances 0 `
    --no-cpu-throttling `
    --cpu 2 `
    --memory 2Gi `
    --timeout 3600 `
    --set-env-vars "MS_RUNS_BUCKET=$Bucket" `
    --set-secrets "MS_API_KEY=ms-api-key:latest"

$url = (gcloud run services describe $Service --region $Region --format "value(status.url)").Trim()
Write-Host ""
Write-Host "deployed: $url"
Write-Host "check:    curl $url/health"
Write-Host "web/ needs RESEARCH_SERVICE_URL=$url and RESEARCH_SERVICE_KEY=<the secret>."
