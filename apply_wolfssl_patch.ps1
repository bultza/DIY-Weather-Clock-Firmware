<#
.SYNOPSIS
  Patches the wolfSSL Arduino library's user_settings.h so the firmware on the
  feature_tls1.3 branch can do a TLS 1.3 handshake to wttr.in on an ESP8266.

.DESCRIPTION
  Applies four content-based, idempotent edits to user_settings.h:
    1. #define HAVE_SNI            (SNI for the wttr.in name-based vhost)
    2. #define WOLFSSL_NO_TLS12    (drop TLS 1.2 to save RAM/flash)
    3. comment out DEBUG_WOLFSSL   (its strings sit in DRAM on ESP8266)
    4. comment out WOLFSSL_HW_METRICS (static counters, ESP-IDF only)

  Matches by surrounding text (not line numbers), so it survives small version
  differences, and re-running it is safe (already-applied edits are skipped).

  Tested against the "wolfssl" Arduino library v5.8.2.

.PARAMETER UserSettings
  Full path to user_settings.h. Defaults to the standard Arduino sketchbook
  location; override it if your sketchbook lives elsewhere, e.g.:
    .\apply_wolfssl_patch.ps1 -UserSettings "D:\arduino\libraries\wolfssl\src\user_settings.h"
#>
param(
    [string]$UserSettings = "$env:USERPROFILE\Documents\Arduino\libraries\wolfssl\src\user_settings.h"
)

if (-not (Test-Path $UserSettings)) {
    Write-Error "user_settings.h not found at: $UserSettings`nInstall the 'wolfssl' library via the Arduino Library Manager, or pass -UserSettings <path>."
    exit 1
}

$text = [System.IO.File]::ReadAllText($UserSettings)
$nl   = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$applied = @()
$skipped = @()
$missing = @()

function Edit-Block($name, $from, $to) {
    if ($script:text.Contains($to)) { $script:skipped += $name; return }
    if ($script:text.Contains($from)) {
        $script:text = $script:text.Replace($from, $to)
        $script:applied += $name
    } else {
        $script:missing += $name
    }
}

# 1. HAVE_SNI  (the blank line between the two #defines is unique to the ESP8266 block)
Edit-Block "HAVE_SNI" `
    ("    #define HAVE_TLS_EXTENSIONS$nl$nl    #define HAVE_SUPPORTED_CURVES") `
    ("    #define HAVE_TLS_EXTENSIONS$nl$nl    /* Server Name Indication: needed so name-based vhosts (e.g. wttr.in)$nl     * route the request to the right certificate/site. */$nl    #define HAVE_SNI$nl$nl    #define HAVE_SUPPORTED_CURVES")

# 2. WOLFSSL_NO_TLS12
Edit-Block "WOLFSSL_NO_TLS12" `
    ("    #define HAVE_SUPPORTED_CURVES$nl$nl    #if defined(WOLFSSL_NO_TLS13) && defined(WOLFSSL_NO_TLS12)") `
    ("    #define HAVE_SUPPORTED_CURVES$nl$nl    /* wttr.in is TLS 1.3-only, so drop all TLS 1.2 code/data to save the$nl     * limited ESP8266 RAM (this alone helps the .bss fit in dram0_0_seg). */$nl    #define WOLFSSL_NO_TLS12$nl$nl    #if defined(WOLFSSL_NO_TLS13) && defined(WOLFSSL_NO_TLS12)")

# 3. Disable DEBUG_WOLFSSL (the active one, identified by the following comment line)
Edit-Block "DEBUG_WOLFSSL off" `
    ("#define DEBUG_WOLFSSL$nl/* Debug options:") `
    ("/* #define DEBUG_WOLFSSL */  /* OFF: debug strings live in DRAM on ESP8266 */$nl/* Debug options:")

# 4. Disable WOLFSSL_HW_METRICS
Edit-Block "WOLFSSL_HW_METRICS off" `
    ("$nl#define WOLFSSL_HW_METRICS$nl") `
    ("$nl/* #define WOLFSSL_HW_METRICS */  /* OFF: static metric counters, ESP-IDF only */$nl")

if ($applied.Count -gt 0) {
    [System.IO.File]::WriteAllText($UserSettings, $text)
}

Write-Output "wolfSSL user_settings.h patch ($UserSettings):"
if ($applied.Count) { Write-Output ("  applied : " + ($applied -join ", ")) }
if ($skipped.Count) { Write-Output ("  already : " + ($skipped -join ", ")) }
if ($missing.Count) {
    Write-Warning ("anchors not found (apply by hand, see README): " + ($missing -join ", "))
    exit 2
}
Write-Output "Done. Recompile the sketch."
