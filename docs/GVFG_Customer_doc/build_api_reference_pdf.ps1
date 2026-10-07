param(
    [Parameter(Position = 0)]
    [string]$InputMarkdown,

    [Parameter(Position = 1)]
    [string]$OutputPdf,

    [Parameter(Position = 2)]
    [string]$TemplateDocx
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($InputMarkdown)) {
    $InputMarkdown = Join-Path $scriptDir 'GVFG_CUSTOMER_API_REFERENCE.md'
}
if ([string]::IsNullOrWhiteSpace($OutputPdf)) {
    $OutputPdf = [IO.Path]::ChangeExtension($InputMarkdown, '.pdf')
}
if ([string]::IsNullOrWhiteSpace($TemplateDocx)) {
    $TemplateDocx = Join-Path $scriptDir 'GVFG_CUSTOMER_API_REFERENCE.docx'
}

$InputMarkdown = [IO.Path]::GetFullPath($InputMarkdown)
$OutputPdf = [IO.Path]::GetFullPath($OutputPdf)
$TemplateDocx = [IO.Path]::GetFullPath($TemplateDocx)

if (-not (Test-Path -LiteralPath $InputMarkdown -PathType Leaf)) {
    throw "Markdown file not found: $InputMarkdown"
}
if (-not (Test-Path -LiteralPath $TemplateDocx -PathType Leaf)) {
    throw "Branding template not found: $TemplateDocx"
}
if (-not (Get-Command ConvertFrom-Markdown -ErrorAction SilentlyContinue)) {
    throw 'ConvertFrom-Markdown is unavailable. Run this script with PowerShell 7.'
}

$edgeCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe'),
    (Join-Path $env:ProgramFiles 'Microsoft\Edge\Application\msedge.exe')
) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) }
if (-not $edgeCandidates) {
    throw 'Microsoft Edge was not found.'
}
$edge = @($edgeCandidates)[0]

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-DocxAssetBase64 {
    param([string]$DocxPath, [string]$EntryName)
    $archive = [IO.Compression.ZipFile]::OpenRead($DocxPath)
    try {
        $entry = $archive.GetEntry($EntryName)
        if (-not $entry) { throw "Template asset not found: $EntryName" }
        $input = $entry.Open()
        try {
            $memory = [IO.MemoryStream]::new()
            try {
                $input.CopyTo($memory)
                return [Convert]::ToBase64String($memory.ToArray())
            } finally { $memory.Dispose() }
        } finally { $input.Dispose() }
    } finally { $archive.Dispose() }
}

$bannerBase64 = Get-DocxAssetBase64 $TemplateDocx 'word/media/image1.jpeg'

$markdown = Get-Content -LiteralPath $InputMarkdown -Raw -Encoding UTF8
$markdown = $markdown -replace '(?m)^\[TOC\]\s*$', ''
$markdown = $markdown -replace '(?m)^===\s*$', ''
$bodyHtml = (ConvertFrom-Markdown -InputObject $markdown).Html
$titleMatch = [regex]::Match($bodyHtml, '(?s)^\s*<h1[^>]*>(.*?)</h1>\s*')
if (-not $titleMatch.Success) {
    throw 'The Markdown document must start with a level-1 heading (# Title).'
}
$documentTitleHtml = $titleMatch.Groups[1].Value
$documentTitleText = [Net.WebUtility]::HtmlDecode(
    ($documentTitleHtml -replace '<[^>]+>', '')).Trim()
$documentTitle = [Net.WebUtility]::HtmlEncode($documentTitleText)
$bodyHtml = $bodyHtml.Substring($titleMatch.Length)

