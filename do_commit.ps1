Set-Location 'c:\Arduino\crySense_ai'

# Garante .gitignore com secrets.h
$gitignore = '.gitignore'
if (-not (Test-Path $gitignore)) {
    "crySence/secrets.h" | Out-File -Encoding utf8 $gitignore
} elseif (-not (Select-String -Path $gitignore -Pattern 'secrets\.h' -Quiet)) {
    Add-Content $gitignore "`ncrySence/secrets.h"
}

git add -A

$msg = "fix: corrige web server, falsos positivos de choro e API de confianca

- web_server.h: substitui LittleFS por SPIFFS; remove include duplicado de
  ArduinoJson; elimina conflito de particao que impedia o servidor web de subir
- audio_player.h: reduz GAIN_MULTIPLIER 5.0->2.5; threshold de silencio
  0.015->0.025; silencio bypassa a IA e injeta noise direto na fila
- crySence.ino: gWebState.confianca em 0-1 (evita dupla multiplicacao x100)

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"

git commit -m $msg
git log --oneline -5
