// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Combine
import FixtureCore
import Foundation

/// A side's team editor (the pencil's picker): the team to put in its place, typed or picked from
/// the team list, which it searches as the text changes, and why the last replacement failed.
@MainActor
final class TeamEditor: ObservableObject {
	/// The most characters the picker takes, as the Windows field's limit.
	static let textLimit = 255
	static let placeholder = "Team ID or club short name"
	static let problem = "Enter a team ID or club short name (up to 255 UTF-8 bytes)."

	let side: Int
	@Published private(set) var text: String
	/// The labels the list shows.
	@Published private(set) var matches: [String] = []
	/// Why the team cannot replace the side, in red under the picker.
	@Published var error: String?
	/// Asks the picker for the keyboard focus, with its text selected, each time it changes.
	@Published private(set) var focusSerial = 1
	let teams: ChoiceList
	private var watcher: AnyCancellable?

	/// Starts with the side's team, listed by it.
	init(side: Int, team: String, teams: ChoiceList) {
		self.side = side
		text = VenueForm.limited(team, to: TeamEditor.textLimit)
		self.teams = teams
		// The list, once it arrives, filtered by what has been typed meanwhile.
		watcher = teams.$state.dropFirst().receive(on: DispatchQueue.main).sink { [weak self] _ in
			self?.filter()
		}
		filter()
	}

	var title: String {
		side == 0 ? "Replace home team" : "Replace away team"
	}

	/// Typing, which filters the list.
	func type(_ text: String) {
		self.text = VenueForm.limited(text, to: TeamEditor.textLimit)
		filter()
	}

	/// A label chosen from the list, which leaves the list as it is.
	func pick(_ label: String) {
		text = label
	}

	func requestFocus() {
		focusSerial += 1
	}

	func filter() {
		let choices = teams.state == .loaded ? teams.choices : []
		matches = ChoiceSearch.matches(choices, text, .teams).map(\.label)
	}

	/// What replace-home or replace-away is given: a listed label's ID, or the text as it is. Nil
	/// for text that is blank, holds a line break or takes more than 255 UTF-8 bytes.
	var team: String? {
		let blank = text.unicodeScalars.allSatisfy { $0 == " " || $0 == "\t" }
		let breaks = text.unicodeScalars.contains { $0 == "\r" || $0 == "\n" || $0 == "\0" }
		guard !text.isEmpty, !blank, !breaks, text.utf8.count <= TeamEditor.textLimit else {
			return nil
		}
		let choices = teams.state == .loaded ? teams.choices : []
		return ChoiceSearch.listedID(choices, text).map(String.init) ?? text
	}
}

/// The swap prompt: the player to put in the place of a side's selected player, searched for by
/// name in the player list or given by unique ID, and whether that player may already be in the
/// fixture.
@MainActor
final class SwapPrompt: ObservableObject, Identifiable {
	static let title = "Swap player"
	/// The most characters the field takes, as the Windows field's limit.
	static let textLimit = 1023
	/// Typing is searched for once it pauses this long.
	static let filterDelay: TimeInterval = 0.1
	static let problem =
		"Choose the new player from the list, or enter their unique ID: a number from 1 to "
		+ "4294967295."
	static let duplicatesLabel = "Allow duplicates?"

	let side: Int
	let block: Int
	/// The swapped-out player's name.
	let name: String
	let players: ChoiceList
	@Published private(set) var text = ""
	/// The labels the list shows.
	@Published private(set) var matches: [String] = []
	/// "Allow duplicates?": --allow-duplicate.
	@Published var allowDuplicates = false
	/// Typing waits to be searched.
	@Published private(set) var filterPending = false
	@Published private(set) var focusSerial = 1
	/// How long typing waits to be searched (tests shorten it).
	var delay = SwapPrompt.filterDelay
	private var pending: Task<Void, Never>?
	private var watcher: AnyCancellable?

	init(side: Int, block: Int, name: String, players: ChoiceList) {
		self.side = side
		self.block = block
		self.name = name
		self.players = players
		// The list, once it arrives, filtered by what has been typed meanwhile; the prompt's cue
		// follows it.
		watcher = players.$state.dropFirst().receive(on: DispatchQueue.main).sink { [weak self] _ in
			self?.filter()
		}
	}

	deinit {
		pending?.cancel()
	}

	/// "Swap <name> for...".
	var label: String {
		"Swap \(name) for..."
	}

	/// What the field takes: a name once the player list has loaded, otherwise only an ID.
	var cue: String {
		switch players.state {
		case .loaded: return "Player name or unique ID"
		case .loading: return "Unique ID (loading player names…)"
		case .unloaded: return "Unique ID"
		}
	}

