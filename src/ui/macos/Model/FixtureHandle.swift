// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import Foundation

/// A failure described in the viewer's own words, shown as it is; `offersLaunch` if launching the
/// game with the harness would help.
struct ViewerError: Error, Equatable {
	let message: String
	var offersLaunch = false
}

/// The squad edits of the Max buttons, which apply to a side's selected players.
enum PlayerEdit: Int, CaseIterable {
	case happiness
	case condition
	case familiarity

	/// The button's label, as on Windows.
	var title: String {
		switch self {
		case .happiness: return "Max happiness"
		case .condition: return "Max condition"
		case .familiarity: return "Max familiarity"
		}
	}

	/// The menu item's title.
	var menuTitle: String {
		switch self {
		case .happiness: return "Max Happiness"
		case .condition: return "Max Condition"
		case .familiarity: return "Max Familiarity"
		}
	}

	fileprivate var flag: FixtureViewEdit {
		switch self {
		case .happiness: return FIXTURE_VIEW_MAX_HAPPINESS
		case .condition: return FIXTURE_VIEW_MAX_CONDITION
		case .familiarity: return FIXTURE_VIEW_MAX_FAMILIARITY
		}
	}
}

/// The pending player edits that a save writes, or a replacement or simulation carries: the Max
/// buttons' flags and the new line-ups.
struct PlayerChanges: Equatable {
	/// The edit flags, FixtureHandle.blockCount per side, home first.
	var flags: [UInt8]
	/// The new orders, FixtureHandle.blockCount places per side, home first: for each place, the
	/// block whose player moves there (FixtureHandle.order). Nil if neither side has one.
	var lineup: [UInt8]?

	init(
		flags: [UInt8] = [UInt8](repeating: 0, count: 2 * FixtureHandle.blockCount),
		lineup: [UInt8]? = nil
	) {
		self.flags = flags
		self.lineup = lineup
	}

	/// Nothing to write: no flag is set and no order moves a player.
	var isEmpty: Bool {
		!flags.contains { $0 != 0 } && !movesPlayers
	}

	/// An order moves a player to another place.
	var movesPlayers: Bool {
		lineup?.enumerated().contains { Int($0.element) != $0.offset % FixtureHandle.blockCount }
			== true
	}

	/// The changes without a side's: its flags cleared and its order dropped, as when its team is
	/// replaced.
	func dropping(side: Int) -> PlayerChanges {
		let blocks = FixtureHandle.blockCount
		let range = (side * blocks)..<((side + 1) * blocks)
		var changes = self
		changes.flags.replaceSubrange(range, with: repeatElement(0, count: blocks))
		changes.lineup?.replaceSubrange(range, with: (0..<blocks).map { UInt8($0) })
		if !changes.movesPlayers {
			changes.lineup = nil
		}
		return changes
	}

	/// The changes without the flags of the side's player in `block`, as when that player is swapped
	/// out; the orders stay.
	func dropping(side: Int, block: Int) -> PlayerChanges {
		var changes = self
		changes.flags[side * FixtureHandle.blockCount + block] = 0
		return changes
	}
}

/// A fixture read by the C core (src/ui/fixture_view.h), with its pending player edits. It may be
/// created on any thread and is then used only on the main actor; the fixture itself stays in C.
final class FixtureHandle: @unchecked Sendable {
	static let sideCount = 2
	static let columnCount = Int(FIXTURE_VIEW_COLUMNS)
	static let slotCount = Int(FIXTURE_VIEW_SLOTS)
	static let blockCount = Int(FIXTURE_VIEW_BLOCKS)
	static let venueFieldCount = Int(FIXTURE_VIEW_VENUE_FIELDS)
	static let surfaceField = Int(FIXTURE_VIEW_SURFACE)

	private let view: OpaquePointer

	init(json: Data) throws {
		var error = [CChar](repeating: 0, count: 512)
		let errorSize = error.count
		let opened = FixtureHandle.withBytes(json) { bytes, length in
			fixtureView_open(bytes, length, &error, errorSize)
		}
		guard let opened else {
			throw ViewerError(message: FixtureHandle.string(error))
		}
		view = opened
	}

