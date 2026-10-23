# Install eskiuc on Windows (x86-64):
#
#   powershell -c "irm https://eskiu-lang.org/install.ps1 | iex"
#
# It downloads the release's zip, checks it against the release's SHA256SUMS, unpacks it
# into %USERPROFILE%\.eskiu (bin and lib, kept together: eskiuc finds its standard library
# beside itself) and adds its bin to your PATH.
#
#   ESKIU_VERSION   the release tag to install (default: the latest), e.g. v0.14.0
#   ESKIU_PREFIX    where to unpack it (default: %USERPROFILE%\.eskiu)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = 'eskiu-lang/eskiu'
$asset = 'eskiuc-windows-x86_64.zip'

if ([Environment]::Is64BitOperatingSystem -eq $false -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
    Write-Error "There is a prebuilt eskiuc for 64-bit x86 Windows only. Build from source: https://github.com/$repo"
}

$version = if ($env:ESKIU_VERSION) { $env:ESKIU_VERSION } else { 'latest' }
$base = if ($version -eq 'latest') { "https://github.com/$repo/releases/latest/download" }
        else { "https://github.com/$repo/releases/download/$version" }
$prefix = if ($env:ESKIU_PREFIX) { $env:ESKIU_PREFIX } else { Join-Path $env:USERPROFILE '.eskiu' }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('eskiu-' + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $zip = Join-Path $tmp $asset
    Write-Host "Downloading $asset ($version) ..."
    Invoke-WebRequest -Uri "$base/$asset" -OutFile $zip -UseBasicParsing

    $sums = $null
    try { $sums = Invoke-RestMethod -Uri "$base/SHA256SUMS" -UseBasicParsing } catch { }
    if ($sums) {
        if ($sums -is [byte[]]) { $sums = [Text.Encoding]::UTF8.GetString($sums) }
        $line = ($sums -split "`n") | Where-Object { $_ -match " \*?$([regex]::Escape($asset))\s*$" } | Select-Object -First 1
        if ($line) {
            $expected = ($line -split '\s+')[0].ToLower()
            $actual = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLower()
            if ($expected -ne $actual) { Write-Error "Checksum mismatch for $asset (expected $expected, got $actual)." }
            Write-Host 'Checksum verified.'
        } else {
            Write-Warning "No checksum entry for $asset in SHA256SUMS; skipping verification."
        }
    } else {
        Write-Warning "No SHA256SUMS published for $version; skipping checksum verification."
    }

    # Replace an earlier install's files, not anything else in the prefix.
    New-Item -ItemType Directory -Force -Path $prefix | Out-Null
    foreach ($dir in 'bin', 'lib') {
        $old = Join-Path $prefix $dir
        if (Test-Path $old) { Remove-Item -Recurse -Force $old }
    }
    Expand-Archive -Path $zip -DestinationPath $prefix -Force
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

$bin = Join-Path $prefix 'bin'
$exe = Join-Path $bin 'eskiuc.exe'
if (-not (Test-Path $exe)) { Write-Error "The archive did not contain bin\eskiuc.exe." }

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$parts = if ($userPath) { $userPath -split ';' } else { @() }
if ($parts -notcontains $bin) {
    [Environment]::SetEnvironmentVariable('Path', (($parts + $bin) -join ';').Trim(';'), 'User')
    Write-Host "Added $bin to your PATH (open a new terminal to use it)."
}
$env:Path = "$bin;$env:Path"

$installed = & $exe --version
Write-Host "Installed $installed to $exe"
Write-Host 'To build programs it needs MinGW-w64 (gcc or clang) on PATH: MSYS2 (pacman -S mingw-w64-x86_64-gcc) or llvm-mingw.'
