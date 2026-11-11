$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NativeIconProbe {
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg,
        IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "PrivateExtractIconsW")]
    public static extern uint PrivateExtractIcons(string fileName, int iconIndex,
        int width, int height, IntPtr[] phicon, int[] piconid, int nIcons, int[] icons);
    [DllImport("user32.dll")]
    public static extern bool DestroyIcon(IntPtr hIcon);
    public const uint WM_GETICON = 0x007F;
    public const uint SMTO_ABORTIFHUNG = 0x0002;
}
"@

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

$script:journeySteps = @()
function Record-Step {
    # The CI step log floods with app noise, so each milestone lands in a
    # small file that the acceptance artifact preserves verbatim.
    param([string]$Name)
    $script:journeySteps += "$([DateTime]::UtcNow.ToString('HH:mm:ss')) $Name"
    try { ConvertTo-Json -InputObject $script:journeySteps | Set-Content -LiteralPath (Join-Path $output 'journey-steps.json') -Encoding utf8 } catch { }
}

function Contains-Text {
    param($Root, [string]$Fragment)
    try {
        $nodes = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.Condition]::TrueCondition)
        for ($index = 0; $index -lt $nodes.Count; $index++) {
            if ($nodes[$index].Current.Name -like "*$Fragment*") { return $true }
        }
        return $false
    } catch {
        # Window transitions surface transient UIA faults; the callers poll,
        # so treat a fault as "not present right now".
        return $false
    }
}

function Process-Windows {
    param([int]$ProcessId)
    $desktop = [System.Windows.Automation.AutomationElement]::RootElement
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcessId)
    return @($desktop.FindAll([System.Windows.Automation.TreeScope]::Children, $condition))
}

