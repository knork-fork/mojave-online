# Parses the GECK wiki XML export and generates lookup index files.
# Usage: powershell -ExecutionPolicy Bypass -File build_index.ps1

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$xmlPath = Join-Path $scriptDir 'GECK-20260301214412.xml'

Write-Host "Loading XML ($xmlPath)..."
[xml]$wiki = Get-Content $xmlPath -Encoding UTF8
$ns = @{ mw = 'http://www.mediawiki.org/xml/export-0.10/' }
$pages = Select-Xml -Xml $wiki -XPath '//mw:page' -Namespace $ns

$functions = @()
$nonFunctionPages = @()
$animGroupLines = @()

foreach ($pageNode in $pages) {
    $page = $pageNode.Node
    $title = $page.title
    $text = $page.revision.text.'#text'
    if (-not $text) { continue }

    # Extract categories
    $cats = @()
    $catMatches = [regex]::Matches($text, '\[\[Category:([^\]]+)\]\]')
    foreach ($m in $catMatches) {
        $cats += $m.Groups[1].Value.Trim()
    }

    # AnimGroups page - special handling
    if ($title -eq 'AnimGroups') {
        # Decode HTML entities and extract the pre block
        $decoded = $text -replace '&amp;', '&' -replace '&lt;', '<' -replace '&gt;', '>' -replace '&quot;', '"' -replace '&#09;', "`t"
        if ($decoded -match '(?s)<pre>(.*?)</pre>') {
            $animGroupLines += "# AnimGroups"
            $animGroupLines += "# Hex	Dec	Name"
            $animGroupLines += "# Note: some combat animations require actor to be in alerted state (weapon drawn, see SetAlert)"
            $animGroupLines += ""
            foreach ($line in $Matches[1].Trim().Split("`n")) {
                $line = $line.Trim()
                if ($line) { $animGroupLines += $line }
            }
        }
        continue
    }

    # Check for {{Function template
    # (?s) = dot matches newlines, (?m) = ^ matches start of line
    if ($text -match '(?sm)\{\{Function\b(.*?)^\}\}') {
        $funcBlock = $Matches[1]

        # Extract fields from the Function template
        $origin = ''
        $summary = ''
        $name = $title
        $returnType = ''
        $refType = ''
        $alias = ''
        $example = ''

        if ($funcBlock -match '\|origin\s*=\s*(.+)') { $origin = $Matches[1].Trim() }
        if ($funcBlock -match '(?s)\|summary\s*=\s*(.+?)(?=\n\s*\||\n\s*\{\{FunctionArgument)') {
            $summary = $Matches[1].Trim() -replace '\[\[([^\|\]]+)\|([^\]]+)\]\]', '$2' -replace '\[\[([^\]]+)\]\]', '$1' -replace "''", '' -replace '\s+', ' '
        }
        if ($funcBlock -match '\|name\s*=\s*(.+)') { $name = $Matches[1].Trim() }
        if ($funcBlock -match '\|returnType\s*=\s*(.+)') { $returnType = $Matches[1].Trim() }
        if ($funcBlock -match '\|referenceType\s*=\s*(.+)') { $refType = $Matches[1].Trim() }
        if ($funcBlock -match '\|alias\s*=\s*(.+)') { $alias = $Matches[1].Trim() }

        # Extract arguments
        $args = @()
        $argMatches = [regex]::Matches($funcBlock, '(?s)\{\{FunctionArgument(.*?)\}\}')
        foreach ($am in $argMatches) {
            $argBlock = $am.Groups[1].Value
            $argName = ''; $argType = ''; $argOpt = ''
            if ($argBlock -match '\|Name\s*=\s*(.+)') { $argName = $Matches[1].Trim() }
            if ($argBlock -match '\|Type\s*=\s*(.+)') { $argType = $Matches[1].Trim() }
            if ($argBlock -match '\|Optional\s*=\s*(.+)') { $argOpt = $Matches[1].Trim() }
            $argStr = "$argName`:$argType"
            if ($argOpt -eq 'y') { $argStr += '?' }
            $args += $argStr
        }

        $catStr = ($cats | Where-Object { $_ -notmatch '^Functions\s*$' }) -join '; '
        $argStr = $args -join ', '
        $aliasStr = if ($alias) { " (alias: $alias)" } else { '' }
        $refStr = if ($refType) { "$refType." } else { '' }
        $retStr = if ($returnType -and $returnType -ne 'void') { " -> $returnType" } else { '' }

        # Skip malformed entries (broken wiki markup)
        if ($name -match '^\|' -or $name -match '=' -or $name.Trim() -eq '') { continue }

        $functions += [PSCustomObject]@{
            Name = $name
            Origin = $origin
            Signature = "${refStr}${name}($argStr)${retStr}${aliasStr}"
            Summary = $summary
            Categories = $catStr
        }
    }
    else {
        # Non-function pages (guides, category pages, etc.) - store title + first meaningful line
        $stripped = $text -replace '\[\[Category:[^\]]+\]\]', '' -replace '\{\{[^\}]+\}\}', '' -replace '\[\[([^\|\]]+)\|([^\]]+)\]\]', '$2' -replace '\[\[([^\]]+)\]\]', '$1' -replace "'{2,}", '' -replace '<[^>]+>', ''
        $firstLine = ($stripped.Split("`n") | Where-Object { $_.Trim().Length -gt 10 } | Select-Object -First 1)
        if ($firstLine) {
            $firstLine = $firstLine.Trim().Substring(0, [Math]::Min(200, $firstLine.Trim().Length))
        }
        if ($firstLine -and $title -notmatch '^(Category:|Template:|File:|Talk:|User:)') {
            $nonFunctionPages += [PSCustomObject]@{
                Title = $title
                Summary = $firstLine
                Categories = ($cats -join '; ')
            }
        }
    }
}

