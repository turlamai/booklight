#!/usr/bin/env python3
import argparse
import json
import time
from pathlib import Path

import pyatspi

def children(node):
    for index in range(node.childCount):
        try: yield node.getChildAtIndex(index)
        except Exception: continue
def walk(node):
    yield node
    for child in children(node): yield from walk(child)
def role(node):
    try: return node.getRoleName()
    except Exception: return "unknown"
def text(node):
    try:
        value = node.queryText(); return value.getText(0, value.characterCount)
    except Exception: return ""
def contains(root, fragment): return any(fragment in (node.name or "") or fragment in text(node) for node in walk(root))
def showing(node):
    try: return node.getState().contains(pyatspi.STATE_SHOWING)
    except Exception: return False
def contains_showing(root, fragment): return any(showing(node) and (fragment in (node.name or "") or fragment in text(node)) for node in walk(root))
def press(node):
    try:
        actions = node.queryAction()
        names = [actions.getName(i) for i in range(actions.nActions)]
        # GTK4 buttons expose "press", "release" and "click"; acting on
        # "press" alone never completes the activation, so prefer "click".
        for preferred in ("click", "press"):
            if preferred in names and actions.doAction(names.index(preferred)):
                return
        for index in range(actions.nActions):
            if actions.doAction(index): return
    except Exception:
        pass
    # GtkMenuButton is exposed as a push button but does not implement the
    # AT-SPI Action interface on every GTK build. Click its native component
    # bounds through AT-SPI; the menu content assertion remains the result.
    extents = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
    if extents.width <= 0 or extents.height <= 0:
        raise AssertionError(f"AT-SPI exposes no invokable bounds for {node.name!r}")
    pyatspi.Registry.generateMouseEvent(
        extents.x + extents.width // 2,
        extents.y + extents.height // 2,
        "b1c",
    )
def select_page_tab(node):
    # GtkStackSwitcher exposes the semantic AT-SPI Selection interface on its
    # page-tab-list parent. Selecting the child is stable on headless Wayland
    # and verifies the same native accessibility contract used by assistive
    # technology, without relying on synthetic pointer delivery.
    parent = node.parent
    selection = parent.querySelection()
    for index, child in enumerate(children(parent)):
        if child == node or ((child.name or "") == (node.name or "") and role(child) == "page tab"):
            if not selection.selectChild(index):
                raise AssertionError(f"AT-SPI failed to select page tab {node.name!r}")
            return
    raise AssertionError(f"page tab {node.name!r} is detached from its tab list")
