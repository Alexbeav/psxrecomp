# release_overlay_stage.ps1 — THE shared overlay-shard release staging for every
# psxrecomp title. Dot-source it from a title's tools/package_release.ps1:
#
#   . "$FrameworkRoot\tools\release_overlay_stage.ps1"
#   $CgTag = Get-OverlayCgTag -RecompTools ... -RecompInc ... -GameExe ... -GameToml ...
#   Add-OverlayCache     -GameId "SCUS-94423" -CacheSrcRoot ... -Stage ... -CgTag $CgTag
#   Add-OverlayToolchain -Stage ... -RecompDir ... -RecompTools ... -RecompInc ... -MingwBin ... -DlCache ...
#
# WHY THIS FILE EXISTS
# --------------------
# Every title used to carry its own hand-copied version of this logic, and the
# copies drifted three ways (measured 2026-09-01):
#
#   * MegaManX6 invented the tcc toolchain tier (a277e56, 2026-06-25, 345 lines).
#   * Ape Escape's packager was created 2026-07-05 by copying that and trimming
#     it to 146 lines. The overlay cache + toolchain staging were among the lines
#     cut. Ape has NEVER contained `overlay_toolchain` or `AllowNoCache` in any
#     commit, so every Ape release ever shipped ran 100% of its overlay
#     dispatches on the dirty-RAM interpreter (measured: disp_native=0,
#     disp_interp=4,480,307 in a single session).
#   * Tomba 2 later added SHA256 pinning (0a9c3a3); MegaManX6 never got it back
#     and kept trusting whatever the mirror served, plus reusing a cached archive
#     forever on a bare Test-Path.
#
# The copies drifted because improvements never flow back between forks AND
# because nothing fails when a title lacks the feature: the runtime silently
# falls back to interpretation, so a stripped packager looks perfectly healthy.
# That is why Add-OverlayCache THROWS by default instead of warning — a missing
# cache has to stop a release, not scroll past in a log.
#
# Keep this file title-agnostic. Anything game-specific belongs in the caller.

# NOTE: deliberately no `Set-StrictMode` here. This file is DOT-SOURCED, so any
# strictness set at its top level applies to the CALLER's scope for the rest of
# that script -- a framework helper must not silently change how a title's
# packager evaluates. (Measured: it broke an unrelated caller epilogue.)

# Pinned toolchain archives. Single source of truth for every title: a game
# cannot ship an unpinned toolchain by forgetting to pass a hash.
$script:PsxToolchainPins = @{
    PythonVersion = "3.13.1"
    PythonSha256  = "7b7923ff0183a8b8fca90f6047184b419b108cb437f75fc1c002f9d2f8bcec16"
    TccVersion    = "0.9.27"
    TccSha256     = "34a721949a2583fdff725312da092fa0f5f1f284b702e6f811c6954714faabb2"
}

function New-PsxDir {
    param([Parameter(Mandatory)][string]$Path)
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    return $Path
}

<#
.SYNOPSIS
Fetch an archive and verify its SHA256 on EVERY use, including cache hits.

.DESCRIPTION
A bare `if (-not (Test-Path $zip)) { download }` trusts whatever the mirror
served the day the cache was first filled, forever. This verifies the cached
copy too, refetching when it does not match. python.org and savannah both 502
periodically, so transient failures retry with backoff rather than losing a
whole release build.
#>
function Get-PinnedArchive {
    param(
        [Parameter(Mandatory)][string]$Uri,
        [Parameter(Mandatory)][string]$Sha256,
        [Parameter(Mandatory)][string]$Destination,
        [int]$Retries = 4
    )
    $name = Split-Path -Leaf $Destination
    if (Test-Path -LiteralPath $Destination) {
        $have = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLower()
        if ($have -eq $Sha256.ToLower()) { return $Destination }
        Write-Warning "$name in the download cache has SHA256 $have (expected $Sha256); refetching"
        Remove-Item -LiteralPath $Destination -Force
    }
    for ($attempt = 1; $attempt -le $Retries; $attempt++) {
        try {
            $tmp = "$Destination.tmp"
            if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
            Invoke-WebRequest -Uri $Uri -OutFile $tmp -UseBasicParsing
            $got = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash.ToLower()
            if ($got -ne $Sha256.ToLower()) {
                Remove-Item -LiteralPath $tmp -Force
                throw "SHA256 mismatch for $name : got $got, expected $Sha256"
            }
            Move-Item -LiteralPath $tmp -Destination $Destination -Force
            return $Destination
        } catch {
            if ($attempt -eq $Retries) {
                throw ("Failed to fetch $name after $Retries attempts: $($_.Exception.Message). " +
                       "Place a verified copy at $Destination and re-run.")
            }
            $delay = [Math]::Min(30, [Math]::Pow(2, $attempt))
            Write-Warning "$name fetch attempt $attempt failed ($($_.Exception.Message)); retrying in ${delay}s"
            Start-Sleep -Seconds $delay
        }
    }
}

