$content = Get-Content 'c:\Arduino\crySense_ai\crySence\data\assets\index-CmK3eNMi.js' -Raw
foreach ($kw in @('conforto_ok', 'inf_ms', 'pres')) {
    $idx = $content.IndexOf($kw)
    if ($idx -ge 0) {
        Write-Host "=== $kw (pos $idx) ==="
        $start = [Math]::Max(0, $idx - 150)
        $len = [Math]::Min(500, $content.Length - $start)
        Write-Host $content.Substring($start, $len)
        Write-Host ""
    }
}
