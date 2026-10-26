// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import FixtureSamples
import Foundation

/// ViewerModel's offline editing: the Max buttons, the Venue tab, what saves write, and the
/// questions before unsaved edits are discarded, with the Windows viewer's rules and messages.
@MainActor
enum ViewerEditTests {
	static func register() {
		Tests.test("viewer maximises the selected players", maximises)
		Tests.test("viewer changes no player when one lacks the data", allOrNothing)
		Tests.test("viewer saves player edits and clears them", savesPlayerEdits)
		Tests.test("viewer edits the venue and saves it", editsVenue)
		Tests.test("viewer edits only stored venue fields, within 63 characters", limitsVenue)
		Tests.test("viewer says which venue entry is invalid", refusesVenue)
		Tests.test("viewer asks before an open discards edits", asksBeforeOpening)
		Tests.test("viewer asks before closing with edits", asksBeforeClosing)
		Tests.test("viewer allows no edits while a file operation runs", locksEdits)
	}

	private static let surface = FixtureHandle.surfaceField
	private static let capacity = 4
	private static let homeFans = 6

	private static func model() -> (ViewerModel, FakePrompts) {
		let prompts = FakePrompts()
		return (ViewerModel(log: ViewerLog(echo: false), prompts: prompts), prompts)
	}

	private static func open(_ model: ViewerModel, _ url: URL) {
		model.open(url)
		check(waitUntil { !model.isBusy }, "the open did not finish")
	}

	private static func save(_ model: ViewerModel, _ url: URL, binary: Bool = false) {
		model.save(to: url, binary: binary)
		check(waitUntil { !model.isBusy }, "the save did not finish")
	}

	/// The labelled sample without its pitch length, which the venue checks refuse: it stores no
	/// venue value, so its player edits can be saved.
	private static func playersJSON() -> Data {
		replacing(
			sampleJSON(FIXTURE_SAMPLE_LABELLED),
			"{\"name\":\"venue\",\"u32\":0},",
			with: "{\"u32\":0},"
		)
	}

	static func maximises() {
		let scratch = Scratch()
		let (model, prompts) = model()
		open(model, scratch.file("next.json", playersJSON()))
		guard let fixture = model.fixture else {
			check(false, "no fixture")
			return
		}
		// Nothing selected: the buttons are off and do nothing.
		check(!model.canEditPlayers(side: 0) && !model.canEditPlayers(side: 1))
		model.maxPlayers(.happiness, side: 0)
		check(!fixture.hasPlayerEdits && prompts.errors.isEmpty)

		model.select(side: 0, blocks: [0, 11])
		check(model.canEditPlayers(side: 0) && !model.canEditPlayers(side: 1))
		let revision = model.revision
		model.maxPlayers(.happiness, side: 0)
		check(model.revision > revision, "the lists were not told")
		checkEqual(fixture.cell(side: 0, row: 0, column: 5), "20")
		checkEqual(fixture.cell(side: 0, row: 11, column: 5), "20")
		checkEqual(fixture.cell(side: 0, row: 1, column: 5), "12")
		checkEqual(model.heading?.marker, " (unsaved player edits)")
		check(model.hasUnsavedEdits)
		checkEqual(model.selection, [[0, 11], []])

		model.maxPlayers(.condition, side: 0)
		checkEqual(
			(6...8).map { fixture.cell(side: 0, row: 0, column: $0) },
			["100.00%", "100.00%", "0"]
		)
		// The other side's lists show their own players.
		model.select(side: 1, blocks: [0])
		model.maxPlayers(.happiness, side: 1)
		checkEqual(fixture.cell(side: 1, row: 11, column: 5), "20")
		checkEqual(fixture.cell(side: 1, row: 11, column: 6), "98.76%")
		let flags = fixture.playerEdits
		checkEqual(
			flags[0],
			UInt8(FIXTURE_VIEW_MAX_HAPPINESS.rawValue | FIXTURE_VIEW_MAX_CONDITION.rawValue)
		)
		checkEqual(flags[11], flags[0])
		checkEqual(flags[1], 0)
		checkEqual(flags[FixtureHandle.blockCount], UInt8(FIXTURE_VIEW_MAX_HAPPINESS.rawValue))
		check(prompts.errors.isEmpty, "\(prompts.errors)")
		check(model.log.lines.last?.hasSuffix("Max happiness: 1 away player") == true)

		// Without familiarity data nothing changes, and the reason is the Windows viewer's.
		model.maxPlayers(.familiarity, side: 0)
		checkEqual(prompts.errors.last?.title, "FM24 Match Harness")
		checkEqual(
			prompts.lastError,
			"The selected player or tactic is missing tactical familiarity data (export the fixture "
				+ "again); no players were changed."
		)
		checkEqual(fixture.playerEdits, flags)
		checkEqual(
			PlayerEdit.allCases.map(\.title),
			["Max happiness", "Max condition", "Max familiarity"]
		)
	}