	deinit {
		fixtureView_close(view)
	}

	// MARK: The summary and headings

	/// The summary's lines after its first: competition, venue and date; weather; referee.
	var summaryLines: [String] {
		(0..<3).map { line in FixtureHandle.text { fixtureView_summaryLine(view, UInt32(line), $0, $1) }
		}
	}

	/// "Save: <name>, <game_date>" for a fixture that names the save it was exported from, which
	/// fm24ctl checks before a job; nil for one that names none it can use.
	var saveLine: String? {
		let line = FixtureHandle.text { fixtureView_saveLine(view, $0, $1) }
		return line.isEmpty ? nil : line
	}

	/// The fixture has been saved as binary, which keeps no save: it names none from now on.
	func forgetSave() {
		fixtureView_forgetSave(view)
	}

	/// The two notes that end the summary, and how many of their first characters are bold.
	static let notes: [(text: String, labelLength: Int)] = (0..<2).map { index in
		var labelLength = 0
		let text = String(cString: fixtureView_note(UInt32(index), &labelLength))
		return (text, labelLength)
	}

	func heading(side: Int) -> String {
		FixtureHandle.text { fixtureView_heading(view, UInt32(side), $0, $1) }
	}

	func teamName(side: Int) -> String {
		FixtureHandle.text { fixtureView_teamName(view, UInt32(side), $0, $1) }
	}

	/// The name of the side's player in `block`.
	func playerName(side: Int, block: Int) -> String {
		guard block >= 0 else {
			return ""
		}
		return FixtureHandle.text { fixtureView_playerName(view, UInt32(side), UInt32(block), $0, $1) }
	}

	// MARK: The squad lists and pitch

	static func columnTitle(_ column: Int) -> String {
		String(cString: fixtureView_columnTitle(UInt32(column)))
	}

	static func columnWidth(_ column: Int) -> Int {
		Int(fixtureView_columnWidth(UInt32(column)))
	}

	func rowCount(side: Int) -> Int {
		Int(fixtureView_rowCount(view, UInt32(side)))
	}

	/// The player block a row lists, if that player can be selected.
	func rowBlock(side: Int, row: Int) -> Int? {
		let block = fixtureView_rowBlock(view, UInt32(side), UInt32(row))
		return block >= 0 ? Int(block) : nil
	}

	func cell(side: Int, row: Int, column: Int) -> String {
		FixtureHandle.text { fixtureView_cell(view, UInt32(side), UInt32(row), UInt32(column), $0, $1) }
	}

	/// Where a slot's circle goes on the pitch, as fractions of its width and height.
	func slotSpot(side: Int, slot: Int) -> (x: Double, y: Double)? {
		var x: UInt32 = 0
		var y: UInt32 = 0
		guard fixtureView_slotSpot(view, UInt32(side), UInt32(slot), &x, &y) else {
			return nil
		}
		return (Double(x) / 100, Double(y) / 100)
	}

	// MARK: Player edits

	var hasPlayerEdits: Bool {
		fixtureView_hasPlayerEdits(view)
	}

	/// The edit flags, FixtureHandle.blockCount per side, home first.
	var playerEdits: [UInt8] {
		var flags = [UInt8](repeating: 0, count: 2 * FixtureHandle.blockCount)
		fixtureView_playerEdits(view, &flags)
		return flags
	}

	/// The edit flags with the pending orders, if either side has one.
	var playerChanges: PlayerChanges {
		let edited = (0..<FixtureHandle.sideCount).contains { lineupEdited(side: $0) }
		let lineup = (0..<FixtureHandle.sideCount).flatMap { order(side: $0).map { UInt8($0) } }
		return PlayerChanges(flags: playerEdits, lineup: edited ? lineup : nil)
	}

	/// Forgets the Max edits once a save has written them; a pending order stays until
	/// commitOrder.
	func clearPlayerEdits() {
		fixtureView_clearPlayerEdits(view)
	}

