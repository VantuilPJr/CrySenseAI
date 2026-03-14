Set-Location 'c:\Arduino\crySense_ai'
git add -A
$msg = "fix: reverte gain, corrige inf_ms e pressure/conforto no dashboard

- audio_player.h: reverte GAIN_MULTIPLIER 2.5->5.0; o modelo foi treinado com
  ganho 5.0 e reduzi-lo fazia a IA ver colica como fome (sinal mais fraco = padrao
  de fome). O gate de silencio (ja corrigido) e suficiente para evitar falsos positivos
- crySence.ino: gWebState.inf_ms = ir.infMs agora e atualizado no TaskIA; era 0 sempre
- index-CmK3eNMi.js: Pressao e Conforto estavam hardcoded como '--' no bundle
  compilado; agora leem e.pres e e.conforto_ok da API

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
git commit -m $msg
git log --oneline -3