	static func allOrNothing() {
		let scratch = Scratch()
		let (model, prompts) = model()
		open(model, scratch.file("familiar.json", sampleJSON(FIXTURE_SAMPLE_FAMILIARITY)))
		guard let fixture = model.fixture else {
			check(false, "no fixture")
			return
		}
		let familiar = (9..<16).map { fixture.cell(side: 0, row: 1, column: $0) }
		// Player 0 has no familiarity entries; player 1 has, but is not changed either.
		model.select(side: 0, blocks: [0, 1])
		model.maxPlayers(.familiarity, side: 0)
		check(prompts.lastError?.hasPrefix("The selected player or tactic is missing") == true)
		checkEqual((9..<16).map { fixture.cell(side: 0, row: 1, column: $0) }, familiar)
		check(!fixture.hasPlayerEdits && !model.hasUnsavedEdits)
		model.select(side: 0, blocks: [1])
		model.maxPlayers(.familiarity, side: 0)
		checkEqual(
			(9..<16).map { fixture.cell(side: 0, row: 1, column: $0) },
			[String](repeating: "100.00%", count: 7)
		)
		checkEqual(prompts.errors.count, 1)

		// A player without all the wellbeing values cannot be maximised, nor anyone selected with
		// them: player 0's unlabelled values, from sharpness[1] on, are not read when the sequence
		// they belong to does not have the expected types.
		let json = replacing(playersJSON(), "{\"u16\":10000}", with: "{\"u8\":100}")
		open(model, scratch.file("partial.json", json))
		guard let partial = model.fixture else {
			check(false, "no fixture")
			return
		}
		model.select(side: 0, blocks: [0, 1])
		model.maxPlayers(.condition, side: 0)
		checkEqual(
			prompts.lastError,
			"The selected player is missing wellbeing data; no players were changed."
		)
		checkEqual(partial.cell(side: 0, row: 1, column: 6), "98.76%")
		check(!partial.hasPlayerEdits)
		// Their morale is labelled, so happiness can be maximised.
		model.maxPlayers(.happiness, side: 0)
		checkEqual(partial.cell(side: 0, row: 0, column: 5), "20")
		checkEqual(prompts.errors.count, 2)
	}

	static func savesPlayerEdits() throws {
		let scratch = Scratch()
		let json = playersJSON()
		let (model, prompts) = model()
		open(model, scratch.file("next.json", json))
		guard let fixture = model.fixture else {
			check(false, "no fixture")
			return
		}
		model.select(side: 0, blocks: [0, 11])
		model.maxPlayers(.happiness, side: 0)
		model.maxPlayers(.condition, side: 0)
		model.select(side: 1, blocks: [5])
		model.maxPlayers(.condition, side: 1)
		let flags = fixture.playerEdits
		let expected = try FixtureHandle.write(
			json: json,
			playerEdits: flags,
			venue: nil,
			binary: false
		)

		let saved = scratch.file("edited.json")
		prompts.destination = (saved, false)
		let revision = model.revision
		model.requestSave()
		check(waitUntil { !model.isBusy })
		check(prompts.errors.isEmpty, "\(prompts.errors)")
		checkEqual(try Data(contentsOf: saved), expected)
		checkEqual(try Data(contentsOf: scratch.file("next.json")), json)
		// The saved file is on show, with the edits written and none pending.
		checkEqual(model.heading, SummaryHeading(name: "edited.json", marker: ""))
		check(!fixture.hasPlayerEdits && !model.hasUnsavedEdits)
		check(model.revision > revision)
		checkEqual(model.source?.json, expected)
		checkEqual(model.selection, [[0, 11], [5]])
		let reopened = try FixtureHandle(json: try Data(contentsOf: saved))
		for side in 0..<2 {
			for row in 0..<fixture.rowCount(side: side) {
				checkEqual(
					(0..<16).map { reopened.cell(side: side, row: row, column: $0) },
					(0..<16).map { fixture.cell(side: side, row: row, column: $0) }
				)
			}
		}
		checkEqual(reopened.cell(side: 0, row: 11, column: 5), "20")

		// Edits saved as binary are encoded after they are written.
		model.select(side: 0, blocks: [3])
		model.maxPlayers(.happiness, side: 0)
		let binary = scratch.file("edited.bin")
		let written = try FixtureHandle.write(
			json: expected,
			playerEdits: fixture.playerEdits,
			venue: nil,
			binary: false
		)
		save(model, binary, binary: true)
		checkEqual(
			try Data(contentsOf: binary),
			try FixtureHandle.write(json: written, playerEdits: nil, venue: nil, binary: true)
		)
		checkEqual(model.source?.json, written)
		check(!model.hasUnsavedEdits)
		check(prompts.errors.isEmpty, "\(prompts.errors)")
	}

