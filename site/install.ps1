# Install eskiuc on Windows (x86-64):
#
#   powershell -c "irm https://eskiu-lang.org/install.ps1 | iex"
#
# It downloads the release's zip, checks it against the release's SHA256SUMS, unpacks it
# into %USERPROFILE%\.eskiu (bin and lib, kept together: eskiuc finds its standard library
# beside itself) and adds its bin to your PATH. eskiuc links programs with a MinGW C
# driver: when none is on PATH (gcc, clang or cc), it also installs llvm-mingw (about
# 190 MB, checked against a pinned SHA-256) into %USERPROFILE%\.eskiu\toolchain, where
# eskiuc finds it.
#
#   ESKIU_VERSION       the release tag to install (default: the latest), e.g. v0.14.0
#   ESKIU_PREFIX        where to unpack it (default: %USERPROFILE%\.eskiu)
#   ESKIU_TOOLCHAIN     1: install llvm-mingw even with a C driver on PATH; 0: never

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

# A C driver for linking: one on PATH, else llvm-mingw beside eskiuc.
$tcVersion = '20261020'
$tcSha256 = 'fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd'
$toolchain = Join-Path $prefix 'toolchain'
$haveCC = @('gcc', 'clang', 'cc') | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue }
$want = if ($env:ESKIU_TOOLCHAIN -eq '1') { $true } elseif ($env:ESKIU_TOOLCHAIN -eq '0') { $false }
        else { -not $haveCC -and -not (Test-Path (Join-Path $toolchain 'bin\clang.exe')) }
if ($want) {
    $tcName = "llvm-mingw-$tcVersion-ucrt-x86_64"
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('eskiu-tc-' + [Guid]::NewGuid())
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        $zip = Join-Path $tmp "$tcName.zip"
        Write-Host "No C driver on PATH: downloading $tcName (about 190 MB) ..."
        Invoke-WebRequest -Uri "https://github.com/mstorsjo/llvm-mingw/releases/download/$tcVersion/$tcName.zip" -OutFile $zip -UseBasicParsing
        $actual = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLower()
        if ($actual -ne $tcSha256) { Write-Error "Checksum mismatch for $tcName.zip (expected $tcSha256, got $actual)." }
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        if (Test-Path $toolchain) { Remove-Item -Recurse -Force $toolchain }
        Move-Item -Path (Join-Path $tmp $tcName) -Destination $toolchain
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
    Write-Host "Installed llvm-mingw $tcVersion to $toolchain (eskiuc links with it)."
}
# The toolchain's bin holds the runtime DLLs (libc++, libunwind, winpthread) the programs
# it links load, so it goes on PATH too.
$tcBin = Join-Path $toolchain 'bin'
if (Test-Path (Join-Path $tcBin 'clang.exe')) {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = if ($userPath) { $userPath -split ';' } else { @() }
    if ($parts -notcontains $tcBin) {
        [Environment]::SetEnvironmentVariable('Path', (($parts + $tcBin) -join ';').Trim(';'), 'User')
    }
    $env:Path = "$env:Path;$tcBin"
} elseif (-not $haveCC -and -not (Test-Path (Join-Path $toolchain 'bin\clang.exe'))) {
    Write-Host 'To build programs eskiuc needs a MinGW-w64 C driver: rerun with ESKIU_TOOLCHAIN=1, or install MSYS2 (pacman -S mingw-w64-x86_64-gcc).'
}
Write-Host 'eskiuc doctor lists what each kind of build needs.'
