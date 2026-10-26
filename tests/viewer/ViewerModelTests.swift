// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureSamples
import Foundation

/// ViewerModel: opening and saving fixtures, with the Windows viewer's rules and messages.
@MainActor
enum ViewerModelTests {
	static func register() {
		Tests.test("viewer opens a JSON fixture and shows it", opens)
		Tests.test("viewer keeps the fixture on show when an open fails", keepsFixture)
		Tests.test("viewer refuses files it cannot open", refusesFiles)
		Tests.test("viewer runs one file operation at a time", oneAtATime)
		Tests.test("viewer saves as JSON and as binary", saves)
		Tests.test("viewer copies a fixture with nothing to write", copies)
		Tests.test("viewer leaves the destination alone when a save fails", failedSaves)
		Tests.test("viewer selects only players that can be selected", selects)
		Tests.test("viewer marks unsaved edits in the summary", marksEdits)
	}

	private static func model() -> (ViewerModel, FakePrompts) {
		let prompts = FakePrompts()
		return (ViewerModel(log: ViewerLog(echo: false), prompts: prompts), prompts)
	}

	/// Opens `url` and waits for the open to finish.
	private static func open(_ model: ViewerModel, _ url: URL) {
		model.open(url)
		check(waitUntil { !model.isBusy }, "the open did not finish")
	}

	private static func save(_ model: ViewerModel, _ url: URL, binary: Bool) {
		model.save(to: url, binary: binary)
		check(waitUntil { !model.isBusy }, "the save did not finish")
	}

	static func opens() {
		let scratch = Scratch()
		let url = scratch.file("next.json", sampleJSON(FIXTURE_SAMPLE_LABELLED))
		let (model, prompts) = model()
		var noted: [URL] = []
		model.noteFile = { noted.append($0) }
		checkEqual(model.title, "FM24 Match Harness (protocol \(FixtureHandle.protocolVersion))")
		check(model.fixture == nil && model.heading == nil)
		model.open(url)
		checkEqual(model.busy, .file)
		check(waitUntil { !model.isBusy })
		checkEqual(model.heading, SummaryHeading(name: "next.json", marker: ""))
		checkEqual(model.source?.url, url)
		checkEqual(model.source?.kind, .json)
		checkEqual(model.source?.temporary, false)
		checkEqual(model.fixture?.teamName(side: 0), "Aston Villa")
		checkEqual(noted, [url])
		check(prompts.errors.isEmpty)
		check(model.log.lines.last?.hasSuffix("Opened next.json") == true)
		// The Open panel leads to the same open.
		let other = scratch.file("other.json", sampleJSON(FIXTURE_SAMPLE_SWAPPED))
		prompts.fixtureToOpen = other
		model.requestOpen()
		check(waitUntil { !model.isBusy })
		checkEqual(model.heading?.name, "other.json")
		checkEqual(model.fixture?.rowCount(side: 0), 23)
	}

	static func keepsFixture() {
		let scratch = Scratch()
		let (model, prompts) = model()
		open(model, scratch.file("next.json", sampleJSON(FIXTURE_SAMPLE_LABELLED)))
		let shown = model.fixture
		open(model, scratch.file("broken.json", Data("{}".utf8)))
		checkEqual(prompts.errors.count, 1)
		checkEqual(prompts.errors.first?.title, "FM24 Match Harness")
		check(prompts.lastError?.isEmpty == false)
		check(model.fixture === shown)
		checkEqual(model.heading?.name, "next.json")
		check(model.log.lines.last?.contains("Error: ") == true)
		open(model, scratch.file("missing.json"))
		checkEqual(prompts.lastError, "cannot read fixture")
		check(model.fixture === shown)
	}