	static func editsVenue() throws {
		let scratch = Scratch()
		let json = sampleJSON(FIXTURE_SAMPLE_VENUE)
		let (model, prompts) = model()
		open(model, scratch.file("venue.json", json))
		guard let fixture = model.fixture else {
			check(false, "no fixture")
			return
		}
		let stored = fixture.venue
		checkEqual(model.venue, VenueForm(stored))
		checkEqual(model.venue.texts[0], "67.5999985")
		checkEqual(model.venue.surface, 1)
		check(!model.venue.dirty)
		// The same text is no change.
		model.setVenueText("67.5999985", field: 0)
		check(!model.hasUnsavedEdits)
		model.setVenueText("72.5", field: 0)
		check(model.venue.dirty && model.hasUnsavedEdits)
		checkEqual(model.venue.texts[0], "72.5")
		model.setVenueText("40000", field: homeFans)
		model.setVenueSurface(0)
		checkEqual(model.heading?.marker, " (unsaved venue edits)")
		checkEqual(fixture.venue[0].value, stored[0].value)

		var values = stored
		values[0].value = UInt64(Float(72.5).bitPattern)
		values[homeFans].value = 40000
		values[surface].value = 1
		let expected = try FixtureHandle.write(
			json: json,
			playerEdits: nil,
			venue: values,
			binary: false
		)
		let saved = scratch.file("saved.json")
		save(model, saved)
		check(prompts.errors.isEmpty, "\(prompts.errors)")
		checkEqual(try Data(contentsOf: saved), expected)
		// The saved values are the fixture's and the form shows them as stored.
		checkEqual(model.heading, SummaryHeading(name: "saved.json", marker: ""))
		checkEqual(model.venue, VenueForm(values))
		checkEqual(model.venue.texts[0], "72.5")
		checkEqual(model.venue.surface, 0)
		check(fixture.venue.map(\.value) == values.map(\.value))
		let reopened = try FixtureHandle(json: try Data(contentsOf: saved))
		check(reopened.venue.map(\.value) == values.map(\.value))
		checkEqual(FixtureHandle.venueText(surface, reopened.venue[surface]), "Grass")

		// Saving again writes the same values, which are now stored.
		let again = scratch.file("again.json")
		save(model, again)
		checkEqual(try Data(contentsOf: again), expected)
	}

	static func limitsVenue() {
		let scratch = Scratch()
		let (model, _) = model()
		// Before any fixture, and for a field the fixture does not store, nothing changes.
		model.setVenueText("1", field: capacity)
		checkEqual(model.venue, VenueForm())
		open(model, scratch.file("next.json", sampleJSON(FIXTURE_SAMPLE_LABELLED)))
		check(!(0..<FixtureHandle.venueFieldCount).contains(where: model.venueEditable))
		model.setVenueText("1", field: capacity)
		model.setVenueSurface(0)
		check(!model.venue.dirty)
		checkEqual(model.venue.texts[1], "0")
		checkEqual(model.venue.surface, nil)

		open(model, scratch.file("venue.json", sampleJSON(FIXTURE_SAMPLE_VENUE)))
		check((0..<FixtureHandle.venueFieldCount).allSatisfy(model.venueEditable))
		model.setVenueText(String(repeating: "1", count: 70), field: capacity)
		checkEqual(model.venue.texts[capacity], String(repeating: "1", count: 63))
		checkEqual(VenueForm.limited(String(repeating: "😀", count: 40)).count, 31)
		checkEqual(VenueForm.limited("short"), "short")
		// The surface's text is its name; it changes only through its choices.
		model.setVenueText("Grass", field: surface)
		checkEqual(model.venue.texts[surface], "Grass/Synthetic mix")
		model.setVenueSurface(5)
		checkEqual(model.venue.surface, nil)
	}

