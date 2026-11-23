# Locate zlib1.dll for the backend's pdf FFI (racket/pdfgist/pdfdoc.rkt
# binds libz "uncompress").
#
# Two distinct needs:
#   1. `raco rivet build` instantiates the backend while assembling the
#      codegen contract, so zlib1.dll must be resolvable on PATH BEFORE the
#      build runs (CI adds the printed directory to PATH).
#   2. The shipped app needs zlib1.dll beside RivetHost.exe; the
#      CopyZlibDll target copies it from the cache this script fills.
#
# Search order: Git for Windows (mingw64, on every GitHub runner), the
# Racket installation, vcpkg. The found DLL is cached under
# windows\build\zlib (gitignored); a cached copy short-circuits the search.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File locate-zlib.ps1
#          -ProjectDir <windows dir>
param(
  [Parameter(Mandatory = $true)][string]$ProjectDir
)

$ErrorActionPreference = "Stop"

$cache = Join-Path $ProjectDir "build\zlib"
$target = Join-Path $cache "zlib1.dll"
if (Test-Path $target) {
  Write-Host "zlib1.dll cached: $target"
  Write-Host "ZLIBDIR=$cache"
  exit 0
}

$candidates = @()
foreach ($root in @("C:\Program Files\Git", $env:RACKET_DIR,
                    "$env:VCPKG_INSTALLATION_ROOT\installed")) {
  if ($root -and (Test-Path $root)) {
    $candidates += Get-ChildItem -Recurse -Filter zlib1.dll $root `
      -ErrorAction SilentlyContinue | Select-Object -First 1
  }
}

$dll = $candidates | Where-Object { $_ } | Select-Object -First 1
if (-not $dll) {
  throw "zlib1.dll not found (searched Git for Windows, RACKET_DIR, vcpkg). Install zlib or set RACKET_DIR/VCPKG_INSTALLATION_ROOT."
}

New-Item -ItemType Directory -Force -Path $cache | Out-Null
Copy-Item $dll.FullName $target -Force
Write-Host "zlib1.dll: $($dll.FullName) -> $target"
Write-Host "ZLIBDIR=$cache"