	static func refusesFiles() {
		let scratch = Scratch()
		let (model, prompts) = model()
		model.open(scratch.file("fixture.txt", Data("{}".utf8)))
		check(!model.isBusy)
		checkEqual(
			prompts.lastError,
			"Select a .json or .bin fixture with a path shorter than 1024 characters."
		)
		let long = URL(fileURLWithPath: "/" + String(repeating: "a", count: 1020) + ".json")
		model.open(long)
		checkEqual(prompts.errors.count, 2)
		checkEqual(prompts.lastError, FixtureFiles.pathProblem)
		// Upper-case extensions are fixtures too; binary ones are decoded by the game, which this
		// model cannot reach.
		let binary = scratch.file("next.BIN", Data([1, 2, 3]))
		open(model, binary)
		checkEqual(prompts.lastError, ViewerModel.noHarness)
		check(model.fixture == nil)
		checkEqual(model.status, ViewerModel.noHarness)
		checkEqual(model.retry, .open(binary))
	}

	static func oneAtATime() {
		let scratch = Scratch()
		let (model, prompts) = model()
		let url = scratch.file("next.json", sampleJSON(FIXTURE_SAMPLE_LABELLED))
		model.open(url)
		// With no fixture on show yet, closing may stop the first open.
		checkEqual(model.closeCheck(), .close)
		model.open(url)
		checkEqual(prompts.lastError, "Please wait for the current fixture operation to finish.")
		check(waitUntil { !model.isBusy })
		model.open(url)
		checkEqual(
			model.closeCheck(),
			.blocked("A fixture file operation is running. Please wait before closing.")
		)
		model.requestSave()
		checkEqual(prompts.errors.count, 2)
		check(prompts.offered == nil)
		check(waitUntil { !model.isBusy })
		checkEqual(model.closeCheck(), .close)
		checkEqual(prompts.errors.count, 2)
	}

	static func saves() throws {
		let scratch = Scratch()
		let json = sampleJSON(FIXTURE_SAMPLE_VENUE)
		let source = scratch.file("venue.json", json)
		let (model, prompts) = model()
		var noted: [URL] = []
		model.noteFile = { noted.append($0) }
		open(model, source)
		let venue = try FixtureHandle(json: json).venue
		let written = try FixtureHandle.write(json: json, playerEdits: nil, venue: venue, binary: false)

		// The Save panel offers the file's own name, folder and format.
		let saved = scratch.file("saved.json")
		prompts.destination = (saved, false)
		model.requestSave()
		check(prompts.offered?.name == "venue.json" && prompts.offered?.binary == false)
		checkEqual(prompts.offered?.directory?.path, scratch.url.path)
		check(waitUntil { !model.isBusy })
		check(prompts.errors.isEmpty, "\(prompts.errors)")
		// Any stored venue value is written, which recomputes the values derived from them.
		checkEqual(try Data(contentsOf: saved), written)
		checkEqual(try Data(contentsOf: source), json)
		checkEqual(model.heading?.name, "saved.json")
		checkEqual(model.source?.json, written)
		checkEqual(noted.last, saved)

		// JSON to binary is encoded here, without the game.
		let binary = scratch.file("saved.bin")
		save(model, binary, binary: true)
		let encoded = try FixtureHandle.write(json: written, playerEdits: nil, venue: nil, binary: true)
		checkEqual(try Data(contentsOf: binary), encoded)
		checkEqual(model.source?.kind, .binary)
		checkEqual(model.source?.original, encoded)
		checkEqual(model.source?.json, written)
		model.requestSave()
		check(prompts.offered?.name == "saved.bin" && prompts.offered?.binary == true)
		check(waitUntil { !model.isBusy })
		// Binary back to JSON uses the JSON kept in memory, without decoding the file again.
		let again = scratch.file("again.json")
		save(model, again, binary: false)
		checkEqual(try Data(contentsOf: again), written)
		check(prompts.errors.isEmpty, "\(prompts.errors)")
		// A save whose format does not match the file's extension is refused.
		save(model, scratch.file("mismatch.json"), binary: true)
		checkEqual(prompts.lastError, FixtureFiles.pathProblem)
	}

