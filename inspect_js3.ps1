$content = Get-Content 'c:\Arduino\crySense_ai\crySence\data\assets\index-CmK3eNMi.js' -Raw
foreach ($kw in @('hPa', 'Conforto', 'Temperatura', 'Umidade', 'n.temp', 'n.umid', 'n.pres', 'n.conforto')) {
    $idx = $content.IndexOf($kw)
    if ($idx -ge 0) {
        Write-Host "=== '$kw' at $idx ==="
        $start = [Math]::Max(0, $idx - 200)
        $len   = [Math]::Min(600, $content.Length - $start)
        Write-Host $content.Substring($start, $len)
        Write-Host ""
    } else {
        Write-Host "NOT FOUND: $kw"
    }
}
