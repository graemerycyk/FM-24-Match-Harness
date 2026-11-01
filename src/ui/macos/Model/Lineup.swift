// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// The Edit line-up sheet: a new order of one side's players, a row per place (the XI in its
/// slots, the substitutes, then the players left out), which Confirm makes the side's pending
/// order, as `fm24ctl lineup` writes one. Players change places only by exchanging them.
@MainActor
final class LineupEditor: ObservableObject, Identifiable {
	static let reserveHelp =
		"Not among the substitutes; no such player came on in the matches tested."
	static let ratingHelp =
		"The player's familiarity with the slot's position (pos_*, 1-20), as a hint only: whether "
		+ "the match engine uses it has not been tested."
	static let rebuildNote =
		"Line-up changes are played only without a rebuild: tick Skip rebuild or leave the seeds "
		+ "blank."
	static let aiHelp = "FM24's AI picks this side's XI at kick-off."
	/// The table's columns.
	static let columns = ["Slot", "Position", "Role", "Player", "Rating"]

	let side: Int
	let fixture: FixtureHandle
	/// The side's places: one per player.
	let count: Int
	/// The substitutes' places after the XI.
	let benchSize: Int
	/// The order when the sheet opened: the side's pending order, or the file's.
	let initial: [Int]
	/// For each place, the block whose player the new order puts there.
	@Published private(set) var order: [Int]
	/// The selected row's place.
	@Published var selection: Int?
	/// What `fm24ctl lineup` would warn of in the new order, a sentence each.
	@Published private(set) var warnings: [String] = []

	/// Starts from the side's pending order, with the player in `block` selected if one is given.
	/// The side's line-up must be editable (FixtureHandle.lineupEditable): a row per player.
	init(fixture: FixtureHandle, side: Int, selecting block: Int? = nil) {
		self.side = side
		self.fixture = fixture
		count = fixture.rowCount(side: side)
		benchSize = fixture.benchSize(side: side)
		initial = Array(fixture.order(side: side).prefix(count))
		order = initial
		selection = block.flatMap { fixture.placeOf(side: side, block: $0) }
		warnings = fixture.lineupWarnings(side: side, order: order)
	}

	var sideName: String {
		side == 0 ? "home" : "away"
	}

	/// "Edit home line-up — Liverpool".
	var title: String {
		"Edit \(sideName) line-up — \(fixture.teamName(side: side))"
	}

	// MARK: The rows

	/// "0" to "10" for the XI's slots, "Sub" for the substitutes, then "Reserve".
	func slot(_ place: Int) -> String {
		switch kind(place) {
		case .slot: return String(place)
		case .substitute: return "Sub"
		case .reserve: return "Reserve"
		}
	}

	/// The slot's position, for the XI.
	func position(_ place: Int) -> String {
		kind(place) == .slot ? fixture.cell(side: side, row: place, column: 2) : ""
	}

	/// The slot's role and duty ("Advanced Forward (Attack)"), for the XI.
	func role(_ place: Int) -> String {
		guard kind(place) == .slot else {
			return ""
		}
		let role = fixture.cell(side: side, row: place, column: 3)
		let duty = fixture.cell(side: side, row: place, column: 4)
		return ["", "-", "Unknown"].contains(duty) ? role : "\(role) (\(duty))"
	}

	/// The name of the player the new order puts at `place`.
	func player(_ place: Int) -> String {
		fixture.playerName(side: side, block: block(place))
	}

	/// The player's rating for the slot's position (a hint), for the XI; "-" if it is not known.
	func rating(_ place: Int) -> String {
		guard kind(place) == .slot else {
			return ""
		}
		return fixture.positionRating(side: side, block: block(place), slot: place)
			.map(String.init) ?? "-"
	}

	/// The new order puts another player at `place` than the file has there: the row is bold.
	func isChanged(_ place: Int) -> Bool {
		order.indices.contains(place) && order[place] != place
	}

	/// The row's tooltip: who the file has there, if another player is, and why a reserve is.
	func help(_ place: Int) -> String? {
		var lines: [String] = []
		if isChanged(place) {
			lines.append("Was \(fixture.playerName(side: side, block: place))")
		}
		if kind(place) == .reserve {
			lines.append(LineupEditor.reserveHelp)
		}
		return lines.isEmpty ? nil : lines.joined(separator: "\n")
	}

	/// "Slot 6, DM", "Sub" or "Reserve".
	func placeLabel(_ place: Int) -> String {
		LineupEditor.placeLabel(fixture, side: side, place: place)
	}

	/// The same for a place of the side, in a fixture whose line-up can be changed.
	static func placeLabel(_ fixture: FixtureHandle, side: Int, place: Int) -> String {
		let bench = fixture.benchSize(side: side)
		if place >= FixtureHandle.slotCount {
			return place < FixtureHandle.slotCount + bench ? "Sub" : "Reserve"
		}
		let position = fixture.cell(side: side, row: place, column: 2)
		return position.isEmpty || position == "-" ? "Slot \(place)" : "Slot \(place), \(position)"
	}

	/// Exchange with: the other players, "Florian Wirtz — Sub", with their places.
	var exchangeChoices: [(place: Int, label: String)] {
		guard let selection else {
			return []
		}
		return order.indices.filter { $0 != selection }.map { place in
			(place, "\(player(place)) — \(placeLabel(place))")
		}
	}