	static func refusesVenue() throws {
		let scratch = Scratch()
		let (model, prompts) = model()
		open(model, scratch.file("venue.json", sampleJSON(FIXTURE_SAMPLE_VENUE)))
		let saved = scratch.file("saved.json")
		prompts.destination = (saved, false)

		// An entry that is not a number: the Venue tab, the field focused, no Save panel.
		model.setVenueText("4x", field: capacity)
		model.requestSave()
		checkEqual(prompts.lastError, "Capacity: Enter a valid whole number.")
		checkEqual(model.tab, .venue)
		checkEqual(model.venueFocus?.field, capacity)
		check(prompts.offered == nil, "the Save panel was offered")
		// A save straight to a file checks the same; the view clears the request once focused.
		model.tab = .squads
		save(model, saved)
		checkEqual(prompts.errors.count, 2)
		checkEqual(model.tab, .venue)
		check(!FileManager.default.fileExists(atPath: saved.path))
		if let request = model.venueFocus {
			model.focusedVenue(ViewerModel.VenueFocus(field: request.field, serial: request.serial - 1))
			checkEqual(model.venueFocus, request)
			model.focusedVenue(request)
		}
		checkEqual(model.venueFocus, nil)

		model.setVenueText("1.5", field: homeFans)
		model.setVenueText("42777", field: capacity)
		model.requestSave()
		checkEqual(prompts.lastError, "Home fans: Enter a valid whole number.")
		checkEqual(model.venueFocus?.field, homeFans)
		model.setVenueText("", field: 0)
		model.requestSave()
		checkEqual(prompts.lastError, "Pitch width (m): Enter a valid number.")
		checkEqual(model.venueFocus?.field, 0)
		model.setVenueText("67.6", field: 0)

		// The checks across fields: no field to focus.
		model.focusedVenue(model.venueFocus ?? ViewerModel.VenueFocus(field: 0, serial: 0))
		model.setVenueText("41000", field: homeFans)
		model.tab = .squads
		model.requestSave()
		checkEqual(prompts.lastError, "Home fans + Away fans cannot exceed Capacity.")
		checkEqual(model.tab, .venue)
		checkEqual(model.venueFocus, nil)
		check(prompts.offered == nil, "the Save panel was offered")
		model.setVenueText("30000", field: homeFans)
		model.requestSave()
		check(waitUntil { !model.isBusy })
		check(prompts.offered != nil)
		check(FileManager.default.fileExists(atPath: saved.path))
		checkEqual(prompts.errors.count, 5)

		// A stored surface that is neither choice must be chosen first.
		let other = replacing(
			sampleJSON(FIXTURE_SAMPLE_VENUE),
			"{\"u8\": 8,\"name\":\"venue.surface\"",
			with: "{\"u8\": 2,\"name\":\"venue.surface\""
		)
		open(model, scratch.file("other.json", other))
		checkEqual(model.venue.surface, nil)
		check(model.venueEditable(surface))
		save(model, scratch.file("other-saved.json"))
		checkEqual(prompts.lastError, "Surface: Enter a valid surface selection.")
		checkEqual(model.venueFocus?.field, surface)
		model.setVenueSurface(1)
		save(model, scratch.file("other-saved.json"))
		checkEqual(prompts.errors.count, 6)
		checkEqual(model.fixture?.venue[surface].value, 8)
	}

