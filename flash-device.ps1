<#
.SYNOPSIS
Selects a device profile (config + favicon) and uploads firmware via PlatformIO.

.DESCRIPTION
Prepares project files for a target device by copying the selected profile config
to `config.json` and `data/config.json`, and managing `data/favicon.svg`.
Then runs the appropriate PlatformIO upload command for USB or OTA mode.

.PARAMETER ota
Use OTA upload mode.
Mutually exclusive with `-usb`.

.PARAMETER usb
Use USB upload mode.
Mutually exclusive with `-ota`.

.PARAMETER Device
Target device profile.
Allowed values: `peletky`, `boiler`, `test`.

.PARAMETER NoUpload
Prepare/copy files only. Skip the final PlatformIO upload command.

.PARAMETER SkipLiveCompare
Skip OTA live-config preflight comparison (`/api/settings` vs selected profile).

.EXAMPLE
.\flash-device.ps1 -usb -Device peletky
Prepares peletky profile and uploads using the USB PlatformIO environment.

.EXAMPLE
.\flash-device.ps1 -ota -Device boiler
Prepares boiler profile and uploads using OTA with the configured host/IP.

.EXAMPLE
.\flash-device.ps1 -ota -Device test -NoUpload
Applies the test profile and exits before upload.

.NOTES
Edit the "User-editable variables" section in this script to set per-device
OTA host/IP or USB port mappings.
#>
[CmdletBinding()]
param(
    [Parameter(ParameterSetName = "ota", Mandatory = $true)]
    [switch]$ota,

    [Parameter(ParameterSetName = "usb", Mandatory = $true)]
    [switch]$usb,

    [Parameter(Mandatory = $true)]
    [ValidateSet("peletky", "boiler", "test")]
    [string]$Device,

    [switch]$NoUpload,

    [switch]$SkipLiveCompare
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# -----------------------------
# User-editable variables
# -----------------------------
$PioEnvByUploadType = @{
    ota = "esp32-s3-devkitc-1-ota"
    usb = "esp32-s3-devkitc-1"
}

# Set per-device OTA host/IP and optional USB COM port.
$DeviceTargets = @{
    peletky = @{
        ota = "esp32peletky.local"
        usb = ""
    }
    boiler = @{
        ota = "esp32boiler.local"
        usb = ""
    }
    test = @{
        ota = "esp32test.local"
        usb = ""
    }
}

$DeviceProfiles = @{
    peletky = @{
        config = "!config.json.peletky"
        icon = "!favicon.svg.peletky"
    }
    boiler = @{
        config = "!config.json.boiler"
        icon = "!favicon.svg.boiler"
    }
    test = @{
        config = "!config.json.test"
        icon = $null
    }
}

# OTA preflight compare behavior
$CompareIgnorePaths = @(
    "network.serial_logging_enabled"
)
$CompareIgnoreKeys = @(
    "password",
    "password_hash"
)
$MaxDiffLinesToPrint = 80

function Assert-PathExists {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
}

function Copy-FileChecked {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][string]$Label
    )

    Assert-PathExists -Path $SourcePath -Label $Label
    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
    Write-Host "Copied ${Label}: $SourcePath -> $DestinationPath"
}

function Assert-JsonValid {
    param(
        [Parameter(Mandatory = $true)][string]$Path
    )

    Assert-PathExists -Path $Path -Label "JSON file"
    $content = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    try {
        $null = $content | ConvertFrom-Json
    }
    catch {
        throw "Invalid JSON in $Path. $($_.Exception.Message)"
    }
}

function Is-JsonArray {
    param(
        [Parameter(Mandatory = $true)]$Value
    )

    return ($Value -is [System.Array]) -or (
        $Value -is [System.Collections.IEnumerable] -and
        -not ($Value -is [string]) -and
        -not ($Value -is [System.Collections.IDictionary]) -and
        -not ($Value -is [PSCustomObject])
    )
}

function Is-JsonObject {
    param(
        [Parameter(Mandatory = $true)]$Value
    )

    return ($Value -is [System.Collections.IDictionary]) -or ($Value -is [PSCustomObject])
}

function Get-JsonObjectKeys {
    param(
        [Parameter(Mandatory = $true)]$Value
    )

    if ($Value -is [System.Collections.IDictionary]) {
        return @($Value.Keys)
    }

    return @($Value.PSObject.Properties.Name)
}

function Get-JsonObjectValue {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Key
    )

    if ($Value -is [System.Collections.IDictionary]) {
        return $Value[$Key]
    }

    return $Value.$Key
}

function Should-IgnoreComparePath {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path
    )

    return $CompareIgnorePaths -contains $Path
}

function Should-IgnoreCompareKey {
    param(
        [Parameter(Mandatory = $true)][string]$Key
    )

    return $CompareIgnoreKeys -contains $Key
}