	// MARK: Exchanging players

	/// Exchanges the players at two places.
	func exchange(_ first: Int, _ second: Int) {
		guard first != second, order.indices.contains(first), order.indices.contains(second) else {
			return
		}
		order.swapAt(first, second)
		warnings = fixture.lineupWarnings(side: side, order: order)
	}

	/// Exchange with: the selected row's player and the one at `place`, the selection staying with
	/// the row.
	func exchangeSelection(with place: Int) {
		if let selection {
			exchange(selection, place)
		}
	}

	/// Move Up or Down: the selected player can exchange with the next row that way.
	func canMove(by delta: Int) -> Bool {
		selection.map { order.indices.contains($0 + delta) } ?? false
	}

	/// Exchanges the selected player with the neighbouring row's, and selects them there.
	func move(by delta: Int) {
		guard canMove(by: delta), let selection else {
			return
		}
		exchange(selection, selection + delta)
		self.selection = selection + delta
	}

	/// A row dropped on another: the two players exchange places, and the dropped one is selected.
	func drop(_ place: Int, onto target: Int) {
		guard place != target, order.indices.contains(place), order.indices.contains(target) else {
			return
		}
		exchange(place, target)
		selection = target
	}

	/// Puts every player back in the place the file has them.
	func reset() {
		order = Array(order.indices)
		warnings = fixture.lineupWarnings(side: side, order: order)
	}

	/// Reset has something to undo.
	var isReordered: Bool {
		order.enumerated().contains { $0.element != $0.offset }
	}

	/// The places whose player Confirm changes.
	var moved: [Int] {
		order.indices.filter { order[$0] != initial[$0] }
	}

	/// The log's line for Confirm: "Exchanged Ryan Gravenberch (slot 10) and Florian Wirtz (sub) in
	/// the home line-up" for two players, else "Changed the home line-up (N players)".
	var summary: String {
		let places = moved
		guard places.count == 2 else {
			return "Changed the \(sideName) line-up (\(places.count) players)"
		}
		let names = places.map { place in
			let label = kind(place) == .slot ? "slot \(place)" : slot(place).lowercased()
			return "\(fixture.playerName(side: side, block: initial[place])) (\(label))"
		}
		return "Exchanged \(names[0]) and \(names[1]) in the \(sideName) line-up"
	}

	// MARK: Helpers

	private enum Kind {
		case slot
		case substitute
		case reserve
	}

	private func kind(_ place: Int) -> Kind {
		place < FixtureHandle.slotCount
			? .slot : place < FixtureHandle.slotCount + benchSize ? .substitute : .reserve
	}

	private func block(_ place: Int) -> Int {
		order.indices.contains(place) ? order[place] : -1
	}
}

/// A side's line-up before a team replacement, whose rebuild lists the side's players as the game's
/// build does: each place's player by unique ID, to put them back in their places as a pending
/// order, as replace-home and replace-away do in the file they write.
@MainActor
struct KeptLineup {
	let side: Int
	/// The unique ID of each place's player.
	let people: [UInt32]
	/// Their names, for those the rebuilt side lacks.
	let names: [String]

	/// The side's line-up as the lists show it, if it was changed (a new order is pending, or the
	/// file marks it) and each player's unique ID is stored.
	init?(_ fixture: FixtureHandle, side: Int) {
		guard fixture.lineupEditable(side: side),
			fixture.lineupEdited(side: side) || fixture.lineupMarked(side: side)
		else {
			return nil
		}
		let blocks = fixture.order(side: side).prefix(fixture.rowCount(side: side))
		let people = blocks.compactMap { fixture.personId(side: side, block: $0) }
			.compactMap { UInt32(exactly: $0) }
		guard people.count == blocks.count else {
			return nil
		}
		self.side = side
		self.people = people
		names = blocks.map { fixture.playerName(side: side, block: $0) }
	}

	var sideName: String {
		side == 0 ? "home" : "away"
	}

	/// Makes the order that puts the players back in their places the side's pending order in the
	/// rebuilt `fixture` (none if they are in them already). The log's lines: that the line-up was
	/// kept, and each player the side lacks, with who takes that place.
	func restore(in fixture: FixtureHandle) -> Result<[String], ViewerError> {
		guard let kept = fixture.order(side: side, people: people),
			fixture.setOrder(side: side, kept.order)
		else {
			return .failure(
				ViewerError(
					message: "Cannot keep the \(sideName) line-up from before the replacement: the new "
						+ "fixture's \(sideName) order cannot be changed."
				)
			)
		}
		var lines = ["Kept the \(sideName) line-up from before the replacement"]
		for (index, found) in kept.found.enumerated() where !found {
			var line = "\(names[index]) (\(people[index])) is not in the rebuilt \(sideName) squad"
			if let place = kept.places[index] {
				let block = kept.order[place]
				let id = fixture.personId(side: side, block: block).map { " (\($0))" } ?? ""
				line +=
					"; \(fixture.playerName(side: side, block: block))\(id) takes that place "
					+ "(\(LineupEditor.placeLabel(fixture, side: side, place: place)))"
			}
			lines.append(line)
		}
		return .success(lines)
	}
}
