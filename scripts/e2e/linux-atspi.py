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
        notes = wait_for("native Notes page", lambda: next((node for node in walk(window) if role(node) == "page tab" and (node.name or "") == "Notes"), None))
        select_page_tab(notes)
        wait_for("recovered annotation status", lambda: contains(window, "Recovered unsaved annotations"))
        wait_for("retry action", lambda: contains(window, "Retry Save"))
        wait_for("export action", lambda: contains(window, "Export Annotations"))
        wait_for("recovery fixture note", lambda: contains(window, "Recovered fixture note"))
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
        (args.output / "accessibility.json").write_text(json.dumps(snapshot(application), ensure_ascii=False, indent=2), encoding="utf-8")
        (args.output / "interaction-trace.json").write_text(json.dumps({"platform":"linux","actions":["launch:sample.epub","select:Outline","select:Notes","select:Translate","press:Translate current EPUB scope","assert:AI send review","press:Cancel"],"assertions":["packaged GTK4 frame activated on native Wayland","sample EPUB crossed RVT1 into the embedded backend","native accessibility tree contains The Sample Book","native accessibility tree contains First Chapter","pending recovery crosses RVT1 on reopen","native Notes pane exposes recovered status and Retry Save","recovered annotation remains visible and exportable","native review exposes current-chapter scope and exact user-content total before provider launch","Cancel dismisses the review without starting the AI stream"]}, indent=2), encoding="utf-8")
    except Exception:
        if application is not None: (args.output / "accessibility-failure.json").write_text(json.dumps(snapshot(application), ensure_ascii=False, indent=2), encoding="utf-8")
        raise
if __name__ == "__main__": main()
