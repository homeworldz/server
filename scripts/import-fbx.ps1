# Import a source file (.fbx) by uploading it, which is all an import is
# (ADR 0035): the file is stored untouched and the region converts it into one
# object per mesh in your inventory.
#
# Firestorm cannot do this — its model uploader speaks Collada and posts to the
# viewer's own capability — so until the first-party client carries a button,
# this is the way in. Three requests, which is what any client does:
#
#   1. POST <api>/v1/tokens          userid + password  -> account token
#   2. POST <api>/v1/client/session  account token      -> region endpoint + ticket
#   3. POST <region>/session/uploads/mesh   the ticket + the file bytes
#
# The ticket is the only credential a region ever sees, it is short-lived
# (default 300 s), and it is minted per region — so this cannot be done with a
# stored secret, and the password is prompted for rather than passed as an
# argument, never echoed and never written anywhere.
#
#   ./scripts/import-fbx.ps1 -File 'C:\dev\homeworldz\mesh\CC\Outfits_Caleb.Fbx' -Userid jim.tarber
#
# Name defaults to the file's base name. The response carries the asset, object
# and inventory item ids of the *first* published part; the rest arrive in the
# same inventory folder.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$File,
    [Parameter(Mandatory = $true)][string]$Userid,
    [string]$Api = 'https://api.homeworldz.com',
    # "last" (the default), "home", or a region name with optional coordinates.
    # The region resolved here is the one that converts the upload.
    [string]$Start = 'last',
    [string]$Name,
    # An import is unbounded CPU on a large file and the region answers when it
    # is done, so this waits far longer than an API call normally would.
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $File)) { throw "no such file: $File" }
$item = Get-Item -LiteralPath $File
if (-not $Name) { $Name = $item.BaseName }

$password = Read-Host -Prompt "Password for $Userid" -AsSecureString
$plain = [System.Net.NetworkCredential]::new('', $password).Password
try {
    $token = (Invoke-RestMethod -Method Post -Uri "$Api/v1/tokens" -ContentType 'application/json' `
        -Body (@{ userid = $Userid; password = $plain } | ConvertTo-Json)).accessToken
} finally {
    $plain = $null
    [System.GC]::Collect()
}
if (-not $token) { throw 'no access token in the response' }

$session = Invoke-RestMethod -Method Post -Uri "$Api/v1/client/session" `
    -Headers @{ Authorization = "Bearer $token" } -ContentType 'application/json' `
    -Body (@{ start = $Start } | ConvertTo-Json)
$region = $session.region.endpoint
if (-not $region) { throw 'the session response named no region endpoint' }
"region:  $($session.region.name) at $region"
"file:    $($item.Name), $([math]::Round($item.Length / 1MB, 1)) MiB, as `"$Name`""

# -InFile streams the bytes rather than materialising a body string, which
# matters: these files run to tens of megabytes.
try {
    $response = Invoke-RestMethod -Method Post -Uri "$region/session/uploads/mesh" `
        -Headers @{ Authorization = "Bearer $($session.ticket.token)"; 'X-Homeworldz-Name' = $Name } `
        -ContentType 'application/octet-stream' -InFile $item.FullName `
        -TimeoutSec $TimeoutSeconds
} catch {
    # The region reports a refusal as JSON with a reason worth reading, and
    # Invoke-RestMethod hides it behind the status line.
    $detail = $_.ErrorDetails.Message
    if (-not $detail -and $_.Exception.Response) {
        $stream = $_.Exception.Response.GetResponseStream()
        $detail = (New-Object System.IO.StreamReader($stream)).ReadToEnd()
    }
    if ($detail) { throw "upload refused: $detail" }
    throw
}
$response | ConvertTo-Json -Depth 5