	/// Why `edit` cannot change the side's players in `blocks`, or nil if it can change them all.
	func editProblem(_ edit: PlayerEdit, side: Int, blocks: Set<Int>) -> String? {
		fixtureView_editProblem(view, UInt32(side), FixtureHandle.flags(blocks), edit.flag)
			.map { String(cString: $0) }
	}

	/// Applies `edit` to the side's players in `blocks` and records it for the next save; false,
	/// changing nothing, if none is given or editProblem gives a problem.
	@discardableResult
	func apply(_ edit: PlayerEdit, side: Int, blocks: Set<Int>) -> Bool {
		fixtureView_applyEdit(view, UInt32(side), FixtureHandle.flags(blocks), edit.flag)
	}

	private static func flags(_ blocks: Set<Int>) -> [Bool] {
		var flags = [Bool](repeating: false, count: blockCount)
		for block in blocks where flags.indices.contains(block) {
			flags[block] = true
		}
		return flags
	}

	// MARK: The line-up

	/// FM24's AI does not manage the side, so its first 11 players are its XI.
	func lineupKnown(side: Int) -> Bool {
		fixtureView_lineupKnown(view, UInt32(side))
	}

	/// The side's order can be changed: its line-up is known and its blocks are as many as its
	/// players, at least 11. Its squad list then has a row per place.
	func lineupEditable(side: Int) -> Bool {
		fixtureView_lineupEditable(view, UInt32(side))
	}

	/// The fixture says the side's line-up was changed ("<side>_lineup"), or a save changed it.
	func lineupMarked(side: Int) -> Bool {
		fixtureView_lineupMarked(view, UInt32(side))
	}

	/// A new order of the side is pending.
	func lineupEdited(side: Int) -> Bool {
		fixtureView_lineupEdited(view, UInt32(side))
	}

	/// The substitutes' places after the first 11.
	func benchSize(side: Int) -> Int {
		Int(fixtureView_benchSize(view, UInt32(side)))
	}

	/// The side's pending order, FixtureHandle.blockCount places: for each place, the block whose
	/// player plays there (the place's own block without an order).
	func order(side: Int) -> [Int] {
		var from = [UInt8](repeating: 0, count: FixtureHandle.blockCount)
		fixtureView_order(view, UInt32(side), &from)
		return from.map(Int.init)
	}

	/// Makes `order` (the block for each of the side's places, the rest left in their own) the
	/// side's pending order; false, changing nothing, if it does not place each player once or the
	/// side's order cannot be changed.
	@discardableResult
	func setOrder(side: Int, _ order: [Int]) -> Bool {
		guard order.count <= FixtureHandle.blockCount,
			order.allSatisfy({ (0..<FixtureHandle.blockCount).contains($0) })
		else {
			return false
		}
		var from = (0..<FixtureHandle.blockCount).map { UInt8($0) }
		for (place, block) in order.enumerated() {
			from[place] = UInt8(block)
		}
		return fixtureView_setOrder(view, UInt32(side), from)
	}

	/// The place the pending order gives the side's player in `block`, or nil if there is no such
	/// player.
	func placeOf(side: Int, block: Int) -> Int? {
		guard block >= 0 else {
			return nil
		}
		let place = fixtureView_placeOf(view, UInt32(side), UInt32(block))
		return place >= 0 ? Int(place) : nil
	}

	/// After a save that wrote the pending orders: each player's block becomes their new place's.
	func commitOrder() {
		fixtureView_commitOrder(view)
	}

	/// The side's order with the players whose unique IDs are `people` in those places, as
	/// replace-home and replace-away keep a line-up (fixtureLineup_byPeople), for setOrder; for each
	/// person, whether the side has them, and their place, or for one it lacks the place another
	/// player takes instead (nil if none is left to). Nil if the side's order cannot be changed.
	func order(
		side: Int,
		people: [UInt32]
	) -> (order: [Int], found: [Bool], places: [Int?])? {
		guard people.count <= FixtureHandle.blockCount else {
			return nil
		}
		var from = [UInt8](repeating: 0, count: FixtureHandle.blockCount)
		var found = [Bool](repeating: false, count: people.count)
		var places = [Int32](repeating: -1, count: people.count)
		guard
			fixtureView_orderByPeople(
				view,
				UInt32(side),
				people,
				UInt32(people.count),
				&from,
				&found,
				&places
			)
		else {
			return nil
		}
		return (from.map(Int.init), found, places.map { $0 >= 0 ? Int($0) : nil })
	}