	static func asksBeforeOpening() {
		let scratch = Scratch()
		let (model, prompts) = model()
		let first = scratch.file("first.json", playersJSON())
		let second = scratch.file("second.json", sampleJSON(FIXTURE_SAMPLE_VENUE))
		// Nothing to lose: no question.
		model.requestOpen(first)
		check(waitUntil { !model.isBusy })
		model.requestOpen(second)
		check(waitUntil { !model.isBusy })
		check(prompts.questions.isEmpty)
		checkEqual(model.heading?.name, "second.json")

		// Venue edits: Cancel keeps them.
		model.setVenueText("1", field: 10)
		prompts.answers = [false]
		model.requestOpen(first)
		check(!model.isBusy)
		checkEqual(prompts.questions.count, 1)
		checkEqual(prompts.questions.first?.title, "Open fixture")
		checkEqual(
			prompts.questions.first?.message,
			"The current fixture has unsaved changes. Discard them and open the selected fixture?"
		)
		checkEqual(prompts.questions.first?.action, "Discard")
		checkEqual(model.heading?.name, "second.json")
		check(model.venue.dirty)
		// An unusable path is refused before asking.
		model.requestOpen(scratch.file("notes.txt"))
		checkEqual(prompts.questions.count, 1)
		checkEqual(prompts.lastError, FixtureFiles.pathProblem)
		// The Open panel asks too; Discard opens the chosen fixture.
		prompts.fixtureToOpen = first
		prompts.answers = [true]
		model.requestOpen()
		check(waitUntil { !model.isBusy })
		checkEqual(prompts.questions.count, 2)
		checkEqual(model.heading, SummaryHeading(name: "first.json", marker: ""))

		// Player edits ask as well.
		model.select(side: 0, blocks: [0])
		model.maxPlayers(.happiness, side: 0)
		model.requestOpen(second)
		checkEqual(prompts.questions.count, 3)
		checkEqual(model.heading?.name, "first.json")
		// So does a temporary fixture, even unedited.
		model.open(first, temporary: true)
		check(waitUntil { !model.isBusy })
		check(!model.hasUnsavedEdits)
		model.requestOpen(second)
		checkEqual(prompts.questions.count, 4)
		checkEqual(model.source?.temporary, true)
	}

	static func asksBeforeClosing() {
		let scratch = Scratch()
		let (model, prompts) = model()
		var answers: [Bool] = []
		checkEqual(model.closeCheck(), .close)
		open(model, scratch.file("next.json", playersJSON()))
		checkEqual(model.closeCheck(), .close)
		model.confirmClose { answers.append($0) }
		checkEqual(answers, [true])
		check(prompts.questions.isEmpty)

		model.select(side: 1, blocks: [0])
		model.maxPlayers(.condition, side: 1)
		checkEqual(model.closeCheck(), .confirm)
		prompts.answers = [false, true]
		model.confirmClose { answers.append($0) }
		model.confirmClose { answers.append($0) }
		checkEqual(answers, [true, false, true])
		checkEqual(prompts.questions.count, 2)
		checkEqual(prompts.questions.first?.title, "Close fixture")
		checkEqual(
			prompts.questions.first?.message,
			"Fixture changes are not saved. Discard them and close?"
		)
		checkEqual(prompts.questions.first?.action, "Discard")

		// Venue edits alone ask too; an unedited temporary fixture closes without asking.
		open(model, scratch.file("venue.json", sampleJSON(FIXTURE_SAMPLE_VENUE)))
		checkEqual(model.closeCheck(), .close)
		model.setVenueSurface(0)
		checkEqual(model.closeCheck(), .confirm)
		model.open(scratch.file("next.json"), temporary: true)
		checkEqual(
			model.closeCheck(),
			.blocked("A fixture file operation is running. Please wait before closing.")
		)
		model.confirmClose { answers.append($0) }
		checkEqual(answers.last, false)
		checkEqual(
			prompts.lastError,
			"A fixture file operation is running. Please wait before closing."
		)
		check(waitUntil { !model.isBusy })
		checkEqual(model.source?.temporary, true)
		checkEqual(model.closeCheck(), .close)
		checkEqual(prompts.questions.count, 2)
		checkEqual(answers.count, 4)
	}

	static func locksEdits() {
		let scratch = Scratch()
		let (model, prompts) = model()
		open(model, scratch.file("venue.json", sampleJSON(FIXTURE_SAMPLE_VENUE)))
		model.open(scratch.file("next.json", playersJSON()))
		check(model.isBusy)
		check(!(0..<FixtureHandle.venueFieldCount).contains(where: model.venueEditable))
		model.setVenueText("1", field: capacity)
		check(!model.venue.dirty)
		check(waitUntil { !model.isBusy })
		model.select(side: 0, blocks: [0])
		model.save(to: scratch.file("saved.json"), binary: false)
		check(model.isBusy)
		check(!model.canEditPlayers(side: 0))
		model.maxPlayers(.happiness, side: 0)
		model.select(side: 0, blocks: [0, 1])
		check(waitUntil { !model.isBusy })
		check(model.fixture?.hasPlayerEdits == false)
		checkEqual(model.selection, [[0], []])
		check(prompts.errors.isEmpty, "\(prompts.errors)")
	}
}