function Invoke-NamedControl {
    param($Root, [string[]]$Names)
    try { return Invoke-NamedControlInner $Root $Names } catch { return $false }
}
function Invoke-NamedControlInner {
    param($Root, [string[]]$Names)
    $nodes = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    for ($index = 0; $index -lt $nodes.Count; $index++) {
        $node = $nodes[$index]
        # A single stale element in the tree must not abort the scan —
        # read its name defensively or the real target later in the
        # enumeration becomes unreachable.
        try {
            $name = $node.Current.Name
        } catch { continue }
        if ($Names -notcontains $name) { continue }
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

function Get-IconPixelDigest {
    param($Icon)
    $bitmap = $Icon.ToBitmap()
    try {
        $rect = [System.Drawing.Rectangle]::new(0, 0, $bitmap.Width, $bitmap.Height)
        $data = $bitmap.LockBits($rect,
            [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $bytes = New-Object byte[] ($data.Stride * $data.Height)
            [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try { return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '') }
            finally { $sha.Dispose() }
        } finally { $bitmap.UnlockBits($data) }
    } finally { $bitmap.Dispose() }
}

function Test-BrandedTitlebarIcon {
    # The packaged exe embeds the project icon as RT_GROUP_ICON resource
    # ID 1 and the WinUI host applies it through AppWindow.SetIcon, so the
    # titlebar icon must be pixel-identical to the exe's own resource at
    # whatever size the shell requested. A build without the resource keeps
    # the placeholder (host fallback); this packaged acceptance must not
    # pass in that state.
    $hwnd = [IntPtr]$window.Current.NativeWindowHandle
    $found = @{}
    foreach ($kind in 1, 2, 3) {  # ICON_BIG, ICON_SMALL, ICON_SMALL2
        $result = [IntPtr]::Zero
        [void][NativeIconProbe]::SendMessageTimeout($hwnd,
            [NativeIconProbe]::WM_GETICON, [IntPtr]$kind, [IntPtr]::Zero,
            [NativeIconProbe]::SMTO_ABORTIFHUNG, 2000, [ref]$result)
        if ($result -ne [IntPtr]::Zero) { $found[$kind] = $result }
    }
    if ($found.Count -eq 0) {
        throw 'titlebar icon absent: WM_GETICON returned nothing (RT_GROUP_ICON resource ID 1 missing or AppWindow.SetIcon not applied)'
    }
    # Reference renderings of the exe's own group icon at the two shell
    # sizes; the titlebar handles must reproduce one of these exactly.
    $referenceDigests = @{}
    foreach ($size in 16, 32) {
        $exeIcons = New-Object 'IntPtr[]' 1
        $ids = New-Object 'int[]' 1
        $sizes = New-Object 'int[]' 1
        $extracted = [NativeIconProbe]::PrivateExtractIcons(
            $executable.FullName, 0, $size, $size, $exeIcons, $ids, 1, $sizes)
        if ($extracted -lt 1 -or $exeIcons[0] -eq [IntPtr]::Zero) {
            throw 'packaged exe exposes no group icon resource (resource ID 1 missing)'
        }
        try {
            $icon = [System.Drawing.Icon]::FromHandle($exeIcons[0])
            try { $referenceDigests[$size] = Get-IconPixelDigest $icon }
            finally { $icon.Dispose() }
        } finally {
            [void][NativeIconProbe]::DestroyIcon($exeIcons[0])
        }
    }
    foreach ($kind in $found.Keys) {
        $icon = [System.Drawing.Icon]::FromHandle($found[$kind])
        try {
            $digest = Get-IconPixelDigest $icon
            if ($referenceDigests.Values -notcontains $digest) {
                throw "titlebar icon (WM_GETICON kind $kind) is not pixel-identical to the packaged exe group icon resource"
            }
            $bitmap = $icon.ToBitmap()
            try {
                $visible = $false
                for ($y = 0; $y -lt $bitmap.Height -and -not $visible; $y += 2) {
                    for ($x = 0; $x -lt $bitmap.Width -and -not $visible; $x += 2) {
                        if ($bitmap.GetPixel($x, $y).A -gt 0) { $visible = $true }
                    }
                }
                if (-not $visible) {
                    throw 'titlebar icon rendered fully transparent (blank resource or placeholder)'
                }
            } finally { $bitmap.Dispose() }
        } finally { $icon.Dispose() }
    }
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
        $entry = [ordered]@{ name = $node.Current.Name; controlType = $node.Current.ControlType.ProgrammaticName; automationId = $node.Current.AutomationId }
        # Field values separate "selection ran but fields did not follow"
        # from "the fields never rendered" in failure evidence.
        try {
            $valuePattern = $node.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
            $entry.value = ([System.Windows.Automation.ValuePattern]$valuePattern).Current.Value
        } catch { }
        $entry
    }
    ConvertTo-Json -InputObject @($snapshot) -Depth 4 |
        Set-Content -LiteralPath $Path -Encoding utf8
}

function Find-EditWithValue {
    param($Root, [string]$Value)
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Edit)
    $edits = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
    for ($index = 0; $index -lt $edits.Count; $index++) {
        try {
            $pattern = $edits[$index].GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
            if (([System.Windows.Automation.ValuePattern]$pattern).Current.Value -eq $Value) {
                return $edits[$index]
            }
        } catch { }
    }
    return $null
}

function Find-PresetCombo {
    param($Root)
    $presetLabels = @('Ollama (local)', 'Ollama（本地）', 'DeepSeek', 'Custom', '自定义',
        'Qwen (Alibaba)', '通义千问 Qwen')
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::ComboBox)
    $combos = $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
    # A collapsed WinUI ComboBox exposes no items in the UIA tree; identify
    # the preset through its Selection pattern (the selected item's name).
    for ($index = 0; $index -lt $combos.Count; $index++) {
        try {
            $selection = $combos[$index].GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern)
            $selected = ([System.Windows.Automation.SelectionPattern]$selection).Current.GetSelection()
            if ($selected.Count -gt 0 -and $presetLabels -contains $selected[0].Current.Name) {
                return $combos[$index]
            }
        } catch { }
    }
    # Fallback: the provider section leads the settings IA, so the first
    # combo on the pane is the preset.
    if ($combos.Count -gt 0) { return $combos[0] }
    return $null
}

