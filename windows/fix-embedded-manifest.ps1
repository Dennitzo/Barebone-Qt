param(
    [Parameter(Mandatory = $true)]
    [string]$ExePath
)

$resolved = Resolve-Path -LiteralPath $ExePath -ErrorAction Stop
$bytes = [System.IO.File]::ReadAllBytes($resolved.Path)
$encoding = [System.Text.Encoding]::ASCII

function Replace-Bytes {
    param(
        [byte[]]$Data,
        [string]$Needle,
        [string]$Replacement
    )

    $needleBytes = $encoding.GetBytes($Needle)
    $replacementBytes = $encoding.GetBytes($Replacement)
    if ($needleBytes.Length -ne $replacementBytes.Length) {
        throw "Replacement must have the same byte length as needle."
    }

    $replaced = 0
    for ($i = 0; $i -le $Data.Length - $needleBytes.Length; $i++) {
        $matches = $true
        for ($j = 0; $j -lt $needleBytes.Length; $j++) {
            if ($Data[$i + $j] -ne $needleBytes[$j]) {
                $matches = $false
                break
            }
        }
        if ($matches) {
            for ($j = 0; $j -lt $replacementBytes.Length; $j++) {
                $Data[$i + $j] = $replacementBytes[$j]
            }
            $replaced++
            $i += $needleBytes.Length - 1
        }
    }
    return $replaced
}

$levelCount = Replace-Bytes -Data $bytes -Needle 'ms_asmv2:level' -Replacement 'level         '
$uiAccessCount = Replace-Bytes -Data $bytes -Needle 'ms_asmv2:uiAccess' -Replacement 'uiAccess         '

if ($levelCount -eq 0 -or $uiAccessCount -eq 0) {
    throw "Embedded manifest attributes were not found in $($resolved.Path)."
}

[System.IO.File]::WriteAllBytes($resolved.Path, $bytes)
Write-Host "Fixed embedded manifest attributes in $($resolved.Path)"
