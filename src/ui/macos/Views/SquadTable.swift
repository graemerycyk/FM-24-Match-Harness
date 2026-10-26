// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import SwiftUI

/// A side's squad list: an AppKit table, for the Windows list's exact column widths, horizontal
/// scrolling, gridlines and tooltips on cut-off text. Only real players can be selected, and the
/// selection is frozen while an operation runs. Its context menu has the Max buttons' edits and
/// Swap player.
struct SquadTable: NSViewRepresentable {
	@ObservedObject var model: ViewerModel
	let side: Int

	func makeCoordinator() -> Coordinator {
		Coordinator(self)
	}

	func makeNSView(context: Context) -> NSScrollView {
		let table = KeyViewTable()
		table.style = .plain
		table.allowsMultipleSelection = true
		table.allowsEmptySelection = true
		table.allowsColumnReordering = false
		table.allowsExpansionToolTips = true
		table.columnAutoresizingStyle = .noColumnAutoresizing
		table.gridStyleMask = [.solidVerticalGridLineMask, .solidHorizontalGridLineMask]
		table.intercellSpacing = NSSize(width: 0, height: 0)
		table.rowHeight = 18
		table.setAccessibilityLabel(side == 0 ? "Home team" : "Away team")
		for column in 0..<FixtureHandle.columnCount {
			let tableColumn = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("\(column)"))
			tableColumn.title = FixtureHandle.columnTitle(column)
			tableColumn.width = CGFloat(FixtureHandle.columnWidth(column))
			tableColumn.minWidth = 20
			tableColumn.resizingMask = .userResizingMask
			table.addTableColumn(tableColumn)
		}
		table.dataSource = context.coordinator
		table.delegate = context.coordinator
		let menu = NSMenu()
		menu.autoenablesItems = false
		menu.delegate = context.coordinator
		table.menu = menu
		context.coordinator.table = table
		let scroll = NSScrollView()
		scroll.documentView = table
		scroll.hasVerticalScroller = true
		scroll.hasHorizontalScroller = true
		scroll.autohidesScrollers = true
		scroll.borderType = .bezelBorder
		return scroll
	}

	func updateNSView(_ scroll: NSScrollView, context: Context) {
		let coordinator = context.coordinator
		coordinator.parent = self
		guard let table = scroll.documentView as? NSTableView else {
			return
		}
		if coordinator.fixture !== model.fixture || coordinator.revision != model.revision {
			coordinator.fixture = model.fixture
			coordinator.revision = model.revision
			coordinator.updating = true
			table.reloadData()
			coordinator.updating = false
		}
		table.isEnabled = !model.isBusy
		let rows = coordinator.rows(of: model.selection[side])
		if rows != table.selectedRowIndexes {
			coordinator.updating = true
			table.selectRowIndexes(rows, byExtendingSelection: false)
			coordinator.updating = false
		}
		// After a swap, the list has the focus, as on Windows.
		let model = self.model
		if let request = model.squadFocus, request.side == side, request.control == .list {
			coordinator.focus.update(
				table,
				serial: request.serial,
				current: { model.squadFocus == request ? request.serial : nil },
				done: { model.focusedSquad(request) }
			)
		}
	}

	@MainActor
	final class Coordinator: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate {
		var parent: SquadTable
		var fixture: FixtureHandle?
		/// The model's revision the rows show.
		var revision = -1
		weak var table: NSTableView?
		/// The selection is being set from the model, not by the user.
		var updating = false
		let focus = FocusTaker()

		init(_ parent: SquadTable) {
			self.parent = parent
		}

		func rows(of blocks: Set<Int>) -> IndexSet {
			guard let fixture else {
				return IndexSet()
			}
			let side = parent.side
			return IndexSet(
				(0..<fixture.rowCount(side: side)).filter { row in
					fixture.rowBlock(side: side, row: row).map(blocks.contains) ?? false
				}
			)
		}

		func numberOfRows(in tableView: NSTableView) -> Int {
			fixture?.rowCount(side: parent.side) ?? 0
		}

		func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int)
			-> NSView?
		{
			guard let fixture, let tableColumn, let column = Int(tableColumn.identifier.rawValue) else {
				return nil
			}
			let identifier = NSUserInterfaceItemIdentifier("cell")
			let cell =
				tableView.makeView(withIdentifier: identifier, owner: nil) as? SquadCell
				?? SquadCell(identifier: identifier)
			cell.text = fixture.cell(side: parent.side, row: row, column: column)
			return cell
		}

		func selectionShouldChange(in tableView: NSTableView) -> Bool {
			!parent.model.isBusy
		}

		func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool {
			!parent.model.isBusy && fixture?.rowBlock(side: parent.side, row: row) != nil
		}

		func tableViewSelectionDidChange(_ notification: Notification) {
			guard !updating, let table = notification.object as? NSTableView, let fixture else {
				return
			}
			let side = parent.side
			let blocks = Set(
				table.selectedRowIndexes.compactMap { fixture.rowBlock(side: side, row: $0) }
			)
			parent.model.select(side: side, blocks: blocks)
		}

		/// The context menu. Clicking a player outside the selection selects that player first, as
		/// Finder does.
		func menuNeedsUpdate(_ menu: NSMenu) {
			menu.removeAllItems()
			let model = parent.model
			let side = parent.side
			if let table, let fixture, !model.isBusy, table.clickedRow >= 0,
				let block = fixture.rowBlock(side: side, row: table.clickedRow),
				!model.selection[side].contains(block)
			{
				table.selectRowIndexes([table.clickedRow], byExtendingSelection: false)
			}
			for edit in PlayerEdit.allCases {
				let item = NSMenuItem(
					title: edit.menuTitle,
					action: #selector(maxPlayers(_:)),
					keyEquivalent: ""
				)
				item.target = self
				item.tag = edit.rawValue
				item.isEnabled = model.canEditPlayers(side: side)
				menu.addItem(item)
			}
			menu.addItem(.separator())
			let swap = NSMenuItem(
				title: "Swap Player…",
				action: #selector(swapPlayer(_:)),
				keyEquivalent: ""
			)
			swap.target = self
			swap.isEnabled = model.canSwap(side: side)
			menu.addItem(swap)
		}

		@objc func maxPlayers(_ sender: NSMenuItem) {
			if let edit = PlayerEdit(rawValue: sender.tag) {
				parent.model.maxPlayers(edit, side: parent.side)
			}
		}

		@objc func swapPlayer(_ sender: NSMenuItem) {
			parent.model.requestSwap(side: parent.side)
		}
	}
}

