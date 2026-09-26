param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Za-z][0-9A-Za-z._+-]*$')][string]$Tag,
    [switch]$SkipInstaller
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$payload = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$executable = Join-Path (Resolve-Path -LiteralPath $BuildDirectory).Path 'ZcVersionBox.exe'
if (!(Test-Path -LiteralPath $executable)) { throw "Missing build: $executable" }
if (Test-Path -LiteralPath $payload) { throw "Packaging requires a new, empty output path: $payload" }
New-Item -ItemType Directory -Path $payload | Out-Null
Copy-Item -LiteralPath $executable -Destination $payload
Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $executable) 'ZcAiLib.dll') -Destination $payload

$deploy = (Get-Command windeployqt.exe -ErrorAction Stop).Source
& $deploy --release --compiler-runtime --no-translations (Join-Path $payload 'ZcVersionBox.exe') (Join-Path $payload 'ZcAiLib.dll')
if ($LASTEXITCODE) { throw 'windeployqt failed' }
$required = 'ZcVersionBox.exe', 'ZcAiLib.dll', 'Qt6Core.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'Qt6Svg.dll', 'platforms/qwindows.dll', 'vc_redist.x64.exe'
foreach ($file in $required) {
    if (!(Test-Path -LiteralPath (Join-Path $payload $file))) { throw "Missing runtime: $file" }
}
$forbidden = Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object { $_.Name -match '(?i)ela|zcwidget|qlementine.*\.dll|zc_(backup_)?tests' }
if ($forbidden) { throw "Unexpected runtime in package: $($forbidden.Name -join ', ')" }
$licenses = New-Item -ItemType Directory -Path (Join-Path $payload 'licenses')
Copy-Item -LiteralPath "$projectRoot/3rdparty/qlementine/LICENSE" -Destination (Join-Path $licenses.FullName 'Qlementine.txt')
Copy-Item -LiteralPath "$projectRoot/3rdparty/efsw/LICENSE" -Destination (Join-Path $licenses.FullName 'efsw.txt')
Copy-Item -LiteralPath "$projectRoot/3rdparty/ZcAILib/LICENSE" -Destination (Join-Path $licenses.FullName 'ZcAILib.txt')
Copy-Item -LiteralPath "$projectRoot/3rdparty/ZcAILib/UPSTREAM.md" -Destination (Join-Path $licenses.FullName 'ZcAILib-upstream.md')
Copy-Item -LiteralPath "$projectRoot/3rdparty/efsw/UPSTREAM.md" -Destination (Join-Path $licenses.FullName 'efsw-upstream.md')
Copy-Item -LiteralPath "$projectRoot/3rdparty/qlementine/UPSTREAM.md" -Destination $licenses.FullName
Get-ChildItem -LiteralPath "$projectRoot/3rdparty/qlementine/LICENSES" -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $licenses.FullName }
Copy-Item -LiteralPath "$projectRoot/LICENSE" -Destination (Join-Path $licenses.FullName 'ZcVersionBox.txt')
if ($SkipInstaller) { return }

$compiler = Get-Command ISCC.exe -ErrorAction SilentlyContinue
$iscc = if ($compiler) { $compiler.Source } else { 'C:/Program Files (x86)/Inno Setup 6/ISCC.exe' }
if (!(Test-Path -LiteralPath $iscc)) { throw 'Inno Setup 6 is required to create the installer' }
$version = $Tag.TrimStart('v')
$packageDirectory = Split-Path -Parent $payload
& $iscc "/DMyAppVersion=$version" "/DPackageTag=$Tag" "/DPayloadDir=$payload" "/O$packageDirectory" "$projectRoot/scripts/windows-installer.iss"
if ($LASTEXITCODE) { throw 'Installer compilation failed' }
if (!(Test-Path -LiteralPath (Join-Path $packageDirectory "ZcVersionBox-$Tag-setup.exe"))) { throw 'Installer output missing' }
