<#
.SYNOPSIS
  Prints the path of vcvars64.bat of the newest Visual Studio 2022 (or later) install that has the C++ x64 tools:
  Build Tools, Community, Professional or Enterprise (GitHub's windows-2022 runners have Enterprise). Uses vswhere,
  which every Visual Studio installer puts in a fixed place; falls back to the Build Tools default path.
  $env:OPF_VCVARS (the path of a vcvars64.bat) overrides the search, as in src\OblivionPerfFix\build.ps1. A release
  install is preferred; a preview / Insiders install (which vswhere lists only with -prerelease) is used when it is
  the only one.
  Exit code 1 (and a message on stderr) if none is found.

.EXAMPLE
  $vcvars = & tools\Find-VCVars.ps1
  cmd /c "call `"$vcvars`" >nul && cl /?"
#>
if ($env:OPF_VCVARS) {
    if (Test-Path -LiteralPath $env:OPF_VCVARS) { return $env:OPF_VCVARS }
    [Console]::Error.WriteLine("OPF_VCVARS is set but $env:OPF_VCVARS does not exist; searching for Visual Studio instead.")
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    foreach ($pre in @($false, $true)) {
        # an argument list, not a splatted '$extra': an empty 'if' result is $null, which reaches vswhere as an empty
        # argument (its "Error 0x57", invalid parameter)
        $vsArgs = @('-latest', '-products', '*', '-version', '[17.0,', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
        if ($pre) { $vsArgs = @('-prerelease') + $vsArgs }
        $paths = & $vswhere $vsArgs
        foreach ($p in @($paths)) {
            if (-not $p) { continue }
            $bat = Join-Path $p 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $bat) { return $bat }
        }
    }
}
$fallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (Test-Path -LiteralPath $fallback) { return $fallback }
[Console]::Error.WriteLine('Visual Studio 2022 with the C++ x64 build tools not found. Install the free "Build Tools for Visual Studio 2022" with the "Desktop development with C++" workload, or set OPF_VCVARS to a vcvars64.bat.')
exit 1
