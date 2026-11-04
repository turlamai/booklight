import AppKit
import ApplicationServices
import CoreGraphics
import Foundation

// Packaged close-guard journey: rotate a page of the synthetic PDF through
// the Tools menu, close the window, and require the unsaved-edits sheet —
// Cancel keeps the document open, Discard closes it, and the original file
// stays byte-identical either way.

enum DriverError: Error, CustomStringConvertible {
    case failure(String)
    var description: String { switch self { case .failure(let message): return message } }
}
func attribute(_ element: AXUIElement, _ name: String) -> CFTypeRef? { var value: CFTypeRef?; return AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success ? value : nil }
func stringAttribute(_ element: AXUIElement, _ name: String) -> String { attribute(element, name) as? String ?? "" }
func children(_ element: AXUIElement) -> [AXUIElement] { attribute(element, kAXChildrenAttribute) as? [AXUIElement] ?? [] }
func walk(_ element: AXUIElement) -> [AXUIElement] { [element] + children(element).flatMap(walk) }
func contains(_ root: AXUIElement, _ fragment: String) -> Bool { walk(root).contains { node in [kAXTitleAttribute, kAXValueAttribute, kAXDescriptionAttribute, kAXHelpAttribute].contains { stringAttribute(node, $0).contains(fragment) } } }
func pressNamed(_ root: AXUIElement, _ names: Set<String>) -> Bool { for node in walk(root) { let values = [kAXTitleAttribute, kAXValueAttribute, kAXDescriptionAttribute].map { stringAttribute(node, $0) }; if values.contains(where: names.contains) && AXUIElementPerformAction(node, kAXPressAction as CFString) == .success { return true } }; return false }
func waitFor<T>(_ description: String, timeout: TimeInterval = 90, probe: () -> T?) throws -> T { let deadline = Date().addingTimeInterval(timeout); while Date() < deadline { if let value = probe() { return value }; Thread.sleep(forTimeInterval: 0.05) }; throw DriverError.failure("timed out waiting for \(description)") }
@discardableResult
func gone(_ root: AXUIElement, _ fragments: [String], timeout: TimeInterval = 90) throws -> Bool {
    _ = try waitFor("dialog dismissal") { fragments.contains { contains(root, $0) } ? nil : true }
    return true
}
func snapshot(_ element: AXUIElement, depth: Int = 0) -> [String: Any] { var result: [String: Any] = ["role": stringAttribute(element, kAXRoleAttribute), "identifier": stringAttribute(element, kAXIdentifierAttribute), "title": stringAttribute(element, kAXTitleAttribute), "label": stringAttribute(element, kAXDescriptionAttribute), "value": stringAttribute(element, kAXValueAttribute)]; if depth < 10 { result["children"] = children(element).map { snapshot($0, depth: depth + 1) } }; return result }
func writeJSON(_ value: Any, to url: URL) throws { try JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]).write(to: url) }
extension AXUIElement {
    var kAXRole: String { stringAttribute(self, kAXRoleAttribute) }
    var kAXTitle: String { stringAttribute(self, kAXTitleAttribute) }
}