	/// The unique ID of the side's player in `block`, if it is stored.
	func personId(side: Int, block: Int) -> Int64? {
		guard block >= 0 else {
			return nil
		}
		let id = fixtureView_personId(view, UInt32(side), UInt32(block))
		return id >= 0 ? id : nil
	}

	/// The rating (1-20) of the side's player in `block` for the position of `slot`, if both are
	/// known.
	func positionRating(side: Int, block: Int, slot: Int) -> Int? {
		guard block >= 0, slot >= 0 else {
			return nil
		}
		let rating = fixtureView_positionRating(view, UInt32(side), UInt32(block), UInt32(slot))
		return rating >= 0 ? Int(rating) : nil
	}

	/// What `fm24ctl lineup` would warn of in the side's order `order`, a sentence each.
	func lineupWarnings(side: Int, order: [Int]) -> [String] {
		guard order.count <= FixtureHandle.blockCount,
			order.allSatisfy({ (0..<FixtureHandle.blockCount).contains($0) })
		else {
			return []
		}
		var from = (0..<FixtureHandle.blockCount).map { UInt8($0) }
		for (place, block) in order.enumerated() {
			from[place] = UInt8(block)
		}
		let text = FixtureHandle.text { fixtureView_lineupWarnings(view, UInt32(side), from, $0, $1) }
		return text.split(separator: "\n").map(String.init)
	}

	// MARK: The venue

	var venue: [FixtureViewNumber] {
		var values = [FixtureViewNumber](
			repeating: FixtureViewNumber(),
			count: FixtureHandle.venueFieldCount
		)
		fixtureView_venue(view, &values)
		return values
	}

	func setVenue(_ values: [FixtureViewNumber]) {
		precondition(values.count == FixtureHandle.venueFieldCount)
		fixtureView_setVenue(view, values)
	}

	func venueEditable(_ field: Int) -> Bool {
		fixtureView_venueEditable(view, UInt32(field))
	}

	static func venueLabel(_ field: Int) -> String {
		String(cString: fixtureView_venueLabel(UInt32(field)))
	}

	/// A value as the venue form shows it: "" when it is not stored.
	static func venueText(_ field: Int, _ value: FixtureViewNumber) -> String {
		text { fixtureView_venueText(UInt32(field), value, $0, $1) }
	}

	static let surfaceNames: [String] = (0..<2).map {
		String(cString: fixtureView_surfaceName(UInt32($0)))
	}

	static func surfaceChoice(_ value: FixtureViewNumber) -> Int? {
		let choice = fixtureView_surfaceChoice(value)
		return choice >= 0 ? Int(choice) : nil
	}

	/// True if any venue value is stored: saves then always write the venue.
	static func hasVenue(_ values: [FixtureViewNumber]) -> Bool {
		fixtureView_hasVenue(values)
	}

	/// A field's value from its entry (for the surface, the chosen index), or the viewer's message
	/// ("<label> Enter a valid ...").
	static func parseVenue(_ field: Int, text: String, surface: Int?) -> Result<UInt64, ViewerError> {
		var value: UInt64 = 0
		var error = [CChar](repeating: 0, count: 256)
		let errorSize = error.count
		// C text ends at a NUL, which no valid entry holds.
		let entry = text.utf8.contains(0) ? "\u{1}" : text
		let parsed = fixtureView_parseVenue(
			UInt32(field),
			entry,
			Int32(surface ?? -1),
			&value,
			&error,
			errorSize
		)
		return parsed ? .success(value) : .failure(ViewerError(message: string(error)))
	}

	/// Why these venue values cannot be saved together (fixtureData_validateVenue), or nil.
	static func venueProblem(_ values: [FixtureViewNumber]) -> String? {
		precondition(values.count == venueFieldCount)
		var error = [CChar](repeating: 0, count: 256)
		let errorSize = error.count
		return fixtureView_validateVenue(values, &error, errorSize) ? nil : string(error)
	}

