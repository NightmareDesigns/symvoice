param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "x86")]
    [string]$Architecture,

    [string]$OutputDirectory = "dist"
)

$ErrorActionPreference = "Stop"
$repository = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildRoot = Join-Path $repository $BuildDirectory
$releaseDirectory = Join-Path $buildRoot "Release"
$audioExecutable = Join-Path $releaseDirectory "symvoice-audio.exe"
$sapiLibrary = Join-Path $releaseDirectory "symvoice-sapi.dll"

foreach ($requiredFile in @($audioExecutable, $sapiLibrary)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required Windows build output is missing: $requiredFile"
    }
}

$destination = Join-Path $repository $OutputDirectory
$bundleName = "symvoice-windows-$Architecture"
$stagingDirectory = Join-Path $destination $bundleName
$zipPath = Join-Path $destination "$bundleName.zip"

New-Item -ItemType Directory -Path $destination -Force | Out-Null
if (Test-Path -LiteralPath $stagingDirectory) {
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

New-Item -ItemType Directory -Path $stagingDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repository "CMakeLists.txt") -Destination $stagingDirectory
Copy-Item -LiteralPath (Join-Path $repository "README.md") -Destination $stagingDirectory
foreach ($directory in @("python", "native", "scripts", "tests")) {
    Copy-Item -LiteralPath (Join-Path $repository $directory) `
        -Destination (Join-Path $stagingDirectory $directory) -Recurse
}

$packagedBuild = Join-Path $stagingDirectory "build\Release"
New-Item -ItemType Directory -Path $packagedBuild -Force | Out-Null
Copy-Item -LiteralPath $audioExecutable -Destination $packagedBuild
Copy-Item -LiteralPath $sapiLibrary -Destination $packagedBuild

Compress-Archive -Path $stagingDirectory -DestinationPath $zipPath -CompressionLevel Optimal
Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
Write-Host "Created PC test bundle: $zipPath"
