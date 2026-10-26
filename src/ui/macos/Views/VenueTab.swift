// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import FixtureCore
import SwiftUI

/// The Venue tab: the eleven venue values in two columns, as the Windows viewer lays them out. A
/// field can be changed when the fixture stores it; fields it does not store are empty. Saves read
/// and check the entries (ViewerModel.readVenue).
struct VenueTab: View {
	@ObservedObject var model: ViewerModel
	let fixture: FixtureHandle

	private static let columns = [0..<6, 6..<FixtureHandle.venueFieldCount]

	var body: some View {
		HStack(alignment: .top, spacing: 0) {
			ForEach(VenueTab.columns, id: \.lowerBound) { fields in
				Grid(alignment: .leading, horizontalSpacing: 4, verticalSpacing: 14) {
					ForEach(fields, id: \.self) { field in
						GridRow {
							Text(FixtureHandle.venueLabel(field))
								.frame(width: 180, alignment: .leading)
							Group {
								if field == FixtureHandle.surfaceField {
									SurfacePopUp(model: model, stored: fixture.venue[field].known)
								} else {
									VenueTextField(model: model, field: field)
								}
							}
							// This also enables the AppKit controls: SwiftUI sets their state.
							.disabled(!model.venueEditable(field))
							.frame(width: 210)
						}
					}
				}
				.frame(maxWidth: .infinity, alignment: .topLeading)
			}
		}
		.padding(12)
		.frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
	}
}

/// Gives a control the keyboard focus when the model asks for it, once it is in a window: each
/// request (told apart by its serial) once, while the model still makes it.
@MainActor
final class FocusTaker {
	private var handled = 0

	/// `serial` is the model's request for this control, or nil; `current` gives the serial the
	/// model still asks for, and `done` tells it the control has the focus. A control that refuses
	/// the focus (a button, without Full Keyboard Access) is asked again later, or with `refused`
	/// not again.
	func update(
		_ control: NSView,
		serial: Int?,
		current: @escaping () -> Int?,
		done: @escaping () -> Void,
		refused: (() -> Void)? = nil
	) {
		guard let serial, serial != handled else {
			return
		}
		handled = serial
		DispatchQueue.main.async { [weak self, weak control] in
			guard let control, current() == serial else {
				return
			}
			if let window = control.window, window.makeFirstResponder(control) {
				done()
			} else if control.window != nil, let refused {
				refused()
			} else {
				self?.handled = 0
			}
		}
	}
}

/// Gives a venue control the keyboard focus when the model asks for it.
@MainActor
private final class VenueFocusTaker {
	private let taker = FocusTaker()

	func update(_ control: NSView, field: Int, model: ViewerModel) {
		let request = model.venueFocus?.field == field ? model.venueFocus : nil
		taker.update(
			control,
			serial: request?.serial,
			current: { model.venueFocus?.field == field ? model.venueFocus?.serial : nil },
			done: {
				if let request {
					model.focusedVenue(request)
				}
			}
		)
	}
}

/// A venue value's entry. An AppKit field rather than a SwiftUI one, which cannot hold typing to
/// the Windows fields' length.
private struct VenueTextField: NSViewRepresentable {
	@ObservedObject var model: ViewerModel
	let field: Int

	func makeCoordinator() -> Coordinator {
		Coordinator(self)
	}

	func makeNSView(context: Context) -> NSTextField {
		let text = NSTextField(string: "")
		text.formatter = LengthLimit(VenueForm.textLimit)
		text.placeholderString = "Not stored"
		text.usesSingleLineMode = true
		text.lineBreakMode = .byClipping
		text.delegate = context.coordinator
		text.setAccessibilityLabel(FixtureHandle.venueLabel(field))
		return text
	}

	func updateNSView(_ text: NSTextField, context: Context) {
		context.coordinator.parent = self
		let value = model.venue.texts[field]
		if text.stringValue != value {
			text.stringValue = value
		}
		context.coordinator.focus.update(text, field: field, model: model)
	}