function Format-CompareValue {
    param(
        $Value
    )

    if ($null -eq $Value) {
        return "<null>"
    }

    if ($Value -is [string]) {
        $text = $Value
    }
    elseif (Is-JsonObject -Value $Value -or Is-JsonArray -Value $Value) {
        $text = $Value | ConvertTo-Json -Compress -Depth 20
    }
    else {
        $text = [string]$Value
    }

    if ($text.Length -gt 180) {
        return $text.Substring(0, 177) + "..."
    }

    return $text
}

function Compare-JsonValues {
    param(
        $LocalValue,
        $LiveValue,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diffs
    )

    if (Should-IgnoreComparePath -Path $Path) {
        return
    }

    if ($null -eq $LocalValue -and $null -eq $LiveValue) {
        return
    }

    if ($null -eq $LocalValue -or $null -eq $LiveValue) {
        $Diffs.Add([PSCustomObject]@{
            Kind = "value_mismatch"
            Path = $Path
            Local = $LocalValue
            Live = $LiveValue
        })
        return
    }

    $localIsObject = Is-JsonObject -Value $LocalValue
    $liveIsObject = Is-JsonObject -Value $LiveValue
    if ($localIsObject -and $liveIsObject) {
        $localKeys = Get-JsonObjectKeys -Value $LocalValue
        $liveKeys = Get-JsonObjectKeys -Value $LiveValue

        $localFiltered = @($localKeys | Where-Object { -not (Should-IgnoreCompareKey -Key $_) } | Sort-Object -Unique)
        $liveFiltered = @($liveKeys | Where-Object { -not (Should-IgnoreCompareKey -Key $_) } | Sort-Object -Unique)

        foreach ($key in $localFiltered) {
            $childPath = if ([string]::IsNullOrEmpty($Path)) { $key } else { "$Path.$key" }
            if (Should-IgnoreComparePath -Path $childPath) {
                continue
            }

            if ($liveFiltered -notcontains $key) {
                $Diffs.Add([PSCustomObject]@{
                    Kind = "missing_in_live"
                    Path = $childPath
                    Local = Get-JsonObjectValue -Value $LocalValue -Key $key
                    Live = $null
                })
                continue
            }

            Compare-JsonValues `
                -LocalValue (Get-JsonObjectValue -Value $LocalValue -Key $key) `
                -LiveValue (Get-JsonObjectValue -Value $LiveValue -Key $key) `
                -Path $childPath `
                -Diffs $Diffs
        }

        foreach ($key in $liveFiltered) {
            if ($localFiltered -contains $key) {
                continue
            }

            $childPath = if ([string]::IsNullOrEmpty($Path)) { $key } else { "$Path.$key" }
            if (Should-IgnoreComparePath -Path $childPath) {
                continue
            }

            $Diffs.Add([PSCustomObject]@{
                Kind = "extra_in_live"
                Path = $childPath
                Local = $null
                Live = Get-JsonObjectValue -Value $LiveValue -Key $key
            })
        }

        return
    }

    $localIsArray = Is-JsonArray -Value $LocalValue
    $liveIsArray = Is-JsonArray -Value $LiveValue
    if ($localIsArray -and $liveIsArray) {
        $localItems = @($LocalValue)
        $liveItems = @($LiveValue)
        if ($localItems.Count -ne $liveItems.Count) {
            $Diffs.Add([PSCustomObject]@{
                Kind = "list_len_mismatch"
                Path = $Path
                Local = $localItems.Count
                Live = $liveItems.Count
            })
            return
        }

        for ($i = 0; $i -lt $localItems.Count; $i++) {
            $childPath = "$Path[$i]"
            Compare-JsonValues `
                -LocalValue $localItems[$i] `
                -LiveValue $liveItems[$i] `
                -Path $childPath `
                -Diffs $Diffs
        }

        return
    }

    if ($LocalValue -ne $LiveValue) {
        $Diffs.Add([PSCustomObject]@{
            Kind = "value_mismatch"
            Path = $Path
            Local = $LocalValue
            Live = $LiveValue
        })
    }
}

function Confirm-Continue {
    param(
        [Parameter(Mandatory = $true)][string]$Prompt
    )

    $answer = Read-Host $Prompt
    return $answer -match '^(?i:y|yes)$'
}