function Select-PresetItem {
    param($Root, [string[]]$ItemNames)
    $combo = Find-PresetCombo $Root
    if (-not $combo) { return $false }
    try {
        $expand = $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
        ([System.Windows.Automation.ExpandCollapsePattern]$expand).Expand()
        # Inline poll: Wait-Until probes run under the caller's dynamic scope,
        # so function locals ($combo/$ItemNames) would not resolve there. The
        # expanded items may live under the combo or in a process-owned popup
        # window, so search both.
        $desktop = [System.Windows.Automation.AutomationElement]::RootElement
        $pidCondition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
            $Root.Current.ProcessId)
        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        $item = $null
        while ([DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 100
            $scopes = @($combo)
            $scopes += @($desktop.FindAll([System.Windows.Automation.TreeScope]::Children, $pidCondition))
            for ($scopeIndex = 0; $scopeIndex -lt $scopes.Count -and -not $item; $scopeIndex++) {
                $items = $scopes[$scopeIndex].FindAll([System.Windows.Automation.TreeScope]::Descendants,
                    [System.Windows.Automation.Condition]::TrueCondition)
                for ($index = 0; $index -lt $items.Count; $index++) {
                    if ($ItemNames -contains $items[$index].Current.Name -and
                        $items[$index].Current.ControlType -eq [System.Windows.Automation.ControlType]::ListItem) {
                        $item = $items[$index]
                        break
                    }
                }
            }
            if ($item) { break }
        }
        if (-not $item) {
            ([System.Windows.Automation.ExpandCollapsePattern]$expand).Collapse()
            return $false
        }
        $selection = $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        ([System.Windows.Automation.SelectionItemPattern]$selection).Select()
        # A UIA selection can leave the popup window open, which blocks the
        # dialog's Close button from the tree; collapse it before returning.
        try {
            ([System.Windows.Automation.ExpandCollapsePattern]$expand).Collapse()
        } catch { }
        return $true
    } catch {
        return $false
    }
}

