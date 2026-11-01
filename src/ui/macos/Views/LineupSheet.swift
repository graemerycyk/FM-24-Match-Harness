// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import SwiftUI

/// The Edit line-up sheet: a row per place of one side (the XI in its slots, the substitutes, then
/// the reserves) with the slot's position and role, the player the new order puts there and their
/// rating for the position. Exchange with, Move Up and Move Down (⌥⌘↑ and ⌥⌘↓) and dropping one
/// row on another exchange two players. What `fm24ctl lineup` would warn of shows under the table
/// as the order changes, with a note while the Simulation tab would rebuild each seed. Reset puts
/// every player back in the file's place, Cancel (Escape) closes the sheet, and Confirm (Return,
/// the default) keeps the order as a pending edit.
struct LineupSheet: View {
	static let width: CGFloat = 600
	static let tableHeight: CGFloat = 420
	/// Room for a few warnings, so that the sheet keeps its size as they come and go.
	static let notesHeight: CGFloat = 64

	@ObservedObject var model: ViewerModel
	@ObservedObject var editor: LineupEditor

	var body: some View {
		VStack(alignment: .leading, spacing: 10) {
			Text(editor.title)
				.font(.headline)
				.lineLimit(1)
				.truncationMode(.tail)
				.help(editor.title)
			LineupTable(editor: editor)
				.frame(height: LineupSheet.tableHeight)
			HStack {
				Menu("Exchange with") {
					ForEach(editor.exchangeChoices, id: \.place) { choice in
						Button(choice.label) { editor.exchangeSelection(with: choice.place) }
					}
				}
				.disabled(editor.selection == nil)
				.fixedSize()
				Button("Move Up") { editor.move(by: -1) }
					.keyboardShortcut(.upArrow, modifiers: [.command, .option])
					.disabled(!editor.canMove(by: -1))
				Button("Move Down") { editor.move(by: 1) }
					.keyboardShortcut(.downArrow, modifiers: [.command, .option])
					.disabled(!editor.canMove(by: 1))
				Spacer()
			}
			VStack(alignment: .leading, spacing: 4) {
				ForEach(Array(editor.warnings.enumerated()), id: \.offset) { _, warning in
					Label(warning, systemImage: "exclamationmark.triangle")
				}
				if model.simulationForm.rebuilds {
					Label(LineupEditor.rebuildNote, systemImage: "info.circle")
						.foregroundColor(.secondary)
				}
			}
			.font(.callout)
			.fixedSize(horizontal: false, vertical: true)
			.frame(maxWidth: .infinity, minHeight: LineupSheet.notesHeight, alignment: .topLeading)
			HStack {
				Button("Reset") { editor.reset() }
					.disabled(!editor.isReordered)
					.help("Put every player back in the place the fixture has them")
				Spacer()
				Button("Cancel") { model.cancelLineup() }
					.keyboardShortcut(.cancelAction)
				Button("Confirm") { model.confirmLineup() }
					.keyboardShortcut(.defaultAction)
			}
		}
		.padding(16)
		.frame(width: LineupSheet.width)
	}
}

/// The sheet's table: an AppKit table, as the squad lists are, whose changed rows are bold with
/// the player the fixture has there in their tooltip, and whose rows can be dropped on one another.
private struct LineupTable: NSViewRepresentable {
	static let placeType = NSPasteboard.PasteboardType("org.fm24-harness.viewer.lineup-place")
	static let widths: [CGFloat] = [56, 70, 200, 180, 52]

	@ObservedObject var editor: LineupEditor

	func makeCoordinator() -> Coordinator {
		Coordinator(self)
	}