	/// Typing: the list is searched once it pauses.
	func type(_ text: String) {
		self.text = VenueForm.limited(text, to: SwapPrompt.textLimit)
		pending?.cancel()
		filterPending = true
		let nanoseconds = UInt64(max(delay, 0) * 1_000_000_000)
		pending = Task { [weak self] in
			try? await Task.sleep(nanoseconds: nanoseconds)
			if !Task.isCancelled {
				self?.filter()
			}
		}
	}

	/// A label chosen from the list, which leaves the list as it is.
	func pick(_ label: String) {
		text = label
	}

	func requestFocus() {
		focusSerial += 1
	}

	/// Lists the players whose names match what is typed, unless it is a unique ID.
	func filter() {
		pending?.cancel()
		pending = nil
		filterPending = false
		let id = !text.isEmpty && text.unicodeScalars.allSatisfy { ("0"..."9").contains($0) }
		let choices = players.state == .loaded && !id ? players.choices : []
		matches = ChoiceSearch.matches(choices, text, .players).map(\.label)
	}

	/// The new player's unique ID, from a listed label or as typed, or nil if it is not one: a
	/// decimal number from 1 to 4294967295, of fewer than 16 characters.
	var person: String? {
		let choices = players.state == .loaded ? players.choices : []
		if let id = ChoiceSearch.listedID(choices, text) {
			return String(id)
		}
		let digits = text.unicodeScalars.allSatisfy { ("0"..."9").contains($0) }
		guard !text.isEmpty, text.utf16.count < 16, digits, let id = UInt64(text), id > 0,
			id <= UInt64(UInt32.max)
		else {
			return nil
		}
		return text
	}
}

/// What replaces part of the fixture: a side's team, or one of its players.
enum ReplacementJob: Equatable {
	case team(side: Int, team: String)
	/// `person` is the new player's unique ID; with `allowDuplicate` the player may already be in
	/// the fixture.
	case swap(side: Int, block: Int, person: String, allowDuplicate: Bool)

	var side: Int {
		switch self {
		case .team(let side, _), .swap(let side, _, _, _): return side
		}
	}

	var isSwap: Bool {
		if case .swap = self {
			return true
		}
		return false
	}

	/// The pending player edits the job carries over: all but the replaced side's, or all but the
	/// swapped-out player's.
	func carriedEdits(_ edits: [UInt8]) -> [UInt8] {
		var carried = edits
		let blocks = FixtureHandle.blockCount
		switch self {
		case .team(let side, _):
			let range = (side * blocks)..<((side + 1) * blocks)
			carried.replaceSubrange(range, with: repeatElement(0, count: blocks))
		case .swap(let side, let block, _, _):
			carried[side * blocks + block] = 0
		}
		return carried
	}

	/// "home.player[N]" or "away.player[N]", for inject-player.
	static func block(side: Int, block: Int) -> String {
		"\(side == 0 ? "home" : "away").player[\(block)]"
	}

	/// What fm24ctl's refusal of a player already in the fixture says, pointed at the prompt's
	/// checkbox rather than the command line option; anything after it is dropped, as on Windows.
	static func swapProblem(_ problem: String) -> String {
		guard let hint = problem.range(of: "use --allow-duplicate to inject anyway") else {
			return problem
		}
		return String(problem[..<hint.lowerBound])
			+ "tick \"Allow duplicates?\" to swap them in anyway"
	}
}

/// The files of a replacement or swap, in the viewer's temporary folder: the fixture fm24ctl is
/// given and the one the game writes back.
enum ReplacementFiles {
	/// Writes what the job is given to `url`: the fixture's JSON (a binary fixture's decoded JSON)
	/// with the edits it carries over, but not the venue, which goes into the result.
	static func writeInput(_ source: FixtureSource, carrying edits: [UInt8]?, to url: URL) throws {
		let changed = edits?.contains { $0 != 0 } == true ? edits : nil
		let bytes =
			changed == nil
			? source.json
			: try FixtureHandle.write(json: source.json, playerEdits: changed, venue: nil, binary: false)
		do {
			try bytes.write(to: url, options: .withoutOverwriting)
		} catch {
			throw ViewerError(
				message: "Cannot write the fixture to replace in the viewer's temporary folder."
			)
		}
	}

	/// The fixture the game wrote at `url`, with the venue values written into it whenever any is
	/// stored, as on Windows, and opened.
	static func loadResult(
		_ url: URL,
		venue: [FixtureViewNumber]
	) throws -> (json: Data, handle: FixtureHandle) {
		var json: Data
		do {
			json = try Data(contentsOf: url)
		} catch {
			throw ViewerError(message: "cannot read replacement fixture")
		}
		if FixtureHandle.hasVenue(venue) {
			json = try FixtureHandle.write(json: json, playerEdits: nil, venue: venue, binary: false)
		}
		return (json, try FixtureHandle(json: json))
	}
}