	// MARK: Saving

	/// The bytes a save writes for `json`: player edits (the Max flags, then the new orders; or
	/// nil), the venue (written if any value is stored), then encoded as a binary fixture if
	/// `binary`.
	static func write(
		json: Data,
		changes: PlayerChanges?,
		venue: [FixtureViewNumber]?,
		binary: Bool
	) throws -> Data {
		var error = [CChar](repeating: 0, count: 512)
		let errorSize = error.count
		var bytes: UnsafeMutablePointer<UInt8>?
		var length = 0
		let written = withBytes(json) { text, count in
			withOptionalArray(changes?.flags) { flags in
				withOptionalArray(changes?.lineup) { order in
					withOptionalArray(venue) { values in
						fixtureView_write(
							text,
							count,
							flags,
							order,
							values,
							binary,
							&bytes,
							&length,
							&error,
							errorSize
						)
					}
				}
			}
		}
		guard written, let bytes else {
			throw ViewerError(message: string(error))
		}
		defer { fixtureView_free(bytes) }
		return Data(bytes: bytes, count: length)
	}

	/// Fixture JSON without its "save" and "game_date", as a binary copy keeps neither.
	static func withoutSave(_ json: Data) throws -> Data {
		var bytes: UnsafeMutablePointer<UInt8>?
		var length = 0
		let written = withBytes(json) { text, count in
			fixtureView_withoutSave(text, count, &bytes, &length)
		}
		guard written, let bytes else {
			throw ViewerError(message: "cannot read the fixture's JSON")
		}
		defer { fixtureView_free(bytes) }
		return Data(bytes: bytes, count: length)
	}

	/// Whether fixture JSON holds teams that replacements put in (home_team or away_team), which
	/// binary fixtures do not keep.
	static func replacesTeams(_ json: Data) -> Bool {
		var teams: [UInt32] = [0, 0]
		let read = withBytes(json) { text, length in fixtureView_teamOverrides(text, length, &teams) }
		return read && teams.contains { $0 != 0 }
	}

	/// The export's file name, "<home>-v-<away>-<unixTime><extension>"; nil if it is too long.
	func exportName(time unixTime: UInt64, extension fileExtension: String) -> String? {
		var name = [CChar](repeating: 0, count: 512)
		let size = name.count
		guard fixtureView_exportName(view, unixTime, fileExtension, &name, size) else {
			return nil
		}
		return String(cString: name)
	}

	/// The harness protocol version this build speaks.
	static let protocolVersion = String(cString: fixtureView_protocol())

	// MARK: Helpers

	/// Text from a fixture_view.h function, which returns the full length and fills at most the
	/// given number of bytes.
	static func text(_ fill: (UnsafeMutablePointer<CChar>, Int) -> Int) -> String {
		var size = 256
		while true {
			var buffer = [CChar](repeating: 0, count: size)
			let length = buffer.withUnsafeMutableBufferPointer { fill($0.baseAddress!, size) }
			if length < size {
				return String(decoding: buffer[..<length].map { UInt8(bitPattern: $0) }, as: UTF8.self)
			}
			size = length + 1
		}
	}

	private static func string(_ buffer: [CChar]) -> String {
		let bytes = buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
		return String(decoding: bytes, as: UTF8.self)
	}

	/// Data's bytes as C text of a given length (never a null pointer, even when empty).
	private static func withBytes<Result>(
		_ data: Data,
		_ body: (UnsafePointer<CChar>, Int) -> Result
	) -> Result {
		if data.isEmpty {
			return "".withCString { body($0, 0) }
		}
		return data.withUnsafeBytes { raw in
			body(raw.bindMemory(to: CChar.self).baseAddress!, raw.count)
		}
	}

	private static func withOptionalArray<Element, Result>(
		_ array: [Element]?,
		_ body: (UnsafePointer<Element>?) -> Result
	) -> Result {
		guard let array else {
			return body(nil)
		}
		return array.withUnsafeBufferPointer { body($0.baseAddress) }
	}
}
