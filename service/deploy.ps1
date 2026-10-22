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
    [string]$Bucket = "",
    # A bucket holding the daily price cache, mounted read-only into the
    # service at /data. Empty runs on the committed synthetic fixture only.
    [string]$DataBucket = "",
    [string]$DataPrefix = "sharadar"
)
# Failures are caught by exit code (Invoke-Gcloud and Test-Gcloud), not by
# $ErrorActionPreference: gcloud writes progress and notices to stderr, and
# Windows PowerShell 5.1 turns a native command's stderr into error records,
# which "Stop" would make fatal on the first informational line.
$ErrorActionPreference = "Continue"

# gcloud.cmd rather than gcloud, which resolves to gcloud.ps1 and wraps the
# same stderr in error records of its own.
$gcloud = (Get-Command gcloud.cmd -ErrorAction SilentlyContinue).Source
if (-not $gcloud) { throw "gcloud.cmd is not on PATH. Install the Google Cloud SDK and open a new terminal." }
if (-not $Bucket) { $Bucket = "$Project-marketscan-runs" }

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$repo = "marketscan"
$tag = (git -C $root rev-parse --short HEAD).Trim()
$image = "$Region-docker.pkg.dev/$Project/$repo/research:$tag"

function Invoke-Gcloud {
    & $gcloud @args
    if ($LASTEXITCODE -ne 0) { throw "gcloud $($args -join ' ') failed" }
}

# Whether a describe succeeds, i.e. the thing exists.
function Test-Gcloud {
    & $gcloud @args *> $null
    return $LASTEXITCODE -eq 0
}

Write-Host "project $Project, region $Region, image $image"
Invoke-Gcloud config set project $Project

Invoke-Gcloud services enable run.googleapis.com cloudbuild.googleapis.com `
    artifactregistry.googleapis.com storage.googleapis.com secretmanager.googleapis.com

if (-not (Test-Gcloud artifacts repositories describe $repo --location $Region)) {
    Invoke-Gcloud artifacts repositories create $repo --repository-format docker --location $Region
}

if (-not (Test-Gcloud storage buckets describe "gs://$Bucket")) {
    Invoke-Gcloud storage buckets create "gs://$Bucket" --location $Region --uniform-bucket-level-access
}

if (-not (Test-Gcloud secrets describe ms-api-key)) {
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
$number = (& $gcloud projects describe $Project --format "value(projectNumber)").Trim()
$account = "$number-compute@developer.gserviceaccount.com"
Invoke-Gcloud secrets add-iam-policy-binding ms-api-key `
    --member "serviceAccount:$account" --role roles/secretmanager.secretAccessor
Invoke-Gcloud storage buckets add-iam-policy-binding "gs://$Bucket" `
    --member "serviceAccount:$account" --role roles/storage.objectAdmin

# Cloud Build runs as the same default account on projects created since
# Google stopped granting it Editor automatically, and without this role it
# cannot read the uploaded source, write logs, or push the image.
Invoke-Gcloud projects add-iam-policy-binding $Project `
    --member "serviceAccount:$account" --role roles/cloudbuild.builds.builder --condition None

Push-Location $root
try {
    Invoke-Gcloud builds submit --config service/cloudbuild.yaml --substitutions "_IMAGE=$image" .
} finally {
    Pop-Location
}

$dataArgs = @()
if ($DataBucket) {
    Invoke-Gcloud storage buckets add-iam-policy-binding "gs://$DataBucket" `
        --member "serviceAccount:$account" --role roles/storage.objectViewer
    # Cloud Storage volumes need the second-generation execution environment.
    $dataArgs = @(
        "--execution-environment", "gen2",
        "--add-volume", "name=data,type=cloud-storage,bucket=$DataBucket,readonly=true",
        "--add-volume-mount", "volume=data,mount-path=/data"
    )
    $dataEnv = ",MS_DAILY_DIR=/data/$DataPrefix"
} else {
    $dataEnv = ""
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
    --set-env-vars "MS_RUNS_BUCKET=$Bucket$dataEnv" `
    --set-secrets "MS_API_KEY=ms-api-key:latest" `
    @dataArgs

$url = (& $gcloud run services describe $Service --region $Region --format "value(status.url)").Trim()
Write-Host ""
Write-Host "deployed: $url"
Write-Host "check:    curl $url/health"
Write-Host "web/ needs RESEARCH_SERVICE_URL=$url and RESEARCH_SERVICE_KEY=<the secret>."