/// A table that hands Tab and Shift-Tab on to the next and previous controls: NSTableView keeps
/// them, which would trap the keyboard focus inside a SwiftUI window.
final class KeyViewTable: NSTableView {
	override func keyDown(with event: NSEvent) {
		switch event.charactersIgnoringModifiers {
		case "\t": window?.selectKeyView(following: self)
		case "\u{19}": window?.selectKeyView(preceding: self)
		default: super.keyDown(with: event)
		}
	}
}

/// A squad list cell: one line of text, cut off with an ellipsis. The small system font fits the
/// Windows list's column widths, as Segoe UI does there.
final class SquadCell: NSTableCellView {
	private let label = NSTextField(labelWithString: "")

	init(identifier: NSUserInterfaceItemIdentifier) {
		super.init(frame: .zero)
		self.identifier = identifier
		label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
		label.lineBreakMode = .byTruncatingTail
		label.translatesAutoresizingMaskIntoConstraints = false
		addSubview(label)
		textField = label
		NSLayoutConstraint.activate([
			label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 3),
			label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -3),
			label.centerYAnchor.constraint(equalTo: centerYAnchor),
		])
	}

	@available(*, unavailable)
	required init?(coder: NSCoder) {
		fatalError("SquadCell is not archived")
	}

	var text: String {
		get { label.stringValue }
		set { label.stringValue = newValue }
	}
}
