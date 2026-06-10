param(
    [string]$ModuleName = "entry",
    [string]$TargetName = "default",
    [string]$SdkHome = "<此处填入你的DevEco Studio根目录>",
    [string]$HnpPath,
    [string]$OutPath,
    [switch]$InPlace,
    [switch]$Backup
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Resolve-Path (Join-Path $scriptDir "..")
$moduleRoot = Join-Path $projectRoot $ModuleName
$buildRoot = Join-Path $moduleRoot "build\$TargetName"
$outputsRoot = Join-Path $buildRoot "outputs\$TargetName"

if (-not $HnpPath) {
    $HnpPath = Join-Path $moduleRoot "hnp"
}

$packingTool = Join-Path $SdkHome "default\openharmony\toolchains\lib\app_packing_tool.jar"
if (-not (Test-Path -LiteralPath $packingTool)) {
    throw "app_packing_tool.jar not found: $packingTool"
}

$requiredPaths = @(
    (Join-Path $buildRoot "intermediates\stripped_native_libs\$TargetName"),
    (Join-Path $buildRoot "intermediates\package\$TargetName\module.json"),
    (Join-Path $buildRoot "intermediates\res\$TargetName\resources"),
    (Join-Path $buildRoot "intermediates\res\$TargetName\resources.index"),
    (Join-Path $outputsRoot "pack.info"),
    (Join-Path $buildRoot "intermediates\loader_out\$TargetName\ets"),
    $HnpPath
)

$pkgContextPath = Join-Path $buildRoot "intermediates\loader\$TargetName\pkgContextInfo.json"

foreach ($path in $requiredPaths) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required path not found: $path"
    }
}

if (-not (Get-ChildItem -LiteralPath $HnpPath -Recurse -Filter "*.hnp" -File | Select-Object -First 1)) {
    throw "No .hnp files found under: $HnpPath"
}

$defaultUnsigned = Join-Path $outputsRoot "$ModuleName-$TargetName-unsigned.hap"
if ($InPlace) {
    if ($Backup -and (Test-Path -LiteralPath $defaultUnsigned)) {
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $backupPath = Join-Path $outputsRoot "$ModuleName-$TargetName-unsigned.before-hnp-$stamp.hap"
        Copy-Item -LiteralPath $defaultUnsigned -Destination $backupPath
        Write-Host "Backed up unsigned HAP to: $backupPath"
    }
    $OutPath = $defaultUnsigned
} elseif (-not $OutPath) {
    $OutPath = Join-Path $outputsRoot "$ModuleName-$TargetName-hnp-unsigned.hap"
}

$args = @(
    "-Dfile.encoding=GBK",
    "-jar", $packingTool,
    "--mode", "hap",
    "--force", "true",
    "--lib-path", (Join-Path $buildRoot "intermediates\stripped_native_libs\$TargetName"),
    "--json-path", (Join-Path $buildRoot "intermediates\package\$TargetName\module.json"),
    "--resources-path", (Join-Path $buildRoot "intermediates\res\$TargetName\resources"),
    "--index-path", (Join-Path $buildRoot "intermediates\res\$TargetName\resources.index"),
    "--pack-info-path", (Join-Path $outputsRoot "pack.info"),
    "--out-path", $OutPath,
    "--ets-path", (Join-Path $buildRoot "intermediates\loader_out\$TargetName\ets")
)

if (Test-Path -LiteralPath $pkgContextPath) {
    $args += @("--pkg-context-path", $pkgContextPath)
} else {
    Write-Host "pkgContextInfo.json not found, packing without --pkg-context-path: $pkgContextPath"
}

$args += @("--hnp-path", $HnpPath)

Write-Host "Packing HAP with HNP path: $HnpPath"
& java @args

if ($LASTEXITCODE -ne 0) {
    throw "app_packing_tool failed with exit code $LASTEXITCODE"
}

$hap = Get-Item -LiteralPath $OutPath
Write-Host "Generated: $($hap.FullName)"
Write-Host "Size: $($hap.Length) bytes"

$hnpEntries = tar -tf $hap.FullName | Select-String -Pattern "hnp/.*\.hnp"
if (-not $hnpEntries) {
    throw "Generated HAP does not contain any hnp/*.hnp entry."
}

Write-Host "HNP entries:"
$hnpEntries | ForEach-Object { Write-Host "  $($_.Line)" }
