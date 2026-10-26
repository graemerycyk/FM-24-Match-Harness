// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import UniformTypeIdentifiers

/// The viewer's prompts as sheets on its window: error alerts, and the Open and Save panels with
/// the Windows viewer's titles and fixture types.
@MainActor
final class SystemPrompts: ViewerPrompts {
	private static let binary = UTType(filenameExtension: "bin") ?? .data
	/// FM24's saves, which no app registers a type for.
	private static let save = UTType(filenameExtension: "fm") ?? .data

	/// The window the sheets attach to; alerts are app-modal without one.
	weak var window: NSWindow?
	/// A sheet of the viewer's own on the window (the swap prompt, or the Launch FM24 or Install
	/// Harness sheet), to which alerts and panels attach.
	weak var sheet: NSWindow?

	func showError(_ message: String, title: String) {
		let alert = NSAlert()
		alert.alertStyle = .critical
		alert.messageText = title
		alert.informativeText = message
		alert.addButton(withTitle: "OK")
		present(alert) { _ in }
	}

	/// OK (Return and Escape) and `action`: Launch FM24… or Install Harness….
	func offerLaunch(
		_ message: String,
		title: String,
		action: String,
		completion: @escaping (Bool) -> Void
	) {
		let alert = NSAlert()
		alert.alertStyle = .critical
		alert.messageText = title
		alert.informativeText = message
		let ok = alert.addButton(withTitle: "OK")
		alert.addButton(withTitle: action)
		alert.layout()
		let escape = EscapeKey(frame: .zero)
		escape.button = ok
		alert.window.contentView?.addSubview(escape)
		present(alert) { response in
			completion(response == .alertSecondButtonReturn)
		}
	}

	func confirm(
		_ message: String,
		title: String,
		action: String,
		completion: @escaping (Bool) -> Void
	) {
		let alert = SystemPrompts.confirmation(message, title: title, action: action)
		present(alert) { response in
			completion(response == .alertSecondButtonReturn)
		}
	}

	/// A question whose first button, Cancel, answers both Return, as Enter answers No on Windows,
	/// and Escape, as on any Mac. NSAlert gives a button one key, so Escape gets a view of its own.
	static func confirmation(_ message: String, title: String, action: String) -> NSAlert {
		let alert = NSAlert()
		alert.alertStyle = .warning
		alert.messageText = title
		alert.informativeText = message
		let cancel = alert.addButton(withTitle: "Cancel")
		cancel.keyEquivalent = "\r"
		alert.addButton(withTitle: action).hasDestructiveAction = true
		alert.layout()
		let escape = EscapeKey(frame: .zero)
		escape.button = cancel
		alert.window.contentView?.addSubview(escape)
		return alert
	}

	/// A sheet on the window (or on the swap prompt's sheet, while that is open), or a dialog of
	/// its own when neither can take one.
	private func present(
		_ alert: NSAlert,
		completion: @escaping (NSApplication.ModalResponse) -> Void
	) {
		if let sheet, sheet.isVisible, sheet.attachedSheet == nil {
			alert.beginSheetModal(for: sheet, completionHandler: completion)
		} else if let window, window.isVisible, window.attachedSheet == nil {
			alert.beginSheetModal(for: window, completionHandler: completion)
		} else {
			completion(alert.runModal())
		}
	}

	func chooseFixture(completion: @escaping (URL?) -> Void) {
		let panel = NSOpenPanel()
		panel.title = "Open fixture"
		panel.canChooseFiles = true
		panel.canChooseDirectories = false
		panel.allowsMultipleSelection = false
		let filters: [(String, [UTType])] = [
			("Fixture files (*.bin; *.json)", [.json, SystemPrompts.binary]),
			("JSON fixture (*.json)", [.json]),
			("Binary fixture (*.bin)", [SystemPrompts.binary]),
		]
		let accessory = TypeAccessory(label: "Show:", choices: filters.map(\.0)) { [weak panel] index in
			panel?.allowedContentTypes = filters[index].1
		}
		panel.allowedContentTypes = filters[0].1
		panel.accessoryView = accessory.view
		panel.isAccessoryViewDisclosed = true
		present(panel) { response in
			withExtendedLifetime(accessory) {
				completion(response == .OK ? panel.url : nil)
			}
		}
	}

