# Package the Xbox One build as three apps: the base game, the 2020s mod and
# the Legends mod. See XBOX.md.
#
# One executable serves all three, exactly as on the PC. What differs is the
# game root each package carries and the identity it installs under. A UWP app
# cannot start another process, so the PC's in-game mod swap - which is a
# relaunch - has nothing to work with on the console; each mod is an app of
# its own instead, with its own tile and its own saves.
#
# Inputs:
#   - the clang-cl UWP build:  cmake --build --preset xbox-uwp-release
#   - a staged game folder:    python tools/dlc.py stage
#     with both mods installed. Its base game and its mods\<id> roots are what
#     go into the packages.
#
# Output, in out\appx\:
#   NBAJamOFE_<version>_x64.appx
#   NBAJamOFE-2020s_<version>_x64.appx
#   NBAJamOFE-Legends_<version>_x64.appx
#   OpenJam.cer                     the certificate they are signed with
#   Microsoft.VCLibs.x64.14.00.appx the CRT they depend on, if the SDK has it
#
# Each package holds a whole game, about 850 MB. They contain the game data
# from your own copy and are for your own console; do not share them.

param(
    [ValidateSet("all", "base", "2020", "legends")]
    [string[]]$Variant = @("all"),

    # The folder `python tools/dlc.py stage` built.
    [Parameter(Mandatory = $true)]
    [string]$GameFolder,

    [string]$BuildDir = "",

    # Mod folder names under <GameFolder>\mods, if they cannot be found by
    # name. Ids are made from the mod's own name, so they are looked for by
    # what they contain.
    [string]$Mod2020Id = "",
    [string]$ModLegendsId = "",

    # Four parts, as the manifest wants. Raise it to update an installed app.
    [string]$Version = "1.0.0.0",

    # Must match the subject of the certificate the packages are signed with.
    [string]$Publisher = "CN=OpenJam",

    # A .pfx to sign with. Without one, a self-signed certificate is made once
    # and kept in out\appx so later packages are signed by the same one.
    [string]$CertificatePath = "",
    [string]$CertificatePassword = "openjam"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $BuildDir) { $BuildDir = Join-Path $here "out\build\xbox-uwp-release" }
$outDir = Join-Path $here "out\appx"
$template = Join-Path $here "packaging\xbox\AppxManifest.xml.in"
$assets = Join-Path $here "packaging\xbox\Assets"

$GameFolder = (Resolve-Path $GameFolder).Path
if (-not (Test-Path (Join-Path $BuildDir "nbajam_ofe.exe"))) {
    throw "Not built: $BuildDir\nbajam_ofe.exe`nRun: cmake --preset xbox-uwp-release; cmake --build --preset xbox-uwp-release"
}
if (-not (Test-Path (Join-Path $GameFolder "default.xex"))) {
    throw "No game in $GameFolder. Point -GameFolder at what 'python tools/dlc.py stage' built."
}

# makeappx and signtool come with the Windows SDK, under a folder per version.
# The newest one wins.
function Find-SdkTool([string]$name) {
    $onPath = Get-Command $name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $found = Get-ChildItem $kits -Filter $name -Recurse -ErrorAction SilentlyContinue |
             Where-Object { $_.Directory.Name -eq "x64" } |
             Sort-Object { $_.Directory.Parent.Name } -Descending |
             Select-Object -First 1
    if (-not $found) { throw "$name not found. Install the Windows 10/11 SDK." }
    return $found.FullName
}
$makeappx = Find-SdkTool "makeappx.exe"
$signtool = Find-SdkTool "signtool.exe"

# Which mod folder is which. The id is made from the mod's name, so it is
# found by what it says rather than spelled out here.
function Find-Mod([string]$given, [string]$pattern, [string]$what) {
    $mods = Join-Path $GameFolder "mods"
    if ($given) {
        $p = Join-Path $mods $given
        if (-not (Test-Path (Join-Path $p "default.xex"))) { throw "$p is not a mod's game root" }
        return $p
    }
    $hits = @(Get-ChildItem $mods -Directory -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -match $pattern })
    if ($hits.Count -ne 1) {
        $names = (Get-ChildItem $mods -Directory -ErrorAction SilentlyContinue).Name -join ", "
        throw "Could not tell which mod is $what (found: $names). Name it with -Mod2020Id / -ModLegendsId."
    }
    return $hits[0].FullName
}