$tocItems = [Collections.Generic.List[string]]::new()
$headingIndex = 0
$headingPattern = [regex]'<h([23])\s+id="[^"]*">(.*?)</h\1>'
$bodyHtml = $headingPattern.Replace($bodyHtml, {
    param($match)
    $script:headingIndex++
    $level = [int]$match.Groups[1].Value
    $inner = $match.Groups[2].Value
    $plain = [Net.WebUtility]::HtmlDecode(($inner -replace '<[^>]+>', '')).Trim()
    $id = "section-$script:headingIndex"
    $encodedText = [Net.WebUtility]::HtmlEncode($plain)
    $tocItems.Add("<li class=`"toc-$level`"><a href=`"#$id`">$encodedText</a></li>")
    return "<h$level id=`"$id`">$inner</h$level>"
})
$tocHtml = $tocItems -join [Environment]::NewLine

$css = @'
@page { size: A4; margin: 0 0 17mm; }
* { box-sizing: border-box; }
body { margin: 0; color: #172033; font-family: "Microsoft JhengHei", "Segoe UI", sans-serif; font-size: 9.4pt; line-height: 1.48; }
.page-layout { width: 100%; border: 0; border-collapse: collapse; margin: 0; padding: 0; }
.page-layout > thead { display: table-header-group; }
.page-layout > thead > tr.banner-row > td { width: 100%; height: 25.03mm; padding: 0; border: 0; background: #000; }
.page-layout > thead > tr.header-spacer > td { width: 100%; height: 6mm; padding: 0; border: 0; background: #fff; }
.page-layout > thead img { display: block; width: 210mm; height: 25.03mm; object-fit: fill; }
.page-layout > tbody > tr > td { padding: 0 15mm; border: 0; vertical-align: top; }
.content { position: relative; z-index: 1; }
h1 { color: #0b3a69; font-size: 25pt; margin: 0 0 16pt; padding-bottom: 8pt; border-bottom: 3px solid #2e75b6; }
h2 { color: #0b3a69; font-size: 17pt; margin: 20pt 0 8pt; padding-bottom: 4pt; border-bottom: 1px solid #9fbad2; break-after: avoid; }
h3 { color: #174f7c; font-size: 12.5pt; margin: 15pt 0 6pt; break-after: avoid; }
p { margin: 4pt 0 8pt; }
ul, ol { margin: 4pt 0 8pt 20pt; padding: 0; }
li { margin: 2pt 0; }
code { font-family: Consolas, "Courier New", monospace; font-size: 8.6pt; color: #7a1f45; background: #f3f5f7; padding: .5pt 2.5pt; border-radius: 2px; }
pre { background: #f3f5f7; border-left: 3px solid #2e75b6; padding: 7pt 9pt; white-space: pre-wrap; overflow-wrap: anywhere; break-inside: avoid; }
pre code { color: #172033; background: transparent; padding: 0; }
.content table { width: 100%; border-collapse: collapse; margin: 6pt 0 11pt; font-size: 8.2pt; break-inside: auto; }
.content thead { display: table-header-group; }
.content tr { break-inside: avoid; }
.content th { color: white; background: #2e75b6; text-align: left; font-weight: 600; }
.content th, .content td { border: 1px solid #b7c7d6; padding: 4pt 5pt; vertical-align: top; overflow-wrap: anywhere; }
.content tbody tr:nth-child(even) { background: rgba(245,248,251,.92); }
a { color: #155b91; text-decoration: none; }
.toc { columns: 2; column-gap: 22pt; list-style: none; margin: 0 0 16pt; padding: 10pt 12pt; background: rgba(244,247,250,.92); border: 1px solid #c8d5e1; }
.toc li { break-inside: avoid; margin: 2pt 0; }
.toc-3 { padding-left: 10pt; font-size: 8.7pt; }
.toc-page { break-after: page; page-break-after: always; }
'@

$html = @"
<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<title>$documentTitle</title><style>$css</style></head><body>
<table class="page-layout"><thead><tr class="banner-row"><td><img src="data:image/jpeg;base64,$bannerBase64"></td></tr><tr class="header-spacer"><td></td></tr></thead>
<tbody><tr><td><main class="content"><section class="toc-page"><h1>$documentTitleHtml</h1>
<h2>目錄</h2><ul class="toc">$tocHtml</ul></section>$bodyHtml</main></td></tr></tbody></table>
</body></html>
"@

$outputDirectory = Split-Path -Parent $OutputPdf
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory | Out-Null
}
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("gvfg-md-pdf-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
$htmlPath = Join-Path $temporaryRoot 'reference.html'
$temporaryPdf = Join-Path $temporaryRoot 'reference.pdf'
try {
    [IO.File]::WriteAllText($htmlPath, $html, [Text.UTF8Encoding]::new($false))
    $uri = [Uri]::new($htmlPath).AbsoluteUri
    $arguments = @('--headless', '--disable-gpu', '--no-pdf-header-footer', "--print-to-pdf=$temporaryPdf", $uri)
    Start-Process -FilePath $edge -ArgumentList $arguments -Wait -WindowStyle Hidden | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath $temporaryPdf -PathType Leaf) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }
    if (-not (Test-Path -LiteralPath $temporaryPdf -PathType Leaf)) {
        throw 'Microsoft Edge did not create the PDF.'
    }
    if ((Get-Item -LiteralPath $temporaryPdf).Length -lt 10000) {
        throw 'The generated PDF is unexpectedly small.'
    }
    Copy-Item -LiteralPath $temporaryPdf -Destination $OutputPdf -Force
    Write-Host "PDF created: $OutputPdf" -ForegroundColor Green
} finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