	func chooseDestination(
		name: String,
		directory: URL?,
		binary: Bool,
		completion: @escaping (URL?, Bool) -> Void
	) {
		let panel = NSSavePanel()
		panel.title = "Save fixture as"
		panel.canCreateDirectories = true
		panel.isExtensionHidden = false
		if let directory {
			panel.directoryURL = directory
		}
		let types: [UTType] = [.json, SystemPrompts.binary]
		var chosen = binary ? 1 : 0
		panel.allowedContentTypes = [types[chosen]]
		panel.nameFieldStringValue = SystemPrompts.name(name, as: types[chosen])
		let accessory = TypeAccessory(
			label: "Format:",
			choices: ["JSON fixture (*.json)", "Binary fixture (*.bin)"],
			selected: chosen
		) { [weak panel] index in
			chosen = index
			guard let panel else {
				return
			}
			panel.allowedContentTypes = [types[index]]
			panel.nameFieldStringValue = SystemPrompts.name(panel.nameFieldStringValue, as: types[index])
		}
		panel.accessoryView = accessory.view
		present(panel) { response in
			withExtendedLifetime(accessory) {
				completion(response == .OK ? panel.url : nil, chosen == 1)
			}
		}
	}

	func chooseExportFolder(current: URL, completion: @escaping (URL?) -> Void) {
		let panel = NSOpenPanel()
		panel.title = "Export folder"
		panel.message = "Exports of fixtures not yet saved go to this folder."
		panel.prompt = "Choose"
		panel.canChooseFiles = false
		panel.canChooseDirectories = true
		panel.canCreateDirectories = true
		panel.allowsMultipleSelection = false
		// The folder, or the nearest one above it that exists.
		var shown = current
		while !FileManager.default.fileExists(atPath: shown.path), shown.pathComponents.count > 1 {
			shown.deleteLastPathComponent()
		}
		panel.directoryURL = shown
		present(panel) { response in
			completion(response == .OK ? panel.url : nil)
		}
	}

	func chooseSave(directory: URL?, completion: @escaping (URL?) -> Void) {
		let panel = NSOpenPanel()
		panel.title = "Choose a save"
		panel.message = "Choose an FM24 save (.fm) for the game to load as it starts."
		panel.prompt = "Choose"
		panel.canChooseFiles = true
		panel.canChooseDirectories = false
		panel.allowsMultipleSelection = false
		panel.allowedContentTypes = [SystemPrompts.save]
		if let directory, FileManager.default.fileExists(atPath: directory.path) {
			panel.directoryURL = directory
		}
		present(panel) { response in
			completion(response == .OK ? panel.url : nil)
		}
	}

	/// A sheet on the viewer's own sheet while one is open, else on the window, or a dialog of its
	/// own when neither can take one.
	private func present(
		_ panel: NSSavePanel,
		completion: @escaping (NSApplication.ModalResponse) -> Void
	) {
		if let sheet, sheet.isVisible, sheet.attachedSheet == nil {
			panel.beginSheetModal(for: sheet, completionHandler: completion)
		} else if let window, window.isVisible, window.attachedSheet == nil {
			panel.beginSheetModal(for: window, completionHandler: completion)
		} else {
			completion(panel.runModal())
		}
	}

	/// A file name with the type's extension in place of its own.
	private static func name(_ name: String, as type: UTType) -> String {
		let stem = (name as NSString).deletingPathExtension
		guard let ext = type.preferredFilenameExtension else {
			return name
		}
		return "\(stem.isEmpty ? "fixture" : stem).\(ext)"
	}
}

/// A panel's accessory: a label and a pop-up of file types.
@MainActor
private final class TypeAccessory: NSObject {
	let view: NSView
	private let changed: (Int) -> Void

	init(label: String, choices: [String], selected: Int = 0, changed: @escaping (Int) -> Void) {
		self.changed = changed
		let popUp = NSPopUpButton(frame: .zero, pullsDown: false)
		popUp.addItems(withTitles: choices)
		popUp.selectItem(at: selected)
		let stack = NSStackView(views: [NSTextField(labelWithString: label), popUp])
		stack.orientation = .horizontal
		stack.edgeInsets = NSEdgeInsets(top: 8, left: 8, bottom: 8, right: 8)
		view = stack
		super.init()
		popUp.target = self
		popUp.action = #selector(choose(_:))
	}

	@objc private func choose(_ sender: NSPopUpButton) {
		changed(sender.indexOfSelectedItem)
	}
}

/// Presses its button when Escape is pressed in its window.
private final class EscapeKey: NSView {
	weak var button: NSButton?

	override func performKeyEquivalent(with event: NSEvent) -> Bool {
		let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
		// Key code 53 is Escape, here without modifiers.
		guard event.type == .keyDown, event.keyCode == 53, modifiers.subtracting(.function).isEmpty,
			let button, button.isEnabled
		else {
			return super.performKeyEquivalent(with: event)
		}
		button.performClick(nil)
		return true
	}
}
