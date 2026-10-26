// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import SwiftUI

/// The pencil beside a team's heading, which opens the side's team editor in a popover anchored
/// to it, so that the heading stays in view. The popover shows while the model has the side's
/// editor open and its pencil is on screen; clicking outside it cancels the edit, unless a
/// replacement is running.
struct PencilButton: NSViewRepresentable {
	@ObservedObject var model: ViewerModel
	let side: Int

	func makeCoordinator() -> Coordinator {
		Coordinator(self)
	}

	func makeNSView(context: Context) -> PencilControl {
		let name = side == 0 ? "Replace home team" : "Replace away team"
		let button = PencilControl(
			image: PencilButton.image(name, enabled: false),
			target: context.coordinator,
			action: #selector(Coordinator.edit(_:))
		)
		button.isBordered = false
		(button.cell as? NSButtonCell)?.imageDimsWhenDisabled = false
		button.imageScaling = .scaleProportionallyDown
		button.toolTip = name
		button.setAccessibilityLabel(name)
		button.moved = { [weak coordinator = context.coordinator] in coordinator?.moved() }
		context.coordinator.button = button
		return button
	}

	func updateNSView(_ button: PencilControl, context: Context) {
		let coordinator = context.coordinator
		coordinator.parent = self
		let enabled = model.canEditTeam(side: side)
		// SwiftUI also sets the control's state, so the image follows the model's, not the control's.
		button.isEnabled = enabled
		if coordinator.enabled != enabled {
			coordinator.enabled = enabled
			button.image = PencilButton.image(button.toolTip ?? "", enabled: enabled)
			button.window?.invalidateCursorRects(for: button)
		}
		coordinator.update()
		let request = model.squadFocus
		if let request, request.side == side, request.control == .pencil {
			coordinator.focus.update(
				button,
				serial: request.serial,
				current: { model.squadFocus == request ? request.serial : nil },
				done: { model.focusedSquad(request) },
				refused: { model.focusedSquad(request) }
			)
		}
	}

	/// The pencil symbol in blue, as on Windows, or grey when it cannot be used, whether or not
	/// the window is active.
	static func image(_ name: String, enabled: Bool) -> NSImage {
		let color: NSColor = enabled ? .linkColor : .secondaryLabelColor
		let symbol = NSImage(systemSymbolName: "pencil", accessibilityDescription: name) ?? NSImage()
		let palette = NSImage.SymbolConfiguration(pointSize: 13, weight: .regular)
			.applying(NSImage.SymbolConfiguration(paletteColors: [color]))
		return symbol.withSymbolConfiguration(palette) ?? symbol
	}

	static func dismantleNSView(_ button: PencilControl, coordinator: Coordinator) {
		coordinator.close()
	}

	@MainActor
	final class Coordinator: NSObject, NSPopoverDelegate {
		var parent: PencilButton
		weak var button: PencilControl?
		let focus = FocusTaker()
		/// Whether the pencil's image shows it enabled.
		var enabled = false
		private var popover: NSPopover?
		/// The editor the popover shows.
		private weak var shown: TeamEditor?
		/// The popover is being closed here, not by the user.
		private var closing = false

		init(_ parent: PencilButton) {
			self.parent = parent
		}

		@objc func edit(_ sender: Any?) {
			parent.model.editTeam(side: parent.side)
		}

		/// Shows or closes the popover as the model's editor and the pencil's place require.
		func update() {
			let model = parent.model
			let editor = model.teamEditor?.side == parent.side ? model.teamEditor : nil
			guard let editor, let button, let window = button.window, window.isVisible,
				!button.isHiddenOrHasHiddenAncestor
			else {
				close()
				return
			}
			if popover?.isShown == true && shown === editor {
				return
			}
			close()
			let controller = NSHostingController(
				rootView: TeamPickerView(model: model, editor: editor, teams: model.teams)
			)
			controller.sizingOptions = [.preferredContentSize]
			let popover = NSPopover()
			popover.behavior = .transient
			popover.animates = false
			popover.contentViewController = controller
			popover.delegate = self
			self.popover = popover
			shown = editor
			popover.show(
				relativeTo: button.bounds,
				of: button,
				preferredEdge: button.isFlipped ? .maxY : .minY
			)
		}

		/// The pencil has left its window (another tab) or come back.
		func moved() {
			DispatchQueue.main.async { [weak self] in
				self?.update()
			}
		}

		func close() {
			guard let popover else {
				return
			}
			closing = true
			popover.close()
			closing = false
			self.popover = nil
			shown = nil
		}

		func popoverShouldClose(_ popover: NSPopover) -> Bool {
			closing || parent.model.canCancelTeamEdit
		}

		/// Closed by the user (a click outside it): the edit is cancelled.
		func popoverDidClose(_ notification: Notification) {
			guard !closing, notification.object as? NSPopover === popover else {
				return
			}
			popover = nil
			shown = nil
			if parent.model.teamEditor?.side == parent.side {
				parent.model.cancelTeamEdit()
			}
		}
	}
}

/// The pencil: a hand cursor while it can be clicked, and a word to its coordinator when it moves
/// between windows.
final class PencilControl: NSButton {
	var moved: () -> Void = {}

	override func resetCursorRects() {
		if isEnabled {
			addCursorRect(bounds, cursor: .pointingHand)
		}
	}

	override func viewDidMoveToWindow() {
		super.viewDidMoveToWindow()
		moved()
	}
}

/// The team editor in the pencil's popover: the picker (pre-filled with the team's name and
/// selected) over the team list's matches, the error of the last attempt in red, and Confirm and
/// Cancel. Return confirms and Escape cancels.
struct TeamPickerView: View {
	static let rows = 15

	@ObservedObject var model: ViewerModel
	@ObservedObject var editor: TeamEditor
	@ObservedObject var teams: ChoiceList

	var body: some View {
		VStack(alignment: .leading, spacing: 8) {
			Text(editor.title)
				.font(.headline)
			SearchPicker(
				text: editor.text,
				placeholder: TeamEditor.placeholder,
				matches: editor.matches,
				limit: TeamEditor.textLimit,
				enabled: model.canCancelTeamEdit,
				focusSerial: editor.focusSerial,
				label: editor.title,
				onType: { editor.type($0) },
				onPick: { editor.pick($0) },
				onSubmit: { model.confirmTeam() },
				onCancel: { model.cancelTeamEdit() }
			)
			.frame(width: 320, height: SearchPicker.height(rows: TeamPickerView.rows))
			if teams.state == .loading {
				Text("Loading teams…")
					.font(.caption)
					.foregroundColor(.secondary)
			}
			if let error = editor.error {
				ErrorText(text: error)
					.frame(width: 320)
			}
			HStack {
				Spacer()
				Button("Cancel") { model.cancelTeamEdit() }
					.disabled(!model.canCancelTeamEdit)
				Button("Confirm") { model.confirmTeam() }
					.keyboardShortcut(.defaultAction)
					.disabled(!model.canEditTeam(side: editor.side))
			}
			.frame(width: 320)
		}
		.padding(12)
	}
}

/// An error in red, wrapped, scrolling once it is taller than 60 points.
struct ErrorText: View {
	let text: String

	var body: some View {
		ViewThatFits(in: .vertical) {
			label
			ScrollView {
				label
			}
		}
		.frame(maxHeight: 60)
	}

	private var label: some View {
		Text(text)
			.foregroundColor(.red)
			.textSelection(.enabled)
			.fixedSize(horizontal: false, vertical: true)
			.frame(maxWidth: .infinity, alignment: .leading)
	}
}