<#
.SYNOPSIS
Derive the codegen cache tag (cg<ver>_<hash>_gc<cfghash>) for a release.

.DESCRIPTION
The tag namespaces the shard cache. It is derived from the PACKAGED game.toml,
not the dev one: a cache built against the dev config lands under a different
tag and the shipped runtime silently ignores it. Callers must pass the staged
game.toml for exactly this reason.
#>
function Get-OverlayCgTag {
    param(
        [Parameter(Mandatory)][string]$RecompTools,   # framework tools/ (has compile_overlays.py)
        [Parameter(Mandatory)][string]$RecompInc,     # framework runtime/include
        [Parameter(Mandatory)][string]$GameExe,       # psxrecomp-game.exe
        [Parameter(Mandatory)][string]$GameToml,      # the STAGED game.toml
        [int]$Flavor = 0                              # 0 base, 1 widescreen, 2 pgxp
    )
    $tagScript = Join-Path ([IO.Path]::GetTempPath()) ("psx_cgtag_" + [guid]::NewGuid().ToString("N") + ".py")
    @"
import importlib.util
s = importlib.util.spec_from_file_location('co', r'$RecompTools\compile_overlays.py')
m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
# Ask the tool that OWNS the cache layout for the tag. Never reformat it here:
# a parallel PowerShell format string is exactly how this went stale when the
# _f<flavor> suffix was added, staging zero shards from a valid cache.
print(m.cache_tag(r'$RecompInc', r'$GameExe', r'$GameToml', $Flavor))
"@ | Set-Content -Encoding ASCII $tagScript
    try {
        $tag = (& py -3 $tagScript).Trim()
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue $tagScript
    }
    if (-not $tag) { throw "Could not derive the overlay codegen tag (compile_overlays.py probe returned nothing)" }
    return $tag
}

<#
.SYNOPSIS
Stage the prebuilt overlay shard cache for one codegen tag, or refuse to ship.