# Sort and write function index
$functions = $functions | Sort-Object Name

$indexLines = @()
$indexLines += "# GECK Function Index - Auto-generated from GECK wiki XML"
$indexLines += "# Format: Name | Origin | Signature | Summary | Categories"
$indexLines += "# Search this file with grep to find relevant functions."
$indexLines += "# For full details, grep the original XML for the function name."
$indexLines += "#"
$indexLines += "# Origins: GECK1=base game, NVSE=NVSE plugin, JIP=JIP LN NVSE, JG=JohnnyGuitar,"
$indexLines += "#          SO=ShowOff NVSE, kNVSE=kNVSE, NX=NX, Hot Reload, etc."
$indexLines += ""

foreach ($f in $functions) {
    $indexLines += "$($f.Name) | $($f.Origin) | $($f.Signature) | $($f.Summary) | $($f.Categories)"
}

$indexPath = Join-Path $scriptDir 'geck_function_index.txt'
$indexLines | Out-File $indexPath -Encoding UTF8
Write-Host "Wrote $($functions.Count) functions to $indexPath"

# Write other pages index
if ($nonFunctionPages.Count -gt 0) {
    $otherLines = @()
    $otherLines += "# GECK Non-Function Pages Index"
    $otherLines += "# Format: Title | Summary | Categories"
    $otherLines += ""
    foreach ($p in ($nonFunctionPages | Sort-Object Title)) {
        $otherLines += "$($p.Title) | $($p.Summary) | $($p.Categories)"
    }
    $otherPath = Join-Path $scriptDir 'geck_other_pages_index.txt'
    $otherLines | Out-File $otherPath -Encoding UTF8
    Write-Host "Wrote $($nonFunctionPages.Count) other pages to $otherPath"
}

# Write anim groups
if ($animGroupLines.Count -gt 0) {
    $animPath = Join-Path $scriptDir 'geck_anim_groups.txt'
    $animGroupLines | Out-File $animPath -Encoding UTF8
    Write-Host "Wrote $($animGroupLines.Count) anim group lines to $animPath"
}

Write-Host "Done."
