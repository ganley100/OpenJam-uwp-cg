# Pack and sign the smoke test as an .appx. Run from a VS developer prompt,
# which puts makeappx and signtool on PATH.
#
#   package.ps1 -Exe <smoketest.exe> -Out <folder> [-Version 1.0.0.0]

param(
    [Parameter(Mandatory = $true)] [string]$Exe,
    [Parameter(Mandatory = $true)] [string]$Out,
    [string]$Version = "1.0.0.0"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)
New-Item -ItemType Directory -Force $Out | Out-Null
$layout = Join-Path $Out "layout"
if (Test-Path $layout) { Remove-Item $layout -Recurse -Force }
New-Item -ItemType Directory -Force $layout | Out-Null

Copy-Item $Exe (Join-Path $layout "smoketest.exe")
Copy-Item -Recurse (Join-Path $repo "packaging\xbox\Assets") (Join-Path $layout "Assets")
(Get-Content (Join-Path $here "AppxManifest.xml") -Raw) -replace "@VERSION@", $Version |
    Set-Content (Join-Path $layout "AppxManifest.xml") -Encoding UTF8

$appx = Join-Path $Out ("OpenJamXboxTest_{0}_x64.appx" -f $Version)
& makeappx pack /o /d $layout /p $appx
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)" }

# Developer Mode installs any signed package; the signature only has to be
# from a certificate whose subject is the manifest's publisher.
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=OpenJam" `
    -KeyUsage DigitalSignature -FriendlyName "OpenJam Xbox packages" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
$pfx = Join-Path $env:RUNNER_TEMP "openjam.pfx"
$pw = ConvertTo-SecureString -String "openjam" -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pw | Out-Null
Export-Certificate -Cert $cert -FilePath (Join-Path $Out "OpenJam.cer") | Out-Null
& signtool sign /fd SHA256 /f $pfx /p openjam $appx
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }

# The C++ runtime the app depends on. The console needs it installed once.
$vclibs = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs" `
    -Recurse -Filter "Microsoft.VCLibs.x64.14.00.appx" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\Retail\\" } | Select-Object -First 1
if ($vclibs) {
    Copy-Item $vclibs.FullName $Out
} else {
    Write-Warning "Microsoft.VCLibs.x64.14.00.appx not found on this machine"
}

Remove-Item $layout -Recurse -Force
Get-ChildItem $Out | Select-Object Name, Length