	func makeNSView(context: Context) -> NSScrollView {
		let table = KeyViewTable()
		table.style = .plain
		table.allowsMultipleSelection = false
		table.allowsEmptySelection = true
		table.allowsColumnReordering = false
		table.allowsExpansionToolTips = true
		table.columnAutoresizingStyle = .noColumnAutoresizing
		table.gridStyleMask = [.solidVerticalGridLineMask, .solidHorizontalGridLineMask]
		table.intercellSpacing = NSSize(width: 0, height: 0)
		table.rowHeight = 18
		table.setAccessibilityLabel(editor.title)
		for (column, title) in LineupEditor.columns.enumerated() {
			let tableColumn = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("\(column)"))
			tableColumn.title = title
			tableColumn.width = LineupTable.widths[column]
			tableColumn.minWidth = 20
			tableColumn.resizingMask = .userResizingMask
			if column == LineupEditor.columns.count - 1 {
				tableColumn.headerToolTip = LineupEditor.ratingHelp
			}
			table.addTableColumn(tableColumn)
		}
		table.dataSource = context.coordinator
		table.delegate = context.coordinator
		table.registerForDraggedTypes([LineupTable.placeType])
		table.setDraggingSourceOperationMask(.move, forLocal: true)
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
		if coordinator.order != editor.order {
			coordinator.order = editor.order
			coordinator.updating = true
			table.reloadData()
			coordinator.updating = false
		}
		let rows = editor.selection.map { IndexSet(integer: $0) } ?? IndexSet()
		if rows != table.selectedRowIndexes {
			coordinator.updating = true
			table.selectRowIndexes(rows, byExtendingSelection: false)
			coordinator.updating = false
			if let row = editor.selection {
				table.scrollRowToVisible(row)
			}
		}
	}

	@MainActor
	final class Coordinator: NSObject, NSTableViewDataSource, NSTableViewDelegate {
		var parent: LineupTable
		/// The order the rows show.
		var order: [Int]?
		weak var table: NSTableView?
		/// The selection is being set from the editor, not by the user.
		var updating = false

		init(_ parent: LineupTable) {
			self.parent = parent
		}

		func numberOfRows(in tableView: NSTableView) -> Int {
			parent.editor.count
		}

		func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int)
			-> NSView?
		{
			guard let tableColumn, let column = Int(tableColumn.identifier.rawValue) else {
				return nil
			}
			let identifier = NSUserInterfaceItemIdentifier("lineup")
			let cell =
				tableView.makeView(withIdentifier: identifier, owner: nil) as? SquadCell
				?? SquadCell(identifier: identifier)
			let editor = parent.editor
			let texts = [
				editor.slot(row), editor.position(row), editor.role(row), editor.player(row),
				editor.rating(row),
			]
			cell.text = texts.indices.contains(column) ? texts[column] : ""
			cell.bold = editor.isChanged(row)
			cell.help = editor.help(row)
			return cell
		}

		func tableViewSelectionDidChange(_ notification: Notification) {
			guard !updating, let table = notification.object as? NSTableView else {
				return
			}
			parent.editor.selection = table.selectedRow >= 0 ? table.selectedRow : nil
		}

		func tableView(_ tableView: NSTableView, pasteboardWriterForRow row: Int)
			-> NSPasteboardWriting?
		{
			let item = NSPasteboardItem()
			item.setString(String(row), forType: LineupTable.placeType)
			return item
		}

		/// A row from this table, dropped on another row: never between rows.
		func tableView(
			_ tableView: NSTableView,
			validateDrop info: NSDraggingInfo,
			proposedRow row: Int,
			proposedDropOperation dropOperation: NSTableView.DropOperation
		) -> NSDragOperation {
			guard info.draggingSource as? NSTableView === tableView, let place = place(info),
				place != row, (0..<parent.editor.count).contains(row)
			else {
				return []
			}
			tableView.setDropRow(row, dropOperation: .on)
			return .move
		}

		func tableView(
			_ tableView: NSTableView,
			acceptDrop info: NSDraggingInfo,
			row: Int,
			dropOperation: NSTableView.DropOperation
		) -> Bool {
			guard dropOperation == .on, let place = place(info) else {
				return false
			}
			parent.editor.drop(place, onto: row)
			return true
		}

		private func place(_ info: NSDraggingInfo) -> Int? {
			info.draggingPasteboard.string(forType: LineupTable.placeType).flatMap { Int($0) }
		}
	}
}
