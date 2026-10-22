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
    # Folders in the data bucket holding .msb files, in the order the engine
    # looks: prices, then rates. recipes.json at the bucket's root, when there,
    # is the catalog's synthetic recipes (scripts/build_recipes.py).
    [string[]]$DataDirs = @("sharadar", "fred"),
    # Alerts (docs/specs/SPEC_SCORECARD_ALERTS.md section 5). An address and
    # RESEND_API_KEY in the user's environment turn them on; either missing
    # deploys without them.
    [string]$AlertEmail = "",
    [double]$AlertMinScore = 80,
    # The weakest verdict that may alert; "" lets any verdict through.
    [string]$AlertMinRobustness = "supported",
    # The web app's address, for the link in an alert.
    [string]$PublicUrl = ""
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

# A key from the user's environment, kept as a secret: a new version only when
# it has changed, and never echoed. Returns whether the secret exists, so the
# service can be given it; with no key anywhere the feature is left off.
function Sync-KeySecret([string]$Variable, [string]$Secret) {
    $key = [Environment]::GetEnvironmentVariable($Variable, "User")
    if (-not $key) { $key = [Environment]::GetEnvironmentVariable($Variable) }
    $have = Test-Gcloud secrets describe $Secret
    if ($key) {
        $key = $key.Trim()
        $stored = ""
        if ($have) {
            $stored = ((& $gcloud secrets versions access latest --secret $Secret 2> $null) -join "").Trim()
        }
        if ($stored -ne $key) {
            $file = New-TemporaryFile
            try {
                [IO.File]::WriteAllText($file, $key)
                if ($have) {
                    Invoke-Gcloud secrets versions add $Secret --data-file $file | Out-Host
                } else {
                    Invoke-Gcloud secrets create $Secret --data-file $file | Out-Host
                }
            } finally {
                Remove-Item $file
            }
            Write-Host "stored $Variable as secret $Secret"
        }
        $have = $true
    }
    if ($have) {
        Invoke-Gcloud secrets add-iam-policy-binding $Secret `
            --member "serviceAccount:$account" --role roles/secretmanager.secretAccessor | Out-Host
    }
    return $have
}

$secrets = "MS_API_KEY=ms-api-key:latest"
# The assistant's Anthropic key. Without it the service runs without the assistant.
if (Sync-KeySecret "ANTHROPIC_API_KEY" "anthropic-api-key") {
    $secrets += ",ANTHROPIC_API_KEY=anthropic-api-key:latest"
} else {
    Write-Host "no ANTHROPIC_API_KEY: deploying without the assistant"
}
# The email service's key, for alerts.
$alerting = $false
if (Sync-KeySecret "RESEND_API_KEY" "resend-api-key") {
    $secrets += ",RESEND_API_KEY=resend-api-key:latest"
    $alerting = [bool]$AlertEmail
}
if (-not $alerting) {
    Write-Host "alerts off: they need -AlertEmail and RESEND_API_KEY"
}
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
$environment = [ordered]@{ MS_RUNS_BUCKET = $Bucket }
if ($DataBucket) {
    Invoke-Gcloud storage buckets add-iam-policy-binding "gs://$DataBucket" `
        --member "serviceAccount:$account" --role roles/storage.objectViewer
    # Cloud Storage volumes need the second-generation execution environment.
    $dataArgs = @(
        "--execution-environment", "gen2",
        "--add-volume", "name=data,type=cloud-storage,bucket=$DataBucket,readonly=true",
        "--add-volume-mount", "volume=data,mount-path=/data"
    )
    $environment["MS_DAILY_DIR"] = ($DataDirs | ForEach-Object { "/data/$_" }) -join ","
    $environment["MS_RECIPES"] = "/data/recipes.json"
}
if ($alerting) {
    $environment["MS_ALERT_EMAIL_TO"] = $AlertEmail
    $environment["MS_ALERT_MIN_SCORE"] = "$AlertMinScore"
    $environment["MS_ALERT_MIN_ROBUSTNESS"] = $AlertMinRobustness
}
if ($PublicUrl) {
    $environment["MS_PUBLIC_URL"] = $PublicUrl
}

# The variables go in a file rather than on the command line: MS_DAILY_DIR
# holds commas, which --set-env-vars splits on, and gcloud.cmd passes its
# arguments through cmd.exe, which reads | and ^ itself.
$envFile = New-TemporaryFile
$environment.GetEnumerator() | ForEach-Object { "$($_.Key): '$($_.Value)'" } |
    Set-Content -Path $envFile -Encoding ascii

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
    --env-vars-file $envFile `
    --set-secrets $secrets `
    @dataArgs
Remove-Item $envFile

$url = (& $gcloud run services describe $Service --region $Region --format "value(status.url)").Trim()
Write-Host ""
Write-Host "deployed: $url"
Write-Host "check:    curl $url/health"
Write-Host "web/ needs RESEARCH_SERVICE_URL=$url and RESEARCH_SERVICE_KEY=<the secret>."