function Assert-PresetSwitchFollows {
    param($SettingsRoot)
    # The seeded profile runs Ollama; switching the provider preset must
    # carry the preset's endpoint and default model into Base URL/Model.
    # GetNewClosure captures $SettingsRoot for probes Wait-Until invokes in
    # its own dynamic scope.
    [void](Wait-Until 'seeded provider endpoint in Base URL' {
        if (Find-EditWithValue $SettingsRoot 'http://127.0.0.1:1/v1') { return $true }; return $false
    }.GetNewClosure())
    [void](Wait-Until 'seeded model in Model' {
        if (Find-EditWithValue $SettingsRoot 'acceptance-never-send') { return $true }; return $false
    }.GetNewClosure())
    Record-Step 'seeded provider fields asserted'
    if (-not (Select-PresetItem $SettingsRoot @('通义千问 Qwen', 'Qwen (Alibaba)'))) {
        throw 'provider preset combo did not offer the Qwen item'
    }
    Record-Step 'Qwen item selected'
    [void](Wait-Until 'Base URL follows the Qwen preset' {
        if (Find-EditWithValue $SettingsRoot 'https://dashscope.aliyuncs.com/compatible-mode/v1') { return $true }; return $false
    }.GetNewClosure())
    Record-Step 'Base URL followed'
    [void](Wait-Until 'Model follows the Qwen preset' {
        if (Find-EditWithValue $SettingsRoot 'qwen3-max') { return $true }; return $false
    }.GetNewClosure())
    Record-Step 'Model followed'
    [void](Wait-Until 'Model follows the Qwen preset' {
        if (Find-EditWithValue $SettingsRoot 'qwen3-max') { return $true }; return $false
    }.GetNewClosure())
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
    & racket (Join-Path $repo 'scripts\acceptance\seed-ai-preview.rkt') $configDir
    if ($LASTEXITCODE -ne 0) { throw 'failed to seed AI preview fixture' }
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
    Test-BrandedTitlebarIcon
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
        # The left-panel mode is per window, not per document: the recovery
        # steps above left the Notes pane selected, so return to the Outline
        # pane before asserting the second book's own chapter navigation.
        [void](Wait-Until 'outline mode for the second EPUB' {
            Invoke-NamedControl $window @('Outline','大纲')
        })
        [void](Wait-Until 'second EPUB still has its own native chapter navigation' {
            Contains-Text $window 'First Chapter'
        })
        Select-EpubTab $bookTabs[0]
        [void](Wait-Until 'notes mode for the first EPUB' {
            Invoke-NamedControl $window @('Annotations','批注')
        })
        [void](Wait-Until 'first EPUB recovery status survives tab switching' {
            (Contains-Text $window 'Recovered fixture note') -and
            ((Contains-Text $window 'Retry Save') -or (Contains-Text $window '重试保存'))
        })
        if (@(Get-EpubTabs $window).Count -ne 2) {
            throw 'an EPUB tab disappeared after switching'
        }
        # The file-first host keeps the AI sidebar hidden until toggled;
        # open it by its localized automation name before reading the pane.
        [void](Wait-Until 'AI panel toggle' {
            Invoke-NamedControl $window @('AI Assistant','AI 助手')
        })
        [void](Wait-Until 'translate current EPUB scope action' {
            Invoke-NamedControl $window @('Translate Chapter','本章翻译','Translate page','本页翻译')
        })
        [void](Wait-Until 'AI send review dialog' {
            (Contains-Text $window 'Review what will be sent') -or
            (Contains-Text $window '核对即将发送的内容')
        })
        [void](Wait-Until 'AI send scope and exact count' {
            ((Contains-Text $window 'Current chapter') -or
             (Contains-Text $window '当前章')) -and
            ((Contains-Text $window 'Total user content:') -or
             (Contains-Text $window '用户内容合计：'))
        })
        [void](Wait-Until 'native AI send cancellation' {
            Invoke-NamedControl $window @('Cancel','取消')
        })
        [void](Wait-Until 'AI send review dismissal' {
            -not ((Contains-Text $window 'Review what will be sent') -or
                  (Contains-Text $window '核对即将发送的内容'))
        })
        if ((Contains-Text $window 'Stop') -or (Contains-Text $window '停止')) {
            throw 'cancelling the AI send review created a stream card'
        }
        # Settings is an app-level surface, not a fourth AI tab. Open the
        # native gear, require the provider section, switch the preset, then
        # close and reopen: the pane reloads stored settings on every open,
        # so the second switch must follow after that reload too.
        [void](Wait-Until 'settings gear' {
            Invoke-NamedControl $window @('Settings','设置')
        })
        $settingsRoot = Wait-Until 'app-level settings surface' {
            $candidates = @($window) + @(Process-Windows -ProcessId $process.Id)
            foreach ($candidate in $candidates) {
                if ((Contains-Text $candidate 'Provider preset') -or
                    (Contains-Text $candidate '服务商预设')) {
                    return $candidate
                }
            }
            return $null
        }
        Record-Step 'settings surface open'
        Assert-PresetSwitchFollows $settingsRoot
        Record-Step 'preset switch followed (round 1)'
        [void](Wait-Until 'settings close action' {
            Invoke-NamedControl $settingsRoot @('Close','关闭')
        })
        # The dialog is its own top-level window, so the main-window
        # dismissal probe cannot see it. Verify it actually left the screen
        # across every process window and re-press Close when the first
        # invoke landed without effect — a lingering modal blocks the gear.
        [void](Wait-Until 'settings dialog left the screen' {
            $candidates = @($window) + @(Process-Windows -ProcessId $process.Id)
            foreach ($candidate in $candidates) {
                if ((Contains-Text $candidate 'Provider preset') -or
                    (Contains-Text $candidate '服务商预设')) {
                    [void](Invoke-NamedControl $candidate @('Close','关闭'))
                    return $false
                }
            }
            return $true
        })
        [void](Wait-Until 'settings dismissal' {
            -not ((Contains-Text $window 'Provider preset') -or
                  (Contains-Text $window '服务商预设'))
        })
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
        actions = @('launch:two-original-epub-paths','switch:first-epub','press:Annotations','switch:second-epub','switch:first-epub','press:AI Assistant','press:Translate current EPUB scope','assert:AI send review','press:Cancel','press:Settings','select:Qwen preset','assert:preset switch follows','press:Close')
        assertions = @('packaged WinUI window activated','branded titlebar icon is pixel-identical to the packaged exe group icon resource','sample EPUB crossed RVT1 into the embedded backend','native accessibility tree contains The Sample Book','native accessibility tree contains First Chapter','pending recovery crosses RVT1 on reopen','native Notes pane exposes recovered status and Retry Save','recovered annotation remains visible and exportable','two distinct native EPUB tabs persist across switches','first EPUB recovery state is isolated','both source EPUB hashes remain unchanged','native review exposes current-chapter scope and exact user-content total before provider launch','Cancel dismisses the review without starting the AI stream','switching the provider preset carries the preset endpoint and default model into Base URL and Model')
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'interaction-trace.json') -Encoding utf8

    $pattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    ([System.Windows.Automation.WindowPattern]$pattern).Close()
    [void](Wait-Until 'Booklight process exit after native window close' { $process.Refresh(); if ($process.HasExited) { return $true }; return $false } 30)
    # ---- close-guard journey: unsaved PDF page edits must survive close ----
    $guardFixture = Join-Path $output "sample-guard-$PID.pdf"
    & racket (Join-Path $repo 'scripts\acceptance\seed-sample-pdf.rkt') $guardFixture
    if ($LASTEXITCODE -ne 0) { throw 'failed to seed the close-guard PDF fixture' }
    $guardHashBefore = (Get-FileHash -LiteralPath $guardFixture -Algorithm SHA256).Hash
    $env:BOOKLIGHT_OPEN = $guardFixture
    $process = Start-Process -FilePath $executable.FullName -PassThru `
        -WorkingDirectory $repo `
        -RedirectStandardOutput (Join-Path $output 'application-guard.stdout.log') `
        -RedirectStandardError (Join-Path $output 'application-guard.stderr.log')
    $window = Wait-Until 'Booklight window for the close-guard journey' {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $desktop.FindFirst([System.Windows.Automation.TreeScope]::Children, $condition)
    }
    try {
        [void](Wait-Until 'synthetic PDF opened' { Contains-Text $window 'sample-guard' })
        # The page-ops pane may already be expanded for a PDF tab; press the
        # unnamed Page edit toggle only to reveal it, then actually rotate —
        # the close guard only exists once a page edit is pending.
        [void](Wait-Until 'rotate page action' {
            if (Contains-Text $window 'Target: page') {
                return Invoke-NamedControl $window @('Rotate Page','旋转本页')
            }
            Invoke-NamedControl $window @('Page edit','页编辑')
            return $false
        })
        # The rotate RPC is async; the tab header gains a bullet only once
        # the in-memory edit is actually pending. Closing earlier would skip
        # the guard legitimately.
        [void](Wait-Until 'pending edits marker in the tab header' {
            Contains-Text $window "$([char]0x2022)"
        })
        $pattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
        ([System.Windows.Automation.WindowPattern]$pattern).Close()
        [void](Wait-Until 'unsaved PDF edits dialog' {
            (Contains-Text $window 'Unsaved PDF edits') -or (Contains-Text $window 'PDF 修改尚未保存')
        })
        [void](Wait-Until 'cancel button' { Invoke-NamedControl $window @('Cancel','取消') })
        [void](Wait-Until 'dialog dismissed after cancel' {
            -not ((Contains-Text $window 'Unsaved PDF edits') -or (Contains-Text $window 'PDF 修改尚未保存'))
        })
        [void](Wait-Until 'document still open after cancel' { Contains-Text $window 'sample-guard' })
        $pattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
        ([System.Windows.Automation.WindowPattern]$pattern).Close()
        [void](Wait-Until 'unsaved PDF edits dialog again' {
            (Contains-Text $window 'Unsaved PDF edits') -or (Contains-Text $window 'PDF 修改尚未保存')
        })
        [void](Wait-Until 'discard button' { Invoke-NamedControl $window @('Discard Changes','放弃更改') })
        [void](Wait-Until 'process exit after discarding the only document' {
            $process.Refresh(); if ($process.HasExited) { return $true }; return $false
        } 30)
    } catch {
        # The process may already be gone when the failure is caught; a dead
        # window must not mask the original error.
        try { Save-Snapshot $window (Join-Path $output 'close-guard-failure.json') } catch { }
        throw
    }
    if ($guardHashBefore -ne (Get-FileHash -LiteralPath $guardFixture -Algorithm SHA256).Hash) { throw 'close-guard journey modified the original PDF' }
    [ordered]@{
        platform = 'windows'
        artifact = $executable.FullName
        actions = @('launch:sample-guard.pdf','press:Page edit','press:Rotate Page','press:WindowClose','assert:Unsaved PDF edits','press:Cancel','press:WindowClose','press:Discard Changes')
        assertions = @('packaged WinUI opens the synthetic PDF','native rotate marks in-memory page edits','window close raises the unsaved-edits dialog','Cancel keeps the document open','Discard closes the window without writing the original')
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'close-guard-trace.json') -Encoding utf8
} finally {
    $env:APPDATA = $savedAppData
    $env:LOCALAPPDATA = $savedLocalData
    $env:BOOKLIGHT_OPEN = $savedOpen
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    if ($process) { $process.Dispose() }
}
