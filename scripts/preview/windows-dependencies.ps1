param(
    [Parameter(Mandatory = $true)] [string] $Report,
    [Parameter(Mandatory = $true)] [string] $RuntimeList,
    [Parameter(Mandatory = $true)] [string] $Objdump,
    [Parameter(Mandatory = $true)] [string[]] $Binary,
    [Parameter(Mandatory = $true)] [string[]] $SearchDirectory
)

$ErrorActionPreference = "Stop"
$systemDirectory = [IO.Path]::GetFullPath([Environment]::SystemDirectory)
$searchDirectories = @($SearchDirectory | ForEach-Object { [IO.Path]::GetFullPath($_) })
$queue = [Collections.Generic.Queue[string]]::new()
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$runtimeByName = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
$lines = [Collections.Generic.List[string]]::new()

foreach ($path in $Binary) {
    $fullPath = [IO.Path]::GetFullPath($path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Packaged PE binary is missing: $fullPath"
    }
    $queue.Enqueue($fullPath)
}

while ($queue.Count -gt 0) {
    $current = $queue.Dequeue()
    if (-not $seen.Add($current)) { continue }
    $imports = & $Objdump -p $current
    if ($LASTEXITCODE -ne 0) { throw "objdump failed for $current" }
    $dllNames = @($imports | ForEach-Object {
        if ($_ -match '^\s*DLL Name:\s*(.+?)\s*$') { $Matches[1] }
    } | Where-Object { $_ } | Sort-Object -Unique)
    if ($dllNames.Count -eq 0) { throw "No PE imports found for $current" }

    foreach ($dllName in $dllNames) {
        if ($dllName -match '^(api-ms-win-|ext-ms-win-)') {
            $lines.Add("$([IO.Path]::GetFileName($current)): $dllName => Windows API set")
            continue
        }
        $resolved = $null
        foreach ($directory in @([IO.Path]::GetDirectoryName($current), $systemDirectory) + $searchDirectories) {
            $candidate = Join-Path $directory $dllName
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $resolved = [IO.Path]::GetFullPath($candidate)
                break
            }
        }
        if ($null -eq $resolved) { throw "Unable to resolve PE import $dllName from $current" }
        if ($resolved.StartsWith($systemDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            $lines.Add("$([IO.Path]::GetFileName($current)): $dllName => %SystemRoot%\System32\$dllName")
            continue
        }
        if ($runtimeByName.ContainsKey($dllName) -and $runtimeByName[$dllName] -ne $resolved) {
            throw "Conflicting runtime DLLs named $dllName"
        }
        $runtimeByName[$dllName] = $resolved
        $lines.Add("$([IO.Path]::GetFileName($current)): $dllName => runtime/$dllName")
        $queue.Enqueue($resolved)
    }
}

[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Report))) | Out-Null
[IO.File]::WriteAllLines([IO.Path]::GetFullPath($Report), @($lines | Sort-Object -Unique))
[IO.File]::WriteAllLines([IO.Path]::GetFullPath($RuntimeList), @($runtimeByName.Values | Sort-Object))
