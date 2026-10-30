# Creates the project's permanent Android signing key and stores it as the two
# repository secrets that service.yml signs the APKs with (see SIGNING.md).
# Run it ONCE. It refuses to overwrite a key in the folder, and refuses to
# replace secrets that already exist on GitHub: an APK signed with a new key
# does not install over one signed with the old key.
#
#   powershell -ExecutionPolicy Bypass -File service\android\new-signing-key.ps1 [-Dir <folder>] [-NoUpload]
#
# The folder gets otzaria-voice.p12 (the key) and password.txt. Keep a copy of
# both somewhere safe: without them no update can ever be published.
param(
    [string]$Dir = (Join-Path $env:USERPROFILE 'otzaria-voice-signing-key'),
    [string]$Repo = 'yakov100/otzaria-voice',
    [switch]$NoUpload
)
$ErrorActionPreference = 'Stop'

$keystore = Join-Path $Dir 'otzaria-voice.p12'
$passFile = Join-Path $Dir 'password.txt'
if ((Test-Path $keystore) -or (Test-Path $passFile)) {
    throw "A key already exists in $Dir. Never replace it; to upload it again, set the secrets by hand (SIGNING.md)."
}
$keytool = Get-Command keytool -ErrorAction SilentlyContinue
if (-not $keytool) { throw 'keytool not found: install Java (Temurin) first.' }

if (-not $NoUpload) {
    $existing = & gh secret list --repo $Repo --json name --jq '.[].name'
    if ($LASTEXITCODE) { throw "gh cannot list the secrets of $Repo (gh auth login?)" }
    if ($existing -contains 'ANDROID_KEYSTORE_BASE64') {
        throw "$Repo already has the secret ANDROID_KEYSTORE_BASE64: a key was made before. Do not replace it."
    }
}

New-Item -ItemType Directory -Force $Dir | Out-Null

# 32 random characters from a 64-character alphabet (no bias, safe on any command line).
$alphabet = [char[]]'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
$bytes = New-Object byte[] 32
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$password = -join ($bytes | ForEach-Object { $alphabet[$_ % 64] })

$env:OTZARIA_KEYSTORE_PASSWORD = $password
try {
    & $keytool.Path -genkeypair -keystore $keystore -storetype PKCS12 -storepass:env OTZARIA_KEYSTORE_PASSWORD `
        -alias otzaria-voice -keyalg RSA -keysize 4096 -validity 36500 -dname 'CN=Otzaria Voice, O=Otzaria'
    if ($LASTEXITCODE) { throw "keytool -genkeypair exited $LASTEXITCODE" }
    [IO.File]::WriteAllText($passFile, $password)
    $cer = Join-Path $env:TEMP "otzaria-voice-$PID.cer"
    & $keytool.Path -exportcert -keystore $keystore -storetype PKCS12 -storepass:env OTZARIA_KEYSTORE_PASSWORD `
        -alias otzaria-voice -file $cer | Out-Null
    if ($LASTEXITCODE) { throw "keytool -exportcert exited $LASTEXITCODE" }
    $fingerprint = (Get-FileHash $cer -Algorithm SHA256).Hash.ToLowerInvariant()
    Remove-Item $cer
} finally {
    Remove-Item Env:\OTZARIA_KEYSTORE_PASSWORD
}

# gh reads each secret from a file on its standard input, so that neither
# value appears on a command line.
function Set-Secret($name, $file) {
    $p = Start-Process gh -ArgumentList 'secret', 'set', $name, '--repo', $Repo `
        -RedirectStandardInput $file -NoNewWindow -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "gh secret set $name failed" }
}

if (-not $NoUpload) {
    $b64File = Join-Path $Dir 'keystore.b64'
    [IO.File]::WriteAllText($b64File, [Convert]::ToBase64String([IO.File]::ReadAllBytes($keystore)))
    try {
        Set-Secret ANDROID_KEYSTORE_BASE64 $b64File
        Set-Secret ANDROID_KEYSTORE_PASSWORD $passFile
    } finally {
        Remove-Item $b64File
    }
}

Write-Host ''
Write-Host "Key folder:  $Dir"
Write-Host "Certificate SHA-256: $fingerprint"
if ($NoUpload) { Write-Host 'Secrets were NOT uploaded (-NoUpload).' } else { Write-Host "Secrets ANDROID_KEYSTORE_BASE64 and ANDROID_KEYSTORE_PASSWORD set on $Repo." }
Write-Host 'Now copy the whole folder to two safe places (SIGNING.md).'
