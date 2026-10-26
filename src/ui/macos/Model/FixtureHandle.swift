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

	/// The bytes a save writes for `json`: player edits (playerEdits' flags, or nil), the venue
	/// (written if any value is stored), then encoded as a binary fixture if `binary`.
	static func write(
		json: Data,
		playerEdits: [UInt8]?,
		venue: [FixtureViewNumber]?,
		binary: Bool
	) throws -> Data {
		var error = [CChar](repeating: 0, count: 512)
		let errorSize = error.count
		var bytes: UnsafeMutablePointer<UInt8>?
		var length = 0
		let written = withBytes(json) { text, count in
			withOptionalArray(playerEdits) { flags in
				withOptionalArray(venue) { values in
					fixtureView_write(text, count, flags, values, binary, &bytes, &length, &error, errorSize)
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
