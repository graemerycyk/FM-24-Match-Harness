// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import SwiftUI

/// A search field over a list of matches, standing in for the Windows pickers' combo boxes:
/// typing searches (the owner refills `matches`), the arrow keys and a click choose a match into
/// the field, Return confirms and Escape cancels. AppKit, for those keys and the field's length.
struct SearchPicker: NSViewRepresentable {
	static let rowHeight: CGFloat = 18
	static let fieldHeight: CGFloat = 22
	static let spacing: CGFloat = 6

	let text: String
	let placeholder: String
	let matches: [String]
	/// The most characters the field takes.
	let limit: Int
	let enabled: Bool
	/// The field asks for the keyboard focus, with its text selected, each time this changes.
	let focusSerial: Int
	let label: String
	let onType: (String) -> Void
	let onPick: (String) -> Void
	let onSubmit: () -> Void
	let onCancel: () -> Void

	/// The height that shows `rows` matches.
	static func height(rows: Int) -> CGFloat {
		fieldHeight + spacing + CGFloat(rows) * rowHeight + 4
	}

	func makeCoordinator() -> Coordinator {
		Coordinator(self)
	}

	func makeNSView(context: Context) -> NSView {
		let coordinator = context.coordinator
		let field = coordinator.field
		field.formatter = LengthLimit(limit)
		field.delegate = coordinator
		field.target = coordinator
		field.action = #selector(Coordinator.searched(_:))
		field.sendsSearchStringImmediately = true
		field.setAccessibilityLabel(label)
		let table = coordinator.table
		let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("match"))
		column.resizingMask = .autoresizingMask
		table.addTableColumn(column)
		table.headerView = nil
		table.style = .plain
		table.rowHeight = SearchPicker.rowHeight
		table.allowsEmptySelection = true
		table.allowsMultipleSelection = false
		table.allowsExpansionToolTips = true
		table.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
		table.dataSource = coordinator
		table.delegate = coordinator
		table.target = coordinator
		table.doubleAction = #selector(Coordinator.chose(_:))
		table.setAccessibilityLabel("Matches")
		let scroll = NSScrollView()
		scroll.documentView = table
		scroll.hasVerticalScroller = true
		scroll.autohidesScrollers = true
		scroll.borderType = .bezelBorder
		let view = NSView()
		for child in [field, scroll] as [NSView] {
			child.translatesAutoresizingMaskIntoConstraints = false
			view.addSubview(child)
		}
		NSLayoutConstraint.activate([
			field.topAnchor.constraint(equalTo: view.topAnchor),
			field.leadingAnchor.constraint(equalTo: view.leadingAnchor),
			field.trailingAnchor.constraint(equalTo: view.trailingAnchor),
			field.heightAnchor.constraint(equalToConstant: SearchPicker.fieldHeight),
			scroll.topAnchor.constraint(equalTo: field.bottomAnchor, constant: SearchPicker.spacing),
			scroll.leadingAnchor.constraint(equalTo: view.leadingAnchor),
			scroll.trailingAnchor.constraint(equalTo: view.trailingAnchor),
			scroll.bottomAnchor.constraint(equalTo: view.bottomAnchor),
		])
		return view
	}

	func updateNSView(_ view: NSView, context: Context) {
		let coordinator = context.coordinator
		coordinator.parent = self
		let field = coordinator.field
		if field.stringValue != text {
			field.stringValue = text
		}
		field.placeholderString = placeholder
		field.isEnabled = enabled
		coordinator.table.isEnabled = enabled
		if coordinator.matches != matches {
			coordinator.matches = matches
			coordinator.table.reloadData()
		}
		coordinator.selectShown()
		coordinator.focus(focusSerial)
	}

	@MainActor
	final class Coordinator: NSObject, NSSearchFieldDelegate, NSTableViewDataSource,
		NSTableViewDelegate
	{
		var parent: SearchPicker
		let field = NSSearchField()
		let table = KeyViewTable()
		var matches: [String] = []
		/// The table's selection is being set here, not by the user.
		private var updating = false
		private var focused = 0

		init(_ parent: SearchPicker) {
			self.parent = parent
		}

		/// Selects the match the field shows, if it is one.
		func selectShown() {
			let row = matches.firstIndex(of: field.stringValue)
			guard table.selectedRow != (row ?? -1) else {
				return
			}
			updating = true
			if let row {
				table.selectRowIndexes([row], byExtendingSelection: false)
				table.scrollRowToVisible(row)
			} else {
				table.deselectAll(nil)
			}
			updating = false
		}

		/// Puts the focus in the field, with its text selected, once per serial and once it is in a
		/// window.
		func focus(_ serial: Int) {
			guard serial != focused else {
				return
			}
			DispatchQueue.main.async { [weak self] in
				guard let self, serial != self.focused, let window = self.field.window else {
					return
				}
				self.focused = serial
				if window.makeFirstResponder(self.field) {
					self.field.currentEditor()?.selectAll(nil)
				}
			}
		}

		func controlTextDidChange(_ notification: Notification) {
			typed()
		}

		/// The field's own action, which its clear button also sends.
		@objc func searched(_ sender: NSSearchField) {
			typed()
		}

		private func typed() {
			if field.stringValue != parent.text {
				parent.onType(field.stringValue)
			}
		}

		func control(
			_ control: NSControl,
			textView: NSTextView,
			doCommandBy selector: Selector
		) -> Bool {
			switch selector {
			case #selector(NSResponder.moveDown(_:)):
				return move(by: 1)
			case #selector(NSResponder.moveUp(_:)):
				return move(by: -1)
			case #selector(NSResponder.insertNewline(_:)):
				parent.onSubmit()
				return true
			case #selector(NSResponder.cancelOperation(_:)):
				parent.onCancel()
				return true
			default:
				return false
			}
		}

		/// The arrow keys choose the next or previous match into the field, as in a combo box.
		private func move(by step: Int) -> Bool {
			guard !matches.isEmpty else {
				return false
			}
			let current = table.selectedRow
			let row = current < 0 ? (step > 0 ? 0 : matches.count - 1) : current + step
			pick(min(max(row, 0), matches.count - 1))
			return true
		}

		private func pick(_ row: Int) {
			let label = matches[row]
			parent.onPick(label)
			field.stringValue = label
			field.currentEditor()?.moveToEndOfDocument(nil)
			selectShown()
		}

		@objc func chose(_ sender: NSTableView) {
			if sender.clickedRow >= 0 && sender.clickedRow < matches.count {
				pick(sender.clickedRow)
				parent.onSubmit()
			}
		}

		func numberOfRows(in tableView: NSTableView) -> Int {
			matches.count
		}

		func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int)
			-> NSView?
		{
			let identifier = NSUserInterfaceItemIdentifier("match")
			let cell =
				tableView.makeView(withIdentifier: identifier, owner: nil) as? MatchCell
				?? MatchCell(identifier: identifier)
			cell.textField?.stringValue = matches[row]
			return cell
		}

		func tableViewSelectionDidChange(_ notification: Notification) {
			let row = table.selectedRow
			if !updating, row >= 0, row < matches.count, matches[row] != field.stringValue {
				pick(row)
			}
		}
	}
}

/// One match: a line of text, cut off with an ellipsis.
private final class MatchCell: NSTableCellView {
	init(identifier: NSUserInterfaceItemIdentifier) {
		super.init(frame: .zero)
		self.identifier = identifier
		let label = NSTextField(labelWithString: "")
		label.lineBreakMode = .byTruncatingTail
		label.translatesAutoresizingMaskIntoConstraints = false
		addSubview(label)
		textField = label
		NSLayoutConstraint.activate([
			label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
			label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
			label.centerYAnchor.constraint(equalTo: centerYAnchor),
		])
	}

	@available(*, unavailable)
	required init?(coder: NSCoder) {
		fatalError("MatchCell is not archived")
	}
}