.DESCRIPTION
Only shards under $CgTag are copied: the runtime ignores every other namespace,
so shipping them would just inflate the download. Shipping with NO cache is a
real downgrade -- every player's first visit to every area runs interpreted --
so it throws unless -AllowNoCache makes that a deliberate, recorded choice.
#>
function Add-OverlayCache {
    param(
        [Parameter(Mandatory)][string]$GameId,        # e.g. SCUS-94423
        [Parameter(Mandatory)][string]$CacheSrcRoot,  # <root>/<buildDir>/cache
        [Parameter(Mandatory)][string]$Stage,         # staging dir
        [Parameter(Mandatory)][string]$CgTag,
        [switch]$AllowNoCache
    )
    $CacheSrc = Join-Path $CacheSrcRoot $GameId
    if (Test-Path $CacheSrc) {
        $CacheDst = Join-Path $Stage "cache/$GameId"
        $cacheFiles = @(Get-ChildItem $CacheSrc -Recurse -File -Include *.dll,*.ranges,*.resident |
            Where-Object { $_.FullName -notmatch '[\\/]sljit[\\/]' -and $_.FullName -match "[\\/]$CgTag[\\/]" })
        if ($cacheFiles.Count -eq 0 -and -not $AllowNoCache) {
            throw ("Overlay cache at $CacheSrc has no shards for this build's codegen tag $CgTag - " +
                   "rebuild the cache with compile_overlays.py against this runtime, or pass " +
                   "-AllowNoCache to release without one deliberately")
        }
        foreach ($f in $cacheFiles) {
            $rel  = $f.FullName.Substring($CacheSrc.Length).TrimStart('\','/')
            $dest = Join-Path $CacheDst $rel
            New-PsxDir (Split-Path $dest) | Out-Null
            Copy-Item $f.FullName $dest
        }
        $dllCount = @(Get-ChildItem $CacheDst -Recurse -Filter *.dll -ErrorAction SilentlyContinue).Count
        Write-Host "Bundled overlay cache: $dllCount native overlay DLL(s) [$CgTag]"

        # Assert the STAGED LAYOUT, not just the copied count (pattern from
        # MegaManX6). The loader scans cache/<id>/<compiler>/<arch-abi>/<tag>/
        # exactly; a shard that lands anywhere else is worth exactly as much as
        # no shard at all, and a count can never tell the difference.
        if ($dllCount -gt 0) {
            # Locate the tag DIRECTORY by name and count what is inside it. A path
            # regex here is separator-fragile: "[\/]" matches only a forward
            # slash, so on Windows it silently matched nothing and failed a
            # perfectly correct 78-shard layout. Directory identity has no such trap.
            $tagDirs = @(Get-ChildItem $CacheDst -Recurse -Directory -ErrorAction SilentlyContinue |
                         Where-Object { $_.Name -eq $CgTag })
            $staged = @($tagDirs | ForEach-Object {
                Get-ChildItem $_.FullName -File -Filter *.dll -ErrorAction SilentlyContinue })
            if ($staged.Count -ne $dllCount) {
                throw ("Staged overlay cache layout is wrong: expected $dllCount shard(s) under " +
                       "cache/$GameId/<compiler>/<arch-abi>/$CgTag/ but found $($staged.Count). " +
                       "The loader scans that exact path, so the bundled cache would never load.")
            }
        }
        return $dllCount
    } elseif ($AllowNoCache) {
        Write-Warning "No overlay cache at $CacheSrc - shipping without one because -AllowNoCache was given"
        return 0
    } else {
        throw @"
No overlay cache found at $CacheSrc, so this package would ship without one and
every player's first session would run overlays on the dirty-RAM interpreter.

Build a cache for THIS release's codegen tag ($CgTag). The tag is derived from
the PACKAGED game.toml, so a cache built against the dev game.toml lands under a
different tag and the shipped runtime will silently ignore it:

  py -3 <framework>\tools\compile_overlays.py ``
      --captures    <coverage vault>\overlay_captures.json ``
      --game-toml   <staged game.toml> ``
      --recompiler  <framework>\recompiler\build\psxrecomp-game.exe ``
      --runtime-include <framework>\runtime\include ``
      --out-dir     $CacheSrcRoot ``
      --gcc         C:\msys64\mingw64\bin\gcc.exe --cps

Then re-run this packager. Pass -AllowNoCache to ship without one anyway.
"@
    }
}

<#
.SYNOPSIS
Stage the self-contained overlay toolchain (embedded python + tcc + recompiler).

.DESCRIPTION
This is the fallback that lets a player with NO compiler installed still turn
captured overlays into native code. Without it the runtime's autocompile gate
(tk_present) is false and overlays stay interpreted forever, which is exactly
how Ape Escape shipped for its entire life.
#>
function Add-OverlayToolchain {
    param(
        [Parameter(Mandatory)][string]$Stage,
        [Parameter(Mandatory)][string]$RecompDir,     # dir holding psxrecomp-game.exe
        [Parameter(Mandatory)][string]$RecompTools,   # framework tools/
        [Parameter(Mandatory)][string]$RecompInc,     # framework runtime/include
        [Parameter(Mandatory)][string]$MingwBin,
        [Parameter(Mandatory)][string]$DlCache
    )
    $Toolchain = New-PsxDir (Join-Path $Stage "overlay_toolchain")
    New-PsxDir $DlCache | Out-Null

    $PyVer = $script:PsxToolchainPins.PythonVersion
    $PyZip = Get-PinnedArchive `
        -Uri "https://www.python.org/ftp/python/$PyVer/python-$PyVer-embed-amd64.zip" `
        -Sha256 $script:PsxToolchainPins.PythonSha256 `
        -Destination (Join-Path $DlCache "python-$PyVer-embed-amd64.zip")
    Expand-Archive -LiteralPath $PyZip -DestinationPath (Join-Path $Toolchain "python") -Force

    $TccVer = $script:PsxToolchainPins.TccVersion
    $TccZip = Get-PinnedArchive `
        -Uri "https://download.savannah.gnu.org/releases/tinycc/tcc-$TccVer-win64-bin.zip" `
        -Sha256 $script:PsxToolchainPins.TccSha256 `
        -Destination (Join-Path $DlCache "tcc-$TccVer-win64-bin.zip")
    $TccTmp = Join-Path $DlCache "tcc_extract"
    if (Test-Path -LiteralPath $TccTmp) { Remove-Item -LiteralPath $TccTmp -Recurse -Force }
    Expand-Archive -LiteralPath $TccZip -DestinationPath $TccTmp -Force
    Copy-Item -Recurse -Force (Join-Path $TccTmp "tcc") (Join-Path $Toolchain "tcc")

    Copy-Item (Join-Path $RecompDir "psxrecomp-game.exe") $Toolchain
    foreach ($d in @("libgcc_s_seh-1.dll","libstdc++-6.dll","libwinpthread-1.dll")) {
        Copy-Item (Join-Path $MingwBin $d) $Toolchain
    }
    Copy-Item (Join-Path $RecompTools "compile_overlays.py") $Toolchain
    $ToolInc = New-PsxDir (Join-Path $Toolchain "include")
    Copy-Item (Join-Path $RecompInc "*.h") $ToolInc

    # The runtime gates autocompile on this exact file. If the layout ever
    # changes, fail here rather than shipping a toolchain the runtime ignores.
    $probe = Join-Path $Toolchain "python\python.exe"
    if (-not (Test-Path -LiteralPath $probe)) {
        throw "Staged overlay_toolchain is missing python\python.exe ($probe); the runtime's autocompile gate would be false and overlays would stay interpreted"
    }
    $tcMB = "{0:N0}" -f ((Get-ChildItem $Toolchain -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
    Write-Host "Bundled overlay toolchain (embedded python + tcc + recompiler): ~$tcMB MB"
    return $Toolchain
}

<#
.SYNOPSIS
Stage the mod catalog and verify it DERIVES from the real sources.

.DESCRIPTION
Every title used to assert a hard-coded package count -- Tomba 2 `-ne 5`,
MegaManX6 `-lt 16`, Ape Escape `-ne 4`. All three describe shared framework
content, so all three go stale the moment the framework gains or loses a mod:
measured 2026-09-01, Tomba 2 demanded 5 while the true catalog was 7 (the
framework had added psx.enhancement.pgxp and the game had added a fourth mod),
which made the title unreleasable for a reason that had nothing to do with it.

So assert the INVARIANT instead of a number: everything the sources define must
survive into the package. That cannot go stale when a mod is added, and it still
catches the failure that actually matters -- a mod silently not shipping.

Staging copies from the BUILD dir, never the source tree: the framework stages
its own builtin packages there at build time, and copying the source tree
silently drops them (pattern and rationale from MegaManX6).
#>
function Add-ModCatalog {
    param(
        [Parameter(Mandatory)][string]$BuildPath,          # build dir (has mods/bundled)
        [Parameter(Mandatory)][string]$Stage,
        [string]$GameModSource      = "",                  # <repo>/mods/preloaded
        [string]$FrameworkModSource = ""                   # <framework>/mods/builtin
    )
    $ModsSrc = Join-Path $BuildPath "mods"
    if (-not (Test-Path (Join-Path $ModsSrc "bundled"))) {
        throw "No mod catalog staged at $ModsSrc - build the runtime first (the framework stages its builtin packages there)"
    }
    Copy-Item -Recurse -Force $ModsSrc (Join-Path $Stage "mods")
    # mods/bundled is build output and ships. Two things under mods/ belong to
    # this machine and must never reach a release: installed/ holds .psxmod
    # archives the developer installed as a player would, and state.toml is
    # their own enable/disable selection over a catalog that ships default-off.
    $StagedMods = Join-Path $Stage "mods"
    Remove-Item -Recurse -Force (Join-Path $StagedMods "installed") -ErrorAction SilentlyContinue
    Remove-Item -Force (Join-Path $StagedMods "state.toml") -ErrorAction SilentlyContinue
    Remove-Item -Force (Join-Path $StagedMods "state.toml.tmp") -ErrorAction SilentlyContinue
    $StagedPkgDir = Join-Path $Stage "mods\bundled"

    $stagedIds = @(Get-ChildItem $StagedPkgDir -Directory -ErrorAction SilentlyContinue |
                   ForEach-Object { $_.Name })
    if ($stagedIds.Count -eq 0) { throw "Staged mod catalog at $StagedPkgDir is empty" }

    # Every package the sources define must be present. Missing = a real defect.
    $missing = @()
    foreach ($src in @($GameModSource, $FrameworkModSource)) {
        if (-not $src -or -not (Test-Path (Join-Path $src "packages"))) { continue }
        foreach ($want in (Get-ChildItem (Join-Path $src "packages") -Directory)) {
            if ($stagedIds -notcontains $want.Name) { $missing += $want.Name }
        }
    }
    if ($missing.Count -gt 0) {
        throw ("Mod catalog is missing package(s) the sources define: " + ($missing -join ", ") +
               ". They exist in the source tree but did not reach $StagedPkgDir, so the release " +
               "would ship a Mods page the dev build does not have.")
    }

    # Report the derived breakdown. Framework-owned packages are the psx.* ones.
    $fw   = @($stagedIds | Where-Object { $_ -like "psx.*" })
    $game = @($stagedIds | Where-Object { $_ -notlike "psx.*" })
    Write-Host ("Bundled mod catalog: {0} package(s) = {1} game-owned + {2} framework-owned" -f `
                $stagedIds.Count, $game.Count, $fw.Count)
    return $stagedIds.Count
}