	static func copies() throws {
		// Without its pitch length, the sample stores no venue value: there is nothing to write.
		let json = replacing(
			sampleJSON(FIXTURE_SAMPLE_LABELLED),
			"{\"name\":\"venue\",\"u32\":0},",
			with: "{\"u32\":0},"
		)
		let scratch = Scratch()
		let (model, prompts) = model()
		open(model, scratch.file("plain.json", json))
		check(model.fixture.map { !FixtureHandle.hasVenue($0.venue) } == true)
		let copy = scratch.file("copy.json")
		save(model, copy, binary: false)
		checkEqual(try Data(contentsOf: copy), json)
		check(prompts.errors.isEmpty, "\(prompts.errors)")
	}

	static func failedSaves() throws {
		let scratch = Scratch()
		let (model, prompts) = model()
		// The labelled sample stores a pitch length of 0, which the venue checks refuse.
		open(model, scratch.file("next.json", sampleJSON(FIXTURE_SAMPLE_LABELLED)))
		let refused = scratch.file("refused.json")
		save(model, refused, binary: false)
		checkEqual(prompts.lastError, "Invalid venue: enter a finite positive pitch dimension.")
		check(!FileManager.default.fileExists(atPath: refused.path))
		checkEqual(model.heading?.name, "next.json")

		open(model, scratch.file("venue.json", sampleJSON(FIXTURE_SAMPLE_VENUE)))
		save(model, scratch.url.appendingPathComponent("missing/saved.json"), binary: false)
		checkEqual(prompts.lastError, "Cannot write the fixture. The destination was not changed.")
		// A folder that cannot be written to keeps the file already there.
		let locked = scratch.url.appendingPathComponent("locked")
		try FileManager.default.createDirectory(at: locked, withIntermediateDirectories: false)
		let existing = locked.appendingPathComponent("existing.json")
		try Data("old".utf8).write(to: existing)
		try FileManager.default.setAttributes([.posixPermissions: 0o555], ofItemAtPath: locked.path)
		defer {
			try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: locked.path)
		}
		save(model, existing, binary: false)
		checkEqual(prompts.lastError, "Cannot write the fixture. The destination was not changed.")
		checkEqual(try Data(contentsOf: existing), Data("old".utf8))
		checkEqual(model.heading?.name, "venue.json")
		let leftovers = try FileManager.default.contentsOfDirectory(atPath: locked.path)
		checkEqual(leftovers, ["existing.json"])
	}

	static func selects() {
		let scratch = Scratch()
		let (model, _) = model()
		model.select(side: 0, blocks: [1])
		checkEqual(model.selection, [[], []])
		open(model, scratch.file("next.json", sampleJSON(FIXTURE_SAMPLE_LABELLED)))
		model.select(side: 1, blocks: [0, 5, 11, 12, 300])
		checkEqual(model.selection, [[], [0, 5, 11]])
		model.select(side: 0, blocks: [2])
		checkEqual(model.selection, [[2], [0, 5, 11]])
		// No change while a file operation runs; a new fixture starts with nothing selected.
		model.open(scratch.file("again.json", sampleJSON(FIXTURE_SAMPLE_VENUE)))
		model.select(side: 0, blocks: [3])
		checkEqual(model.selection, [[2], [0, 5, 11]])
		check(waitUntil { !model.isBusy })
		checkEqual(model.selection, [[], []])
		// The venue sample's players have no names, so none can be selected.
		model.select(side: 0, blocks: [0, 1])
		checkEqual(model.selection, [[], []])
	}

	static func marksEdits() {
		checkEqual(SummaryHeading.marker(venue: false, players: false), "")
		checkEqual(SummaryHeading.marker(venue: true, players: false), " (unsaved venue edits)")
		checkEqual(SummaryHeading.marker(venue: false, players: true), " (unsaved player edits)")
		checkEqual(
			SummaryHeading.marker(venue: true, players: true),
			" (unsaved venue and player edits)"
		)
		checkEqual(SummaryHeading.prefix, "Viewing fixture: ")
	}
}
