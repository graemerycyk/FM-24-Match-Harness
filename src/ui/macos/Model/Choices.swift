// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Combine
import FixtureCore
import Foundation

/// A team the team pickers offer (`fm24ctl list-teams`), or a player the swap prompt offers
/// (`fm24ctl list-players`).
struct Choice: Equatable {
	let id: UInt32
	/// "Name (ID)", or "Common name (Forename Surname) – Club (ID)".
	let label: String
	/// What a search matches, folded (ChoiceSearch.fold): a team's label, or a player's names.
	let folded: [UInt16]
}

/// The two lists and how their pickers search them, as on Windows.
enum ChoiceKind {
	case teams
	case players

	/// The most matches a picker lists.
	static let shown = 200

	/// The fewest folded characters a search needs before it lists anything.
	var minimum: Int {
		self == .teams ? 0 : 3
	}

	/// Whether a match at the start of any word (after a space or "-") counts as one at the start.
	var words: Bool {
		self == .players
	}

	var name: String {
		self == .teams ? "team" : "player"
	}
}

/// Searching the lists as the Windows viewer does (foldText, matchRank and filterPicker in
/// fixture_window.c, whose tests these mirror). Text is compared as UTF-16 units, as there.
enum ChoiceSearch {
	/// Folded text is held to fewer units than this, as the Windows viewer's buffers hold it.
	static let foldLimit = 1024

	/// The Latin letters that no decomposition splits, spelled out.
	private static let letters: [Unicode.Scalar: String] = [
		"Æ": "ae", "æ": "ae", "Ð": "d", "ð": "d", "Ø": "o", "ø": "o", "Þ": "th", "þ": "th",
		"ß": "ss", "Đ": "d", "đ": "d", "Ħ": "h", "ħ": "h", "ı": "i", "Ł": "l", "ł": "l", "Œ": "oe",
		"œ": "oe",
	]

	/// Lower case without accents, so that "odegaard" matches "Ødegaard": accents are split off and
	/// dropped, and the letters above spelled out. Nil if it takes `limit` units or more.
	static func fold(_ text: String, limit: Int = foldLimit) -> [UInt16]? {
		var units: [UInt16] = []
		units.reserveCapacity(text.utf16.count)
		if text.utf8.allSatisfy({ $0 < 0x80 }) {
			for unit in text.utf16 {
				units.append((0x41...0x5A).contains(unit) ? unit + 0x20 : unit)
			}
		} else {
			var kept = String.UnicodeScalarView()
			for scalar in text.decomposedStringWithCanonicalMapping.unicodeScalars {
				if let spelling = letters[scalar] {
					kept.append(contentsOf: spelling.unicodeScalars)
					continue
				}
				// Letters typed as diacritics stay; only the marks that are not letters go.
				let properties = scalar.properties
				let mark = properties.generalCategory == .nonspacingMark || properties.isDiacritic
				if !mark || properties.isAlphabetic {
					kept.append(scalar)
				}
			}
			units.append(contentsOf: String(kept).lowercased().utf16)
		}
		return units.count < limit ? units : nil
	}

	/// Where `part` is in `text`: 0 at its start (or, for `words`, at the start of any of its words),
	/// 1 only elsewhere, or nil nowhere.
	static func rank(_ text: [UInt16], _ part: [UInt16], words: Bool) -> Int? {
		guard !part.isEmpty else {
			return 0
		}
		guard part.count <= text.count else {
			return nil
		}
		var rank: Int?
		let first = part[0]
		for start in 0...(text.count - part.count) where text[start] == first {
			var index = 1
			while index < part.count && text[start + index] == part[index] {
				index += 1
			}
			guard index == part.count else {
				continue
			}
			if start == 0 || (words && (text[start - 1] == 0x20 || text[start - 1] == 0x2D)) {
				return 0
			}
			rank = 1
			if !words {
				break
			}
		}
		return rank
	}