$all = [ordered]@{
    "base"    = @{ Identity = "OpenJam.NBAJamOFE";        File = "NBAJamOFE"
                   Name = "NBA JAM: On Fire Edition";        About = "NBA JAM: On Fire Edition"
                   Root = { $GameFolder } }
    "2020"    = @{ Identity = "OpenJam.NBAJamOFE.2020s";  File = "NBAJamOFE-2020s"
                   Name = "NBA JAM 2020s On Fire Edition";   About = "johnz1's 2020s edition of NBA JAM: On Fire Edition"
                   Root = { Find-Mod $Mod2020Id "2020" "the 2020s edition" } }
    "legends" = @{ Identity = "OpenJam.NBAJamOFE.Legends"; File = "NBAJamOFE-Legends"
                   Name = "NBA JAM Legends On Fire Edition"; About = "johnz1's Legends edition of NBA JAM: On Fire Edition"
                   Root = { Find-Mod $ModLegendsId "legend" "the Legends edition" } }
}
$wanted = if ($Variant -contains "all") { @($all.Keys) } else { $Variant }

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# --- the certificate ---------------------------------------------------------
# Developer Mode installs anything signed, but it has to be signed, and every
# package by the same publisher has to be signed by a certificate whose
# subject is that publisher.
if (-not $CertificatePath) {
    $CertificatePath = Join-Path $outDir "OpenJam.pfx"
    if (-not (Test-Path $CertificatePath)) {
        Write-Host "Making a signing certificate for $Publisher"
        $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher `
            -KeyUsage DigitalSignature -FriendlyName "OpenJam Xbox packages" `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
        $pw = ConvertTo-SecureString -String $CertificatePassword -Force -AsPlainText
        Export-PfxCertificate -Cert $cert -FilePath $CertificatePath -Password $pw | Out-Null
        Export-Certificate -Cert $cert -FilePath (Join-Path $outDir "OpenJam.cer") | Out-Null
    }
}

# --- the CRT the packages depend on -----------------------------------------
# The console installs it alongside the first package that needs it, but only
# if it is handed over too. The Windows SDK ships it.
$vclibs = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} "Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs") `
          -Filter "Microsoft.VCLibs.x64.14.00.appx" -Recurse -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -match "\\Retail\\" } | Select-Object -First 1
if ($vclibs) {
    Copy-Item $vclibs.FullName $outDir -Force
} else {
    Write-Warning "Microsoft.VCLibs.x64.14.00.appx not found in the Windows SDK; get it from Microsoft and install it on the console first."
}

# robocopy succeeds with any exit code below 8.
function Copy-Tree([string]$from, [string]$to, [string[]]$skipDirs, [string[]]$skipFiles) {
    $roboArgs = @($from, $to, "/E", "/NFL", "/NDL", "/NJH", "/NJS", "/NP")
    if ($skipDirs)  { $roboArgs += "/XD"; $roboArgs += $skipDirs }
    if ($skipFiles) { $roboArgs += "/XF"; $roboArgs += $skipFiles }
    & robocopy @roboArgs | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy $from -> $to failed ($LASTEXITCODE)" }
}

foreach ($key in $wanted) {
    $v = $all[$key]
    $root = & $v.Root
    $layout = Join-Path $outDir "layout\$key"
    $appx = Join-Path $outDir ("{0}_{1}_x64.appx" -f $v.File, $Version)
    Write-Host "`n$($v.Name)"
    Write-Host "  game root: $root"

    if (Test-Path $layout) { Remove-Item $layout -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $layout | Out-Null

    # The game root itself, as the package root: the executable plays what is
    # beside it. Left out is everything that belongs to the PC folder rather
    # than to this game - the other mods, saves, logs, the desktop build, the
    # mod manager - and mods.list and saves.path, which on the console would
    # point at things the package does not have. Hard links in a mod's root
    # are copied as the files they point at.
    Copy-Tree $root $layout `
        @((Join-Path $root "mods"), (Join-Path $root "saves"), (Join-Path $root "logs"),
          (Join-Path $root "ui.original"), (Join-Path $root "metadata")) `
        @("*.exe", "*.dll", "*.pdb", "*.log", "mods.list", "saves.path", "unpacking.txt", "crash_stack.txt")

    # What sits beside the executable in the staged folder rather than in the
    # game root: the typefaces and button pictures the chooser and the
    # front end draw with.
    $ui = Join-Path $GameFolder "ui"
    if (Test-Path $ui) { Copy-Tree $ui (Join-Path $layout "ui") @() @() }

    # The UWP build: the executable and the runtime libraries built with it.
    Get-ChildItem $BuildDir -File | Where-Object { $_.Extension -in ".exe", ".dll" } |
        ForEach-Object { Copy-Item $_.FullName $layout -Force }
    $meta = Join-Path $here "metadata"
    if (Test-Path $meta) { Copy-Tree $meta (Join-Path $layout "metadata") @() @() }

    Copy-Tree $assets (Join-Path $layout "Assets") @() @()

    (Get-Content $template -Raw) `
        -replace "@IDENTITY@", $v.Identity `
        -replace "@PUBLISHER@", $Publisher `
        -replace "@VERSION@", $Version `
        -replace "@DISPLAY_NAME@", $v.Name `
        -replace "@DESCRIPTION@", $v.About |
        Set-Content -Path (Join-Path $layout "AppxManifest.xml") -Encoding UTF8

    & $makeappx pack /o /d $layout /p $appx | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "makeappx failed for $key ($LASTEXITCODE)" }
    & $signtool sign /q /fd SHA256 /f $CertificatePath /p $CertificatePassword $appx
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $key ($LASTEXITCODE)" }

    # The layout is a second copy of the whole game; it is not needed once
    # the package exists.
    Remove-Item $layout -Recurse -Force
    Write-Host ("  {0} ({1:N0} MB)" -f $appx, ((Get-Item $appx).Length / 1MB))
}

Write-Host "`nInstall on the console through Device Portal; see XBOX.md."
