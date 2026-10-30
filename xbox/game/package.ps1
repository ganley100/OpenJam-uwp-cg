# Pack and sign the game as an Xbox One app. Run from a VS developer prompt,
# which puts makeappx and signtool on PATH and sets VCToolsRedistDir.
#
#   package.ps1 -BuildDir <folder with nbajam_ofe.exe> -Out <folder> [-Version 1.0.0.0]
#
# The package is only the program: the executable, the ReXGlue DLLs built
# beside it and the achievement data. The C++ runtime is the app one, from
# the Microsoft.VCLibs framework package, which is put beside the .appx to
# install with it. The game itself is
# unpacked into the app's storage on first launch (src/uwp_platform.cpp).

param(
    [Parameter(Mandatory = $true)] [string]$BuildDir,
    [Parameter(Mandatory = $true)] [string]$Out,
    [string]$Version = "1.0.0.0",
    # Which app: the base game, or the mod it plays. Each installs as its
    # own app, with its own storage and saves.
    [ValidateSet("base", "2020s", "Legends")]
    [string]$Variant = "base",
    # The smoke test, the manifest's second tile.
    [Parameter(Mandatory = $true)] [string]$Diagnostics
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)
New-Item -ItemType Directory -Force $Out | Out-Null
$layout = Join-Path $Out "layout"
if (Test-Path $layout) { Remove-Item $layout -Recurse -Force }
New-Item -ItemType Directory -Force $layout | Out-Null

Get-ChildItem $BuildDir -File | Where-Object { $_.Extension -in ".exe", ".dll" } |
    Copy-Item -Destination $layout
Copy-Item $Diagnostics (Join-Path $layout "smoketest.exe")

# Never the desktop C++ runtime: inside a package it stops the app from
# starting on the console. Built against the app runtime, nothing to copy.

# Runtime settings, read from beside the executable.
Copy-Item (Join-Path $here "nbajam_ofe.toml") $layout

$meta = Join-Path $repo "metadata"
if (Test-Path $meta) { Copy-Item -Recurse $meta (Join-Path $layout "metadata") }
Copy-Item -Recurse (Join-Path $repo "packaging\xbox\Assets") (Join-Path $layout "Assets")
# The identity is how the app knows which it is (src/uwp_platform.cpp).
$v = @{
    "base"    = @{ Identity = "OpenJam.NBAJamOFE";         Name = "NBA JAM: On Fire Edition";        Short = "NBA JAM";         File = "NBAJamOFE" }
    "2020s"   = @{ Identity = "OpenJam.NBAJamOFE.2020s";   Name = "NBA JAM 2020s On Fire Edition";   Short = "NBA JAM 2020s";   File = "NBAJamOFE-2020s" }
    "Legends" = @{ Identity = "OpenJam.NBAJamOFE.Legends"; Name = "NBA JAM Legends On Fire Edition"; Short = "NBA JAM Legends"; File = "NBAJamOFE-Legends" }
}[$Variant]
(Get-Content (Join-Path $here "AppxManifest.xml") -Raw) `
    -replace "@VERSION@", $Version `
    -replace "@IDENTITY@", $v.Identity `
    -replace "@DISPLAY_NAME@", $v.Name `
    -replace "@SHORT_NAME@", $v.Short |
    Set-Content (Join-Path $layout "AppxManifest.xml") -Encoding UTF8

$appx = Join-Path $Out ("{0}_{1}_x64.appx" -f $v.File, $Version)
& makeappx pack /o /d $layout /p $appx
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)" }

# Developer Mode installs any signed package; the certificate's subject only
# has to be the manifest's publisher.
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=OpenJam" `
    -KeyUsage DigitalSignature -FriendlyName "OpenJam Xbox packages" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
$pfx = Join-Path $env:RUNNER_TEMP "openjam-game-$Variant.pfx"
$pw = ConvertTo-SecureString -String "openjam" -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pw | Out-Null
& signtool sign /fd SHA256 /f $pfx /p openjam $appx
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }

# The app C++ runtime the package depends on, from the same toolset that
# built it, to install alongside.
$vclibs = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs" `
    -Recurse -Filter "Microsoft.VCLibs.x64.14.00.appx" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\Retail\\" } | Select-Object -First 1
if ($vclibs) {
    Copy-Item $vclibs.FullName $Out
} else {
    Write-Warning "Microsoft.VCLibs.x64.14.00.appx not found on this machine"
}

Write-Host "Package contents:"
Get-ChildItem $layout -Recurse -File | ForEach-Object { "  {0,12:N0}  {1}" -f $_.Length, $_.FullName.Substring($layout.Length + 1) }
Remove-Item $layout -Recurse -Force
Get-ChildItem $Out | Select-Object Name, Length
