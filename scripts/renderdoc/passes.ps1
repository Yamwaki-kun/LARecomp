param([string]$Dump)
$c = Get-Content $Dump
$names = @{}
foreach ($line in $c) {
    if ($line -match '^(ResourceId::\d+) \| ([^|]+) \| ([^|]+) \| (.+)$') {
        $names[$Matches[1]] = "$($Matches[2].Trim()) [$($Matches[3].Trim()) $($Matches[4].Trim())]"
    }
}
$i = ($c | Select-String '=== actions').LineNumber
$cur = $null; $first = 0; $last = 0; $n = 0
foreach ($line in $c[$i..($c.Count - 1)]) {
    if ($line -notmatch '^\s*(\d+) (\S+?)\(.*?(?: -> (.+))?$') { continue }
    $eid = [int]$Matches[1]; $fn = $Matches[2]; $outs = $Matches[3]
    if ($fn -notmatch 'Draw') { continue }
    if ($outs -ne $cur) {
        if ($cur) { "{0,6}-{1,-6} {2,5} draws  {3}" -f $first, $last, $n, (($cur -split ',' | % { $names[$_] }) -join ' + ') }
        $cur = $outs; $first = $eid; $n = 0
    }
    $last = $eid; $n++
}
if ($cur) { "{0,6}-{1,-6} {2,5} draws  {3}" -f $first, $last, $n, (($cur -split ',' | % { $names[$_] }) -join ' + ') }