	@MainActor
	final class Coordinator: NSObject, NSTextFieldDelegate {
		var parent: VenueTextField
		let focus = VenueFocusTaker()

		init(_ parent: VenueTextField) {
			self.parent = parent
		}

		func controlTextDidChange(_ notification: Notification) {
			if let text = notification.object as? NSTextField {
				parent.model.setVenueText(text.stringValue, field: parent.field)
			}
		}
	}
}

/// The surface's pop-up: Grass or Grass/Synthetic mix. A stored surface that is neither shows
/// nothing chosen, which a save refuses, as on Windows.
private struct SurfacePopUp: NSViewRepresentable {
	@ObservedObject var model: ViewerModel
	let stored: Bool

	func makeCoordinator() -> Coordinator {
		Coordinator(self)
	}

	func makeNSView(context: Context) -> NSPopUpButton {
		let popUp = NSPopUpButton(frame: .zero, pullsDown: false)
		popUp.target = context.coordinator
		popUp.action = #selector(Coordinator.choose(_:))
		popUp.setAccessibilityLabel(FixtureHandle.venueLabel(FixtureHandle.surfaceField))
		return popUp
	}

	func updateNSView(_ popUp: NSPopUpButton, context: Context) {
		context.coordinator.parent = self
		let choice = model.venue.surface
		// Without a choice, a first item holds the place: "Not stored", or blank for another surface.
		let titles = (choice == nil ? [stored ? " " : "Not stored"] : []) + FixtureHandle.surfaceNames
		if popUp.itemTitles != titles {
			popUp.removeAllItems()
			popUp.addItems(withTitles: titles)
		}
		popUp.selectItem(at: choice ?? 0)
		context.coordinator.focus.update(popUp, field: FixtureHandle.surfaceField, model: model)
	}

	@MainActor
	final class Coordinator: NSObject {
		var parent: SurfacePopUp
		let focus = VenueFocusTaker()

		init(_ parent: SurfacePopUp) {
			self.parent = parent
		}

		@objc func choose(_ sender: NSPopUpButton) {
			let placeholder = sender.numberOfItems > FixtureHandle.surfaceNames.count ? 1 : 0
			let choice = sender.indexOfSelectedItem - placeholder
			parent.model.setVenueSurface(choice >= 0 ? choice : nil)
		}
	}
}

/// Keeps a text field's entry within a number of UTF-16 units, as a Windows edit control's limit
/// does: longer typing or pasting is cut to fit. With `digitsOnly`, as for a Windows number field,
/// anything but digits is refused.
final class LengthLimit: Formatter {
	private let limit: Int
	private let digitsOnly: Bool

	init(_ limit: Int, digitsOnly: Bool = false) {
		self.limit = limit
		self.digitsOnly = digitsOnly
		super.init()
	}

	@available(*, unavailable)
	required init?(coder: NSCoder) {
		fatalError("LengthLimit is not archived")
	}

	override func string(for value: Any?) -> String? {
		value as? String
	}

	override func getObjectValue(
		_ value: AutoreleasingUnsafeMutablePointer<AnyObject?>?,
		for string: String,
		errorDescription error: AutoreleasingUnsafeMutablePointer<NSString?>?
	) -> Bool {
		value?.pointee = string as NSString
		return true
	}

	override func isPartialStringValid(
		_ partial: String,
		newEditingString replacement: AutoreleasingUnsafeMutablePointer<NSString?>?,
		errorDescription error: AutoreleasingUnsafeMutablePointer<NSString?>?
	) -> Bool {
		if digitsOnly, partial.unicodeScalars.contains(where: { !("0"..."9").contains($0) }) {
			return false
		}
		guard partial.utf16.count > limit else {
			return true
		}
		replacement?.pointee = VenueForm.limited(partial, to: limit) as NSString
		return false
	}
}