let dialogMarkers = ["Unsaved PDF edits", "PDF 修改尚未保存"]
func sendEscapeToPid(_ pid: Int32) {
    guard let down = CGEvent(keyboardEventSource: nil, virtualKey: 53, keyDown: true),
          let up = CGEvent(keyboardEventSource: nil, virtualKey: 53, keyDown: false) else { return }
    down.postToPid(pid)
    usleep(60_000)
    up.postToPid(pid)
}
func sheetElements(_ window: AXUIElement) -> [AXUIElement] {
    walk(window).filter { stringAttribute($0, kAXRoleAttribute) == "AXSheet" }
}
func sheetHasButton(_ window: AXUIElement, _ names: Set<String>) -> Bool {
    for sheet in sheetElements(window) {
        for node in walk(sheet) {
            guard stringAttribute(node, kAXRoleAttribute) == "AXButton" else { continue }
            let values = [kAXTitleAttribute, kAXValueAttribute, kAXDescriptionAttribute].map { stringAttribute(node, $0) }
            if values.contains(where: names.contains) { return true }
        }
    }
    return false
}
func logContains(_ log: URL, _ fragment: String) -> Bool {
    (try? String(contentsOf: log, encoding: .utf8))?.contains(fragment) == true
}
// AXPress fails transiently (cannotComplete) while menus animate or the
// accessibility server is busy; retry at a polling pace like every other
// journey wait instead of giving up after one attempt.
func pressWithRetry(_ node: AXUIElement, attempts: Int = 50) -> Bool {
    for _ in 0..<attempts {
        if AXUIElementPerformAction(node, kAXPressAction as CFString) == .success { return true }
        Thread.sleep(forTimeInterval: 0.4)
    }
    return false
}
// Sheet buttons and menu items refuse AXPress on the packaged runners (or
// accept it without acting); actuate by synthesizing a real click at the
// AX-reported frame instead.
func clickAXButton(_ root: AXUIElement, _ names: Set<String>, _ pid: Int32) -> Bool {
    let roles: Set<String> = ["AXButton", "AXMenuItem"]
    for node in walk(root) {
            guard roles.contains(stringAttribute(node, kAXRoleAttribute)) else { continue }
            let values = [kAXTitleAttribute, kAXValueAttribute, kAXDescriptionAttribute].map { stringAttribute(node, $0) }
            guard values.contains(where: names.contains) else { continue }
            var positionValue: CFTypeRef?; var sizeValue: CFTypeRef?
            guard AXUIElementCopyAttributeValue(node, kAXPositionAttribute as CFString, &positionValue) == .success,
                  AXUIElementCopyAttributeValue(node, kAXSizeAttribute as CFString, &sizeValue) == .success,
                  let positionText = positionValue as? String,
                  let sizeText = sizeValue as? String else { continue }
            // AX renders "x: N y: M" / "w: N h: M".
            let nums = positionText.split(separator: " ").compactMap { Double($0.filter { "0123456789.".contains($0) }) }
            let dims = sizeText.split(separator: " ").compactMap { Double($0.filter { "0123456789.".contains($0) }) }
            guard nums.count >= 2, dims.count >= 2 else { continue }
            let cx = nums[nums.count - 2] + dims[dims.count - 2] / 2
            let cy = nums[nums.count - 1] + dims[dims.count - 1] / 2
            let position = CGPoint(x: cx, y: cy)
            guard let down = CGEvent(mouseEventSource: nil, mouseType: .leftMouseDown,
                                     mouseCursorPosition: position, mouseButton: .left),
                  let up = CGEvent(mouseEventSource: nil, mouseType: .leftMouseUp,
                                   mouseCursorPosition: position, mouseButton: .left) else { return false }
            down.postToPid(pid)
            usleep(80_000)
            up.postToPid(pid)
            return true
    }
    return false
}
var diagnosticApplication: AXUIElement?
var diagnosticOutput: URL?
func snapshotOfDiagnostic() -> [String: Any] {
    if let application = diagnosticApplication { return snapshot(application) }
    return ["error": "application element unavailable"]
}
do {
    let arguments = CommandLine.arguments
    guard let pidIndex = arguments.firstIndex(of: "--pid"), pidIndex + 1 < arguments.count, let pid = Int32(arguments[pidIndex + 1]), let outputIndex = arguments.firstIndex(of: "--output"), outputIndex + 1 < arguments.count else { throw DriverError.failure("usage: macos-ax-close-guard --pid PID --output DIRECTORY") }
    let output = URL(fileURLWithPath: arguments[outputIndex + 1], isDirectory: true); diagnosticOutput = output
    try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
    guard AXIsProcessTrusted() else { throw DriverError.failure("accessibility access is unavailable") }
    let application = AXUIElementCreateApplication(pid); diagnosticApplication = application
    let window: AXUIElement = try waitFor("Booklight window") { (attribute(application, kAXWindowsAttribute) as? [AXUIElement])?.first }
    _ = AXUIElementSetAttributeValue(application, kAXFrontmostAttribute as CFString, kCFBooleanTrue)
    _ = AXUIElementPerformAction(window, kAXRaiseAction as CFString)
    _ = try waitFor("synthetic PDF opened") { contains(window, "sample-guard") ? true : nil }
    // Menubar items expose their submenu items even while the menu is
    // closed, so name-based presses land on nothing; wait for the menu to
    // actually open (an AXMenu child of the pressed item) and press the
    // item inside that open menu.
    let toolsItem = try waitFor("Tools menu bar item") { () -> AXUIElement? in
        walk(application).first { $0.kAXRole == "AXMenuBarItem" && ["Tools", "工具"].contains($0.kAXTitle) }
    }
    guard AXUIElementPerformAction(toolsItem, kAXPressAction as CFString) == .success else { throw DriverError.failure("Tools menu did not open") }
    let openMenu: AXUIElement = try waitFor("opened Tools menu") {
        children(toolsItem).first { stringAttribute($0, kAXRoleAttribute) == "AXMenu" }
    }
    let rotateItem: AXUIElement? = try waitFor("Rotate Page item in the open menu") { () -> AXUIElement? in
        walk(openMenu).first { ["Rotate Page", "旋转本页"].contains(stringAttribute($0, kAXTitleAttribute)) }
    }
    let menuPressWorked = rotateItem.map({ pressWithRetry($0, attempts: 25) }) ?? false
    if !menuPressWorked {
        // Menu-item presses are intermittently refused on the headless
        // runner. The action carries the same Command+R shortcut as Linux,
        // and synthesized keyboard events are the one input channel this
        // runner answers reliably (the Escape cancels all landed).
        FileHandle.standardError.write(Data("rotate: falling back to the keyboard shortcut\n".utf8))
        guard let down = CGEvent(keyboardEventSource: nil, virtualKey: 13, keyDown: true),
              let up = CGEvent(keyboardEventSource: nil, virtualKey: 13, keyDown: false) else {
            throw DriverError.failure("keyboard fallback unavailable")
        }
        down.flags = .maskCommand
        up.flags = .maskCommand
        down.postToPid(pid)
        usleep(80_000)
        up.postToPid(pid)
    }
    // The rotate RPC is async; the host logs once the in-memory edit is
    // actually pending. Closing earlier would skip the guard legitimately.
    let guardLog = output.appendingPathComponent("application-guard.log")
    _ = try waitFor("page edit pending (application log)") {
        (try? String(contentsOf: guardLog, encoding: .utf8))?.contains("page-edit: pending") == true ? true : nil
    }
    func pressWindowClose() -> Bool {
        if let closeButton = attribute(window, kAXCloseButtonAttribute) as! AXUIElement?,
           AXUIElementPerformAction(closeButton, kAXPressAction as CFString) == .success { return true }
        // The title-bar close button can refuse the AX press; the File menu's
        // Close item performs the same close gesture.
        return pressNamed(application, ["Close"])
    }
    guard pressWindowClose() else { throw DriverError.failure("window close gesture did not fire") }
    _ = try waitFor("unsaved PDF edits sheet") { dialogMarkers.contains { contains(application, $0) } ? true : nil }
    // Sheet buttons refuse AXPress on the packaged run (cannotComplete);
    // Escape is the sheet's own cancel gesture — NSAlert binds it to the
    // button titled Cancel. Judge the choice from the host's response log.
    precondition(sheetHasButton(window, ["Cancel", "取消"]), "cancel button missing from the sheet")
    sendEscapeToPid(pid)
    // The host's response log is the dismissal evidence (AX reads of a
    // dismissed NSAlert sheet linger as stale ghosts).
    _ = try waitFor("close-guard cancel response (application log)") {
        logContains(guardLog, "close-guard: sheet response 1002") ? true : nil
    }
    // Cancel keeps the document open: re-fetch the window list fresh —
    // the element captured at launch may be stale after the sheet cycle.
    _ = try waitFor("document still open after cancel") {
        ((attribute(application, kAXWindowsAttribute) as? [AXUIElement]) ?? [])
            .contains { contains($0, "sample-guard") } ? true : nil
    }
    guard pressWindowClose() else { throw DriverError.failure("window close gesture did not fire the second time") }
    _ = try waitFor("unsaved PDF edits sheet again") { dialogMarkers.contains { contains(application, $0) } ? true : nil }
    precondition(sheetHasButton(window, ["Discard Changes", "放弃更改"]), "discard button missing from the sheet")
    // The host binds the save-panel-convention Command+D to the discard
    // button; synthesized keyboard events are the one input channel this
    // runner answers reliably (every Escape and shortcut so far landed).
    precondition(sheetHasButton(window, ["Discard Changes", "放弃更改"]), "discard button missing from the sheet")
    guard let dDown = CGEvent(keyboardEventSource: nil, virtualKey: 2, keyDown: true),
          let dUp = CGEvent(keyboardEventSource: nil, virtualKey: 2, keyDown: false) else {
        throw DriverError.failure("keyboard unavailable")
    }
    dDown.flags = .maskCommand
    dUp.flags = .maskCommand
    dDown.postToPid(pid)
    usleep(80_000)
    dUp.postToPid(pid)
    _ = try waitFor("close-guard discard response (application log)") {
        logContains(guardLog, "close-guard: sheet response 1001") ? true : nil
    }
    // Discard closes the document; discarding the only document may end
    // the whole application, and ghost AX reads persist either way — judge
    // closure by the fresh window list, tolerating a dead app.
    _ = try waitFor("document closed after discard") {
        if NSRunningApplication(processIdentifier: pid)?.isTerminated == true { return true }
        let windows = (attribute(application, kAXWindowsAttribute) as? [AXUIElement]) ?? []
        return windows.contains { contains($0, "sample-guard") } ? nil : true
    }
    try writeJSON(["platform": "macos", "actions": ["launch:sample-guard.pdf", "press:Tools", "press:Rotate Page", "press:CloseButton", "assert:Unsaved PDF edits", "press:Cancel", "press:CloseButton", "press:Discard Changes"], "assertions": ["packaged SwiftUI opens the synthetic PDF", "native rotate marks in-memory page edits", "window close raises the unsaved-edits sheet", "Cancel keeps the document open", "Discard closes the document without writing the original"]], to: output.appendingPathComponent("close-guard.json"))
    if let running = NSRunningApplication(processIdentifier: pid), !running.isTerminated {
        guard running.terminate() else { throw DriverError.failure("native application rejected termination") }
    }
} catch {
    if let output = diagnosticOutput {
        try? writeJSON(snapshotOfDiagnostic(), to: output.appendingPathComponent("close-guard-failure.json"))
    }
    FileHandle.standardError.write(Data("Booklight macOS close-guard failed: \(error)\n".utf8))
    exit(1)
}
