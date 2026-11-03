import AppKit
import ApplicationServices
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

let dialogMarkers = ["Unsaved PDF edits", "PDF 修改尚未保存"]
var diagnosticApplication: AXUIElement?
var diagnosticOutput: URL?
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
    guard pressNamed(application, ["Tools", "工具"]) else { throw DriverError.failure("Tools menu did not open") }
    _ = try waitFor("Tools menu items") { contains(application, "Rotate Page") || contains(application, "旋转本页") ? true : nil }
    guard pressNamed(application, ["Rotate Page", "旋转本页"]) else { throw DriverError.failure("Rotate Page item did not press") }
    let closeButton = try waitFor("window close button", probe: { () -> AXUIElement? in
        attribute(window, kAXCloseButtonAttribute) as! AXUIElement?
    })
    guard AXUIElementPerformAction(closeButton, kAXPressAction as CFString) == .success else { throw DriverError.failure("close button rejected the press") }
    _ = try waitFor("unsaved PDF edits sheet") { dialogMarkers.contains { contains(application, $0) } ? true : nil }
    guard pressNamed(application, ["Cancel", "取消"]) else { throw DriverError.failure("Cancel did not press") }
    _ = try gone(application, dialogMarkers)
    _ = try waitFor("document still open after cancel") { contains(window, "sample-guard") ? true : nil }
    guard AXUIElementPerformAction(closeButton, kAXPressAction as CFString) == .success else { throw DriverError.failure("close button rejected the second press") }
    _ = try waitFor("unsaved PDF edits sheet again") { dialogMarkers.contains { contains(application, $0) } ? true : nil }
    guard pressNamed(application, ["Discard Changes", "放弃更改"]) else { throw DriverError.failure("Discard Changes did not press") }
    _ = try gone(application, dialogMarkers)
    _ = try gone(window, ["sample-guard"])
    try writeJSON(["platform": "macos", "actions": ["launch:sample-guard.pdf", "press:Tools", "press:Rotate Page", "press:CloseButton", "assert:Unsaved PDF edits", "press:Cancel", "press:CloseButton", "press:Discard Changes"], "assertions": ["packaged SwiftUI opens the synthetic PDF", "native rotate marks in-memory page edits", "window close raises the unsaved-edits sheet", "Cancel keeps the document open", "Discard closes the document without writing the original"]], to: output.appendingPathComponent("close-guard.json"))
    guard let running = NSRunningApplication(processIdentifier: pid), running.terminate() else { throw DriverError.failure("native application rejected termination") }
} catch {
    if let application = diagnosticApplication, let output = diagnosticOutput { try? writeJSON(snapshot(application), to: output.appendingPathComponent("close-guard-failure.json")) }
    throw error
}
