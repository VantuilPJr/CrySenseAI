$content = Get-Content 'c:\Arduino\crySense_ai\crySence\data\assets\index-CmK3eNMi.js' -Raw
$keywords = @('conforto_ok', 'conforto', 'pressao', 'pres"', 'n.pres', 'Pressao', 'conforto_ok', 'PSRAM')

foreach ($kw in $keywords) {
    $idx = 0
    $found = 0
    while ($true) {
        $idx = $content.IndexOf($kw, $idx)
        if ($idx -lt 0) { break }
        $found++
        if ($found -le 3) {
            Write-Host "=== '$kw' at $idx ==="
            $start = [Math]::Max(0, $idx - 100)
            $len   = [Math]::Min(400, $content.Length - $start)
            Write-Host $content.Substring($start, $len)
            Write-Host ""
        }
        $idx++
    }
    if ($found -eq 0) { Write-Host "NOT FOUND: $kw" }
}