function Invoke-LiveConfigCompare {
    param(
        [Parameter(Mandatory = $true)][string]$ConfigPath,
        [Parameter(Mandatory = $true)][string]$HostOrIp
    )

    Assert-PathExists -Path $ConfigPath -Label "profile config"
    $localConfig = Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json

    $username = [string]$localConfig.security.username
    $password = [string]$localConfig.security.password_hash
    if ([string]::IsNullOrWhiteSpace($username) -or [string]::IsNullOrWhiteSpace($password)) {
        throw "Profile config missing security.username or security.password_hash required for /api/settings auth."
    }

    $baseUri = if ($HostOrIp -match '^https?://') {
        $HostOrIp.TrimEnd('/')
    }
    else {
        "http://$HostOrIp"
    }
    $settingsUri = "$baseUri/api/settings"

    $token = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${username}:${password}"))
    $headers = @{ Authorization = "Basic $token" }
    $liveConfig = Invoke-RestMethod -Method Get -Uri $settingsUri -Headers $headers -TimeoutSec 20

    $diffs = New-Object System.Collections.Generic.List[object]
    Compare-JsonValues -LocalValue $localConfig -LiveValue $liveConfig -Path "" -Diffs $diffs

    if ($diffs.Count -eq 0) {
        Write-Host "Live config comparison passed: no non-sensitive differences found."
        return
    }

    Write-Warning "Live config comparison found $($diffs.Count) difference(s)."
    $lineNo = 0
    foreach ($diff in $diffs) {
        $lineNo++
        if ($lineNo -gt $MaxDiffLinesToPrint) {
            Write-Host "  ... (showing first $MaxDiffLinesToPrint of $($diffs.Count) differences)"
            break
        }

        $pathLabel = if ([string]::IsNullOrEmpty($diff.Path)) { "<root>" } else { $diff.Path }
        Write-Host ("  [{0}] {1} | local={2} | live={3}" -f `
            $diff.Kind, `
            $pathLabel, `
            (Format-CompareValue -Value $diff.Local), `
            (Format-CompareValue -Value $diff.Live))
    }

    if (-not (Confirm-Continue -Prompt "Differences detected. Continue with upload? [y/N]")) {
        throw "Upload cancelled by user after config diff review."
    }
}

$UploadType = if ($ota) { "ota" } else { "usb" }
$ProjectRoot = Split-Path -Parent $PSCommandPath
$DataDir = Join-Path $ProjectRoot "data"
$ConfigOutRoot = Join-Path $ProjectRoot "config.json"
$ConfigOutData = Join-Path $DataDir "config.json"
$IconOutData = Join-Path $DataDir "favicon.svg"

if (-not (Test-Path -LiteralPath $DataDir -PathType Container)) {
    New-Item -ItemType Directory -Path $DataDir | Out-Null
}

$profile = $DeviceProfiles[$Device]
$configSource = Join-Path $ProjectRoot $profile.config

Copy-FileChecked -SourcePath $configSource -DestinationPath $ConfigOutRoot -Label "config profile"
Assert-JsonValid -Path $ConfigOutRoot
Copy-FileChecked -SourcePath $configSource -DestinationPath $ConfigOutData -Label "config profile (data copy)"

$iconSourceName = $profile.icon
if ([string]::IsNullOrWhiteSpace($iconSourceName)) {
    if (Test-Path -LiteralPath $IconOutData -PathType Leaf) {
        Remove-Item -LiteralPath $IconOutData -Force
        Write-Host "Removed icon for '$Device': $IconOutData"
    }
    else {
        Write-Host "No icon configured for '$Device' and no existing icon to remove."
    }
}
else {
    $iconSource = Join-Path $ProjectRoot $iconSourceName
    Copy-FileChecked -SourcePath $iconSource -DestinationPath $IconOutData -Label "favicon profile"
}

$envName = $PioEnvByUploadType[$UploadType]
if ([string]::IsNullOrWhiteSpace($envName)) {
    throw "No PlatformIO environment configured for upload type '$UploadType'."
}

$uploadTarget = $DeviceTargets[$Device][$UploadType]
$pioArgs = @("run", "-e", $envName, "-t", "upload")
if (-not [string]::IsNullOrWhiteSpace($uploadTarget)) {
    $pioArgs += @("--upload-port", $uploadTarget)
}

if ($UploadType -eq "ota" -and -not $SkipLiveCompare -and -not $NoUpload) {
    if ([string]::IsNullOrWhiteSpace($uploadTarget)) {
        Write-Warning "OTA target host/IP is empty, cannot run live-config preflight compare."
        if (-not (Confirm-Continue -Prompt "Continue without live-config compare? [y/N]")) {
            throw "Upload cancelled by user."
        }
    }
    else {
        try {
            Invoke-LiveConfigCompare -ConfigPath $configSource -HostOrIp $uploadTarget
        }
        catch {
            Write-Warning "Live-config compare failed: $($_.Exception.Message)"
            if (-not (Confirm-Continue -Prompt "Continue with upload anyway? [y/N]")) {
                throw "Upload cancelled by user."
            }
        }
    }
}

Write-Host "Prepared profile '$Device' for '$UploadType' upload."
if ($NoUpload) {
    Write-Host "NoUpload set. Would run: pio $($pioArgs -join ' ')"
    exit 0
}

Write-Host "Running: pio $($pioArgs -join ' ')"
& pio @pioArgs
if ($LASTEXITCODE -ne 0) {
    throw "PlatformIO upload failed with exit code $LASTEXITCODE"
}

Write-Host "Upload completed successfully."