	/// The choices that match `text`, those that match at the start first, up to ChoiceKind.shown;
	/// none if the folded text is shorter than the kind's minimum.
	static func matches(_ choices: [Choice], _ text: String, _ kind: ChoiceKind) -> [Choice] {
		let part = fold(text) ?? []
		guard part.count >= kind.minimum else {
			return []
		}
		var shown: [Choice] = []
		var later: [Choice] = []
		for choice in choices where shown.count < ChoiceKind.shown {
			switch rank(choice.folded, part, words: kind.words) {
			case 0: shown.append(choice)
			case 1 where later.count < ChoiceKind.shown: later.append(choice)
			default: break
			}
		}
		return Array((shown + later).prefix(ChoiceKind.shown))
	}

	/// The ID of the choice listed as `label`, so that a shared name still resolves.
	static func listedID(_ choices: [Choice], _ label: String) -> UInt32? {
		choices.first { $0.label == label }?.id
	}

	/// The choices in a list file, sorted by label ignoring case and accents (sortKey). Lines that
	/// are not choices are skipped; a list with none is refused, as on Windows.
	static func parse(_ data: Data, _ kind: ChoiceKind) -> Result<[Choice], ViewerError> {
		var keyed: [(key: [UInt16], choice: Choice)] = []
		var label = [CChar](repeating: 0, count: 2048)
		var matched = [CChar](repeating: 0, count: 2048)
		let labelSize = label.count
		let matchedSize = matched.count
		data.withUnsafeBytes { raw in
			guard let base = raw.bindMemory(to: CChar.self).baseAddress else {
				return
			}
			var start = 0
			while start < raw.count {
				var end = start
				while end < raw.count && raw[end] != 0x0A {
					end += 1
				}
				var id: UInt32 = 0
				if fixtureView_choice(
					base + start,
					end - start,
					kind == .players,
					&id,
					&label,
					labelSize,
					&matched,
					matchedSize
				) {
					let text = String(cString: label)
					if let folded = fold(String(cString: matched)) {
						keyed.append((sortKey(text), Choice(id: id, label: text, folded: folded)))
					}
				}
				start = end + 1
			}
		}
		guard !keyed.isEmpty else {
			return .failure(ViewerError(message: "the \(kind.name) list is empty"))
		}
		keyed.sort { left, right in
			left.key != right.key
				? left.key.lexicographicallyPrecedes(right.key)
				: left.choice.label.utf16.lexicographicallyPrecedes(right.choice.label.utf16)
		}
		return .success(keyed.map(\.choice))
	}

	/// How a label sorts, approximating the Windows viewer's word sort (CompareStringW ignoring
	/// case), which gives hyphens and apostrophes no weight, so that "co-op" stays with "coop": the
	/// folded label without them, then, between labels that are otherwise equal, the one with them
	/// later.
	static func sortKey(_ label: String) -> [UInt16] {
		let folded = fold(label, limit: Int.max) ?? []
		let ignored: Set<UInt16> = [0x27, 0x2D]
		return folded.filter { !ignored.contains($0) } + [0]
			+ folded.map { ignored.contains($0) ? 0xFFFF : $0 }
	}
}

/// A list loaded from the game once, in the background, the first time a picker needs it, and
/// again after a failure the next time one does; free text still works meanwhile. It is dropped
/// when the game quits, since another save may list other teams and players.
@MainActor
final class ChoiceList: ObservableObject {
	enum State: Equatable {
		case unloaded
		case loading
		case loaded
	}

	let kind: ChoiceKind
	@Published private(set) var state = State.unloaded
	/// The list, sorted by label; empty until it has loaded.
	private(set) var choices: [Choice] = []
	/// Counts drops, so that a load the game's quitting overtook is set aside.
	private var generation = 0

	init(_ kind: ChoiceKind) {
		self.kind = kind
	}

	/// Starts `fetch` unless the list has loaded or is loading.
	func load(_ fetch: @escaping () async -> Result<[Choice], ViewerError>) {
		guard state == .unloaded else {
			return
		}
		state = .loading
		let started = generation
		Task {
			let result = await fetch()
			guard generation == started else {
				return
			}
			if case .success(let loaded) = result {
				choices = loaded
				state = .loaded
			} else {
				state = .unloaded
			}
		}
	}

	func drop() {
		generation += 1
		choices = []
		state = .unloaded
	}
}
