$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repo '.rivet\acceptance\windows'
$fixture = Join-Path $repo 'racket\tests\fixtures\sample.epub'
$executable = Get-ChildItem -LiteralPath (Join-Path $repo 'dist') `
    -Filter RivetHost.exe -File -Recurse | Select-Object -First 1
if (-not $executable) { throw 'packaged Booklight executable was not found' }
if (-not (Test-Path -LiteralPath $fixture)) { throw 'sample EPUB fixture was not found' }
$fixtureHashBefore = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
# A second synthetic file exercises two independent original-path EPUB tabs.
# This test fixture copy never enters Booklight's storage or a managed catalog.
New-Item -ItemType Directory -Force -Path $output | Out-Null
$fixtureSecond = Join-Path $output "sample-second-$PID.epub"
Copy-Item -LiteralPath $fixture -Destination $fixtureSecond -Force
$fixtureSecondHashBefore = (Get-FileHash -LiteralPath $fixtureSecond -Algorithm SHA256).Hash
$appData = Join-Path $output "appdata-$PID"
$localData = Join-Path $output "local-$PID"
New-Item -ItemType Directory -Force -Path $output,$appData,$localData | Out-Null

function Wait-Until {
    param([string]$Description, [scriptblock]$Probe, [int]$TimeoutSeconds = 90)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $value = & $Probe
        if ($null -ne $value -and $value -ne $false) { return $value }
        Start-Sleep -Milliseconds 100
    }
    throw "timed out waiting for $Description"
}

function Contains-Text {
    param($Root, [string]$Fragment)
    $nodes = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    for ($index = 0; $index -lt $nodes.Count; $index++) {
        if ($nodes[$index].Current.Name -like "*$Fragment*") { return $true }
    }
    return $false
}

function Invoke-NamedControl {
    param($Root, [string[]]$Names)
    $nodes = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    for ($index = 0; $index -lt $nodes.Count; $index++) {
        $node = $nodes[$index]
        if ($Names -notcontains $node.Current.Name) { continue }
        try {
            $pattern = $node.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
            ([System.Windows.Automation.InvokePattern]$pattern).Invoke()
            return $true
        } catch {
            try {
                $pattern = $node.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
                ([System.Windows.Automation.TogglePattern]$pattern).Toggle()
                return $true
            } catch { }
        }
    }
    return $false
}

function Get-EpubTabs {
    param($Root)
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::TabItem)
    $items = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
    $tabs = @()
    for ($index = 0; $index -lt $items.Count; $index++) {
        # A WinUI TabViewItem with a custom header TextBlock exposes an empty
        # automation name of its own; the parsed title lives on a descendant
        # text element. Match the tab by its subtree, not by its own name.
        if (Contains-Text $items[$index] 'The Sample Book') {
            $tabs += $items[$index]
        }
    }
    return $tabs
}

function Select-EpubTab {
    param($Tab)
    $pattern = $Tab.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
    ([System.Windows.Automation.SelectionItemPattern]$pattern).Select()
    if (-not ([System.Windows.Automation.SelectionItemPattern]$pattern).Current.IsSelected) {
        throw 'native EPUB tab selection did not change'
    }
}

function Save-Snapshot {
    param($Root, [string]$Path)
    $nodes = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    $snapshot = for ($index = 0; $index -lt [Math]::Min($nodes.Count, 800); $index++) {
        $node = $nodes[$index]
        [ordered]@{ name = $node.Current.Name; controlType = $node.Current.ControlType.ProgrammaticName; automationId = $node.Current.AutomationId }
    }
    ConvertTo-Json -InputObject @($snapshot) -Depth 4 |
        Set-Content -LiteralPath $Path -Encoding utf8
}

$process = $null
$savedAppData = $env:APPDATA
$savedLocalData = $env:LOCALAPPDATA
$savedOpen = $env:BOOKLIGHT_OPEN
try {
    $env:APPDATA = $appData
    $env:LOCALAPPDATA = $localData
    $env:BOOKLIGHT_OPEN = "$fixture`n$fixtureSecond"
    $configDir = Join-Path $appData 'site.jrtx.booklight'
    & racket (Join-Path $repo 'scripts\acceptance\seed-annotation-recovery.rkt') $configDir $fixture
    if ($LASTEXITCODE -ne 0) { throw 'failed to seed annotation recovery fixture' }
    $process = Start-Process -FilePath $executable.FullName -PassThru `
        -WorkingDirectory $repo `
        -RedirectStandardOutput (Join-Path $output 'application.stdout.log') `
        -RedirectStandardError (Join-Path $output 'application.stderr.log')
    $desktop = [System.Windows.Automation.AutomationElement]::RootElement
    $window = Wait-Until 'Booklight window' {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $desktop.FindFirst([System.Windows.Automation.TreeScope]::Children, $condition)
    }
    try {
        $bookTabs = Wait-Until 'two native EPUB tabs with parsed titles' {
            $found = @(Get-EpubTabs $window)
            if ($found.Count -eq 2) { return ,$found }
            return $null
        } 150
        if (@($bookTabs).Count -ne 2) { throw 'expected exactly two EPUB tabs' }
        # The second launch path becomes active; switch to the first original
        # before checking its recovery journal, then round-trip both tabs.
        Select-EpubTab $bookTabs[0]
        [void](Wait-Until 'EPUB title parsed by the embedded backend' { Contains-Text $window 'The Sample Book' })
        [void](Wait-Until 'EPUB table of contents parsed by the embedded backend' { Contains-Text $window 'First Chapter' })
        [void](Wait-Until 'native annotations control' { Invoke-NamedControl $window @('Annotations','批注') })
        [void](Wait-Until 'recovered annotation status' {
            (Contains-Text $window 'Recovered unsaved annotations') -or
            (Contains-Text $window '已恢复尚未写入目标位置的批注')
        })
        [void](Wait-Until 'retry action' {
            (Contains-Text $window 'Retry Save') -or (Contains-Text $window '重试保存')
        })
        [void](Wait-Until 'export action' {
            (Contains-Text $window 'Export Annotations') -or (Contains-Text $window '导出批注')
        })
        [void](Wait-Until 'recovery fixture note' { Contains-Text $window 'Recovered fixture note' })
        Select-EpubTab $bookTabs[1]
        [void](Wait-Until 'second EPUB still has its own native chapter navigation' {
            Contains-Text $window 'First Chapter'
        })
        Select-EpubTab $bookTabs[0]
        [void](Wait-Until 'first EPUB recovery status survives tab switching' {
            (Contains-Text $window 'Recovered fixture note') -and
            ((Contains-Text $window 'Retry Save') -or (Contains-Text $window '重试保存'))
        })
        if (@(Get-EpubTabs $window).Count -ne 2) {
            throw 'an EPUB tab disappeared after switching'
        }
    } catch {
        Save-Snapshot $window (Join-Path $output 'accessibility-failure.json')
        throw
    }
    Save-Snapshot $window (Join-Path $output 'accessibility.json')
    # File-first: opening cannot write library.json/covers or alter the EPUB.
    if (Test-Path -LiteralPath (Join-Path $configDir 'library.json')) { throw 'unexpected library.json' }
    if (Test-Path -LiteralPath (Join-Path $configDir 'covers')) { throw 'unexpected covers cache' }
    if ($fixtureHashBefore -ne (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash) { throw 'original EPUB was modified' }
    if ($fixtureSecondHashBefore -ne (Get-FileHash -LiteralPath $fixtureSecond -Algorithm SHA256).Hash) { throw 'second EPUB was modified' }
    [ordered]@{
        platform = 'windows'
        artifact = $executable.FullName
        actions = @('launch:two-original-epub-paths','switch:first-epub','press:Annotations','switch:second-epub','switch:first-epub')
        assertions = @('packaged WinUI window activated','sample EPUB crossed RVT1 into the embedded backend','native accessibility tree contains The Sample Book','native accessibility tree contains First Chapter','pending recovery crosses RVT1 on reopen','native Notes pane exposes recovered status and Retry Save','recovered annotation remains visible and exportable','two distinct native EPUB tabs persist across switches','first EPUB recovery state is isolated','both source EPUB hashes remain unchanged')
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'interaction-trace.json') -Encoding utf8

    $pattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    ([System.Windows.Automation.WindowPattern]$pattern).Close()
    [void](Wait-Until 'Booklight process exit after native window close' { $process.Refresh(); if ($process.HasExited) { return $true }; return $false } 30)
} finally {
    $env:APPDATA = $savedAppData
    $env:LOCALAPPDATA = $savedLocalData
    $env:BOOKLIGHT_OPEN = $savedOpen
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    if ($process) { $process.Dispose() }
}
