# Pack and sign the game as an Xbox One app. Run from a VS developer prompt,
# which puts makeappx and signtool on PATH and sets VCToolsRedistDir.
#
#   package.ps1 -BuildDir <folder with nbajam_ofe.exe> -Out <folder> [-Version 1.0.0.0]
#
# The package is only the program: the executable, the ReXGlue DLLs built
# beside it, the C++ runtime and the achievement data. The game itself is
# unpacked into the app's storage on first launch (src/uwp_platform.cpp).

param(
    [Parameter(Mandatory = $true)] [string]$BuildDir,
    [Parameter(Mandatory = $true)] [string]$Out,
    [string]$Version = "1.0.0.0",
    # The smoke test built with -DSMOKETEST_DESKTOP_CRT=ON, the manifest's
    # second tile.
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

# The desktop C++ runtime, carried by the app since the console has none.
$crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" |
       Select-Object -First 1
if (-not $crt) { throw "No C++ runtime under $env:VCToolsRedistDir" }
Get-ChildItem $crt.FullName -Filter *.dll | Copy-Item -Destination $layout

$meta = Join-Path $repo "metadata"
if (Test-Path $meta) { Copy-Item -Recurse $meta (Join-Path $layout "metadata") }
Copy-Item -Recurse (Join-Path $repo "packaging\xbox\Assets") (Join-Path $layout "Assets")
(Get-Content (Join-Path $here "AppxManifest.xml") -Raw) -replace "@VERSION@", $Version |
    Set-Content (Join-Path $layout "AppxManifest.xml") -Encoding UTF8

$appx = Join-Path $Out ("NBAJamOFE_{0}_x64.appx" -f $Version)
& makeappx pack /o /d $layout /p $appx
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)" }

# Developer Mode installs any signed package; the certificate's subject only
# has to be the manifest's publisher.
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=OpenJam" `
    -KeyUsage DigitalSignature -FriendlyName "OpenJam Xbox packages" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
$pfx = Join-Path $env:RUNNER_TEMP "openjam-game.pfx"
$pw = ConvertTo-SecureString -String "openjam" -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pw | Out-Null
& signtool sign /fd SHA256 /f $pfx /p openjam $appx
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }

Write-Host "Package contents:"
Get-ChildItem $layout -Recurse -File | ForEach-Object { "  {0,12:N0}  {1}" -f $_.Length, $_.FullName.Substring($layout.Length + 1) }
Remove-Item $layout -Recurse -Force
Get-ChildItem $Out | Select-Object Name, Length