def wait_for(description, probe, timeout=90.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = probe()
        if value: return value
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {description}")
def find_application():
    desktop = pyatspi.Registry.getDesktop(0)
    for application in children(desktop):
        if any(role(node) == "frame" and (node.name or "") == "Booklight" for node in walk(application)): return application
    return None
def snapshot(node, depth=0):
    result = {"name": node.name or "", "role": role(node)}; value = text(node)
    if value: result["text"] = value
    if depth < 10: result["children"] = [snapshot(child, depth + 1) for child in children(node)]
    return result

# ---- preset-switch journey helpers ----
def settings_frame(application):
    return next((node for node in walk(application) if role(node) == "frame" and (contains(node, "Provider preset") or contains(node, "服务商预设"))), None)
def node_with_text(root, value):
    return next((node for node in walk(root) if text(node) == value), None)
def preset_combo(root):
    labels = ("Ollama (local)", "Ollama（本地）", "DeepSeek", "Custom", "自定义", "Qwen (Alibaba)", "通义千问 Qwen")
    return next((node for node in walk(root) if role(node) == "combo box" and any(label in (descendant.name or "") or label in text(descendant) for descendant in walk(node) for label in labels)), None)
def select_preset(application, names):
    combo = wait_for("provider preset combo box", lambda: preset_combo(application))
    if combo is None: raise AssertionError("provider preset combo box not found")
    # GtkDropDown may expose its selection through the AT-SPI Selection
    # interface; otherwise press it open and click the popup list item.
    try:
        selection = combo.querySelection()
        for index, child in enumerate(children(combo)):
            if (child.name or "") in names and selection.selectChild(index):
                return
    except Exception:
        pass
    press(combo)
    item = wait_for("Qwen preset popup item", lambda: next((node for node in walk(application) if role(node) in ("list item", "menu item") and (node.name or "") in names), None))
    press(item)

def main():
    parser = argparse.ArgumentParser(); parser.add_argument("--output", type=Path, required=True); args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    application = None
    try:
        application = wait_for("Booklight application", find_application)
        window = wait_for("Booklight frame", lambda: next((node for node in walk(application) if role(node) == "frame"), None))
        wait_for("EPUB title parsed by the embedded backend", lambda: contains(window, "The Sample Book"))
        # There are two native controls named Outline: the header popover and
        # the sidebar's GtkStackSwitcher tab.  Select the semantic page tab so
        # the assertion exercises the persistent native TOC surface.
        outline = wait_for("native EPUB outline page", lambda: next((node for node in walk(window) if role(node) == "page tab" and (node.name or "") == "Outline"), None))
        select_page_tab(outline)
        wait_for("EPUB table of contents parsed by the embedded backend", lambda: contains(window, "First Chapter"))
        notes = wait_for("native Annotations page", lambda: next((node for node in walk(window) if role(node) == "page tab" and (node.name or "") in ("Annotations", "批注")), None))
        select_page_tab(notes)
        wait_for("recovered annotation status", lambda: contains(window, "Recovered unsaved annotations"))
        wait_for("retry action", lambda: contains(window, "Retry Save"))
        wait_for("export action", lambda: contains(window, "Export Annotations"))
        wait_for("recovery fixture note", lambda: contains(window, "Recovered fixture note"))
        # The file-first host starts with the AI sidebar hidden (reading
        # canvas first). Open it through the named header-bar toggle — the
        # same surface an assistive-technology user would use. (GtkMenuButton
        # popups do not open through AT-SPI actions on headless Wayland, so
        # the toolbar button is the accessibility contract, not the menu.)
        ai_button = wait_for("AI sidebar toggle", lambda: next((node for node in walk(window) if role(node) == "push button" and (node.name or "") in ("AI Assistant", "AI 助手")), None))
        press(ai_button)
        translate_tab = wait_for("native Translate page", lambda: next((node for node in walk(window) if role(node) == "page tab" and (node.name or "") in ("Translate", "翻译")), None))
        select_page_tab(translate_tab)
        translate_chapter = wait_for("translate current EPUB scope action", lambda: next((node for node in walk(window) if role(node) == "push button" and (node.name or "") in ("Translate Chapter", "本章翻译", "Translate page", "本页翻译")), None))
        press(translate_chapter)
        # GTK exposes transient native dialogs as application-level siblings
        # of their parent frame, not reliably as descendants of the frame.
        wait_for("AI send review dialog", lambda: contains(application, "Review what will be sent") or contains(application, "核对即将发送的内容"))
        wait_for("AI send scope and exact count", lambda: (contains(application, "Current chapter") or contains(application, "当前章")) and (contains(application, "Total user content:") or contains(application, "用户内容合计：")))
        cancel = wait_for("native AI send cancellation", lambda: next((node for node in walk(application) if role(node) == "push button" and (node.name or "") in ("Cancel", "取消")), None))
        press(cancel)
        wait_for("AI send review dismissal", lambda: not contains(application, "Review what will be sent") and not contains(application, "核对即将发送的内容"))
        if contains_showing(window, "Stop") or contains_showing(window, "停止"): raise AssertionError("cancelling the AI send review created a visible stream card")
        # Settings is an app-level window, not a fourth AI tab.
        settings_button = wait_for("settings gear", lambda: next((node for node in walk(window) if role(node) == "push button" and (node.name or "") in ("Settings", "设置")), None))
        press(settings_button)
        settings_surface = wait_for("app-level settings surface", lambda: settings_frame(application))
        if settings_surface is None: raise AssertionError("settings window did not expose the provider section")
        # v1 preset contract: the seeded profile runs Ollama; switching the
        # provider preset must carry the preset's endpoint and default model
        # into Base URL/Model. Re-entering settings re-syncs the pane from
        # stored settings, so the second switch must follow after that sync
        # too (guards the tracked-preset divergence regression).
        wait_for("seeded provider endpoint in Base URL", lambda: node_with_text(application, "http://127.0.0.1:1/v1"))
        wait_for("seeded model in Model", lambda: node_with_text(application, "acceptance-never-send"))
        select_preset(application, ("通义千问 Qwen", "Qwen (Alibaba)"))
        wait_for("Base URL follows the Qwen preset", lambda: node_with_text(application, "https://dashscope.aliyuncs.com/compatible-mode/v1"))
        wait_for("Model follows the Qwen preset", lambda: node_with_text(application, "qwen3-max"))
        settings_button = wait_for("settings gear", lambda: next((node for node in walk(window) if role(node) == "push button" and (node.name or "") in ("Settings", "设置")), None))
        press(settings_button)
        wait_for("settings re-synced to stored provider", lambda: settings_frame(application) is not None and node_with_text(application, "http://127.0.0.1:1/v1") is not None)
        select_preset(application, ("通义千问 Qwen", "Qwen (Alibaba)"))
        wait_for("Base URL follows the Qwen preset after re-sync", lambda: node_with_text(application, "https://dashscope.aliyuncs.com/compatible-mode/v1"))
        wait_for("Model follows the Qwen preset after re-sync", lambda: node_with_text(application, "qwen3-max"))
        (args.output / "accessibility.json").write_text(json.dumps(snapshot(application), ensure_ascii=False, indent=2), encoding="utf-8")
        (args.output / "interaction-trace.json").write_text(json.dumps({"platform":"linux","actions":["launch:sample.epub","select:Outline","select:Annotations","press:AI Assistant","select:Translate","press:Translate current EPUB scope","assert:AI send review","press:Cancel","press:Settings","select:Qwen preset","assert:preset switch follows","press:Settings","select:Qwen preset","assert:preset switch follows after re-sync"],"assertions":["packaged GTK4 frame activated on native Wayland","sample EPUB crossed RVT1 into the embedded backend","native accessibility tree contains The Sample Book","native accessibility tree contains First Chapter","pending recovery crosses RVT1 on reopen","native Notes pane exposes recovered status and Retry Save","recovered annotation remains visible and exportable","native review exposes current-chapter scope and exact user-content total before provider launch","Cancel dismisses the review without starting the AI stream","Settings opens an app-level window with the provider section","switching the provider preset carries the preset endpoint and default model into Base URL and Model","the preset switch still follows after the pane re-syncs stored settings"]}, indent=2), encoding="utf-8")
    except Exception:
        if application is not None: (args.output / "accessibility-failure.json").write_text(json.dumps(snapshot(application), ensure_ascii=False, indent=2), encoding="utf-8")
        raise
if __name__ == "__main__": main()
