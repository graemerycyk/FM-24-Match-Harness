// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import Foundation

/// Why the Venue tab's entries cannot be saved: the field at fault (nil for the checks across
/// fields) and the Windows viewer's message.
struct VenueProblem: Error, Equatable {
	let field: Int?
	let message: String
}

/// The Venue tab's entries over the fixture's stored values, as the Windows viewer's venue inputs
/// hold them: each field's text, the surface's choice, and whether anything has been changed.
struct VenueForm: Equatable {
	/// The most UTF-16 units a field takes, as the Windows fields do.
	static let textLimit = 63

	/// Each field's text, in fixture_view.h's order; the surface's is its name.
	private(set) var texts: [String]
	/// The surface's choice (FixtureHandle.surfaceNames), or nil if it is none of them.
	private(set) var surface: Int?
	/// An entry has changed since the fixture was opened or saved.
	private(set) var dirty = false

	/// The entries showing `values` as they are stored (all empty without values).
	init(_ values: [FixtureViewNumber] = []) {
		let field = FixtureHandle.surfaceField
		texts = (0..<FixtureHandle.venueFieldCount).map {
			$0 < values.count ? FixtureHandle.venueText($0, values[$0]) : ""
		}
		surface = field < values.count ? FixtureHandle.surfaceChoice(values[field]) : nil
	}

	mutating func setText(_ text: String, field: Int) {
		guard texts.indices.contains(field), field != FixtureHandle.surfaceField else {
			return
		}
		let limited = VenueForm.limited(text)
		if texts[field] != limited {
			texts[field] = limited
			dirty = true
		}
	}

	mutating func setSurface(_ choice: Int?) {
		let known = choice.flatMap { FixtureHandle.surfaceNames.indices.contains($0) ? $0 : nil }
		if surface != known {
			surface = known
			dirty = true
		}
	}

	/// The values a save writes: `stored`, with each stored field read from its entry, then checked
	/// across fields. Fields the fixture does not store stay unstored.
	func values(over stored: [FixtureViewNumber]) -> Result<[FixtureViewNumber], VenueProblem> {
		var values = stored
		for field in values.indices where values[field].known && field < texts.count {
			switch FixtureHandle.parseVenue(field, text: texts[field], surface: surface) {
			case .success(let value):
				values[field].value = value
			case .failure(let error):
				return .failure(VenueProblem(field: field, message: error.message))
			}
		}
		if let problem = FixtureHandle.venueProblem(values) {
			return .failure(VenueProblem(field: nil, message: problem))
		}
		return .success(values)
	}

	/// `text` cut to `limit` UTF-16 units, without splitting a character.
	static func limited(_ text: String, to limit: Int = textLimit) -> String {
		guard text.utf16.count > limit else {
			return text
		}
		var units = 0
		var limited = ""
		for character in text {
			units += character.utf16.count
			if units > limit {
				break
			}
			limited.append(character)
		}
		return limited
	}
}
