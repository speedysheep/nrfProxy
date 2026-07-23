# Structural hygiene checks for Task 7 (no compiler required).
# Exit 0 if invariants hold.

$ErrorActionPreference = "Stop"
$main = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "src\main.c"
$text = Get-Content $main -Raw
$failures = 0

function Fail($msg) {
    Write-Host "FAIL $msg"
    $script:failures++
}
function Ok($msg) { Write-Host "ok   $msg" }

# bt_set_name must appear after settings_load in main()
$setName = $text.IndexOf("bt_set_name(device_name)")
$settings = $text.IndexOf("settings_load()")
if ($setName -lt 0 -or $settings -lt 0) {
    Fail "bt_set_name or settings_load not found"
} elseif ($setName -lt $settings) {
    Fail "bt_set_name appears before settings_load"
} else {
    Ok "bt_set_name after settings_load"
}

if ($text -match 'adv_blink_on\s*=\s*false') {
    Ok "adv_blink_on cleared on advertising entry"
} else {
    Fail "adv_blink_on not cleared when entering STATUS_ADVERTISING"
}

if ($text -match 'Mutex exemption') {
    Ok "locked_mode mutex exemption comment present"
} else {
    Fail "locked_mode mutex exemption comment missing"
}

if ($text -match 'Deliberately only the RX ring') {
    Ok "TX ring disconnect asymmetry documented"
} else {
    Fail "TX ring disconnect asymmetry comment missing"
}

if ($failures -gt 0) {
    Write-Host "$failures failure(s)"
    exit 1
}
Write-Host "all passed"
exit 0
