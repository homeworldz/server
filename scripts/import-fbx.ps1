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

# HttpWebRequest rather than Invoke-RestMethod, for three reasons that all
# matter at 64 MiB:
#
#   - AllowWriteStreamBuffering = $false streams from disk instead of building
#     the whole body in memory before the first byte goes out.
#   - The region answers a *source* upload only when the import finishes, so the
#     read timeout has to be minutes, separately from the send.
#   - Invoke-RestMethod turns any non-2xx into a terminating error and drops the
#     body, which is where the region puts the reason. Here the body is read on
#     both paths, so a refusal arrives as the sentence it was written as.
#
# Expect: 100-continue is turned off because it buys nothing here and adds a
# round trip the region has no reason to answer.
[System.Net.ServicePointManager]::Expect100Continue = $false
$request = [System.Net.HttpWebRequest]::CreateHttp("$region/session/uploads/mesh")
$request.Method = 'POST'
$request.ContentType = 'application/octet-stream'
$request.Headers.Add('Authorization', "Bearer $($session.ticket.token)")
$request.Headers.Add('X-Homeworldz-Name', $Name)
$request.AllowWriteStreamBuffering = $false
$request.SendChunked = $false
$request.ContentLength = $item.Length
$request.Timeout = $TimeoutSeconds * 1000
$request.ReadWriteTimeout = $TimeoutSeconds * 1000

$source = [System.IO.File]::OpenRead($item.FullName)
$sink = $request.GetRequestStream()
try { $source.CopyTo($sink, 1MB) } finally { $sink.Dispose(); $source.Dispose() }
"sent:    $($item.Length) bytes; waiting for the import (up to $TimeoutSeconds s)"

$read = {
    param($web)
    $reader = New-Object System.IO.StreamReader($web.GetResponseStream())
    try { $reader.ReadToEnd() } finally { $reader.Dispose() }
}
try {
    $web = $request.GetResponse()
    try { "status:  $([int]$web.StatusCode)"; & $read $web } finally { $web.Dispose() }
} catch [System.Net.WebException] {
    if ($_.Exception.Response) {
        $web = $_.Exception.Response
        "status:  $([int]$web.StatusCode)"
        & $read $web
        $web.Dispose()
    } else {
        # No response at all: the connection went away mid-request. Worth
        # distinguishing, because it is not a refusal and the file may well have
        # been stored — the region logs what it received.
        throw "the region closed the connection without answering ($($_.Exception.Status)): $($_.Exception.Message)"
    }
}
