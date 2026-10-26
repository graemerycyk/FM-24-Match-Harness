// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureSamples
import Foundation

/// ViewerModel through fm24ctl's stand-in: the startup export and its start panel, binary
/// fixtures decoded by the game, and the export of a fixture not yet saved, with the Windows
/// viewer's rules and messages and the game kept to the temporary directory.
@MainActor
enum HarnessModelTests {
	static func register() {
		Tests.test("viewer exports the next fixture at startup", exportsAtStartup)
		Tests.test("viewer says why the startup export failed, and retries", explainsStartup)
		Tests.test("viewer shows the harness waiting for another job", showsWaiting)
		Tests.test("viewer stops the startup export or first open on Cancel or quit", cancelsStart)
		Tests.test("viewer opens binary fixtures through the game's decode", decodes)
		Tests.test("viewer keeps the fixture on show when a decode fails", failedDecodes)
		Tests.test("viewer exports a fixture not yet saved", exports)
		Tests.test("viewer never overwrites a file when exporting", exportsWithoutOverwriting)
		Tests.test("viewer exports where a new name cannot be renamed into (exFAT)", exportsToExFAT)
		Tests.test("viewer exports pending edits and checks the venue first", exportsEdits)
		Tests.test("viewer opens fm24ctl's export as not yet saved", opensTemporaryFixture)
	}

	private typealias Setup = HarnessSetup

	private static func plainJSON() -> Data {
		plainSampleJSON()
	}

	private static let exportName = "aston-villa-v-bristol-city-1700000000.json"

	/// Exports the next fixture, as the stand-in makes it from `json`, and waits for it to show.
	private static func startup(_ setup: Setup, _ json: Data) {
		setup.standIn.set(["FIXTURE": setup.scratch.file("next-fixture.json", json).path])
		setup.model.exportNextFixture()
		check(waitUntil { !setup.model.isBusy }, "the export did not finish")
	}

	private static func wait(_ model: ViewerModel) {
		check(waitUntil { !model.isBusy }, "the operation did not finish")
	}

	static func exportsAtStartup() {
		guard let setup = Setup() else {
			return
		}
		let model = setup.model
		var noted: [URL] = []
		model.noteFile = { noted.append($0) }
		setup.standIn.set(["FIXTURE": setup.scratch.file("next-fixture.json", plainJSON()).path])
		model.exportNextFixture()
		checkEqual(model.busy, .file)
		checkEqual(model.activity, ViewerModel.exportingActivity)
		checkEqual(model.retry, .export)
		// With no fixture on show, closing stops the export (cancelsStart).
		checkEqual(model.closeCheck(), .close)
		check(model.canCancelStart)
		wait(model)
		check(!model.canCancelStart)
		checkEqual(model.activity, nil)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(model.fixture?.teamName(side: 0), "Aston Villa")
		check(model.isTemporary && model.heading == nil)
		checkEqual(model.source?.url, setup.model.work.temporaryFixture)
		checkEqual(model.source?.json, plainJSON())
		checkEqual(model.status, "")
		checkEqual(model.retry, nil)
		checkEqual(noted, [])
		checkEqual(
			setup.standIn.runs,
			[["get-next-fixture", setup.temporary.appendingPathComponent("temp-fixture.json").path]]
		)
		setup.checkTemporaryOnly()
		// An export not yet saved closes without asking, as on Windows.
		checkEqual(model.closeCheck(), .close)
		check(model.log.text.contains("fm24ctl: ok: stand-in"), model.log.text)
	}

	static func explainsStartup() {
		guard let setup = Setup(serving: false) else {
			return
		}
		let model = setup.model
		// The harness's own error line.
		setup.standIn.set(["STATUS": "1", "STDOUT": "error: no upcoming fixture\n"])
		model.exportNextFixture()
		wait(model)
		check(model.fixture == nil)
		checkEqual(model.status, "error: no upcoming fixture")
		checkEqual(model.retry, .export)
		checkEqual(model.activity, nil)
		check(setup.prompts.errors.isEmpty, "the start panel says why, without an alert")
		check(model.log.text.contains("Error: error: no upcoming fixture"), model.log.text)
		// No harness serving: fm24ctl's explanation, and the game's state for the start panel.
		let explanation =
			"Football Manager 2024 is not running.\nSet Steam's launch option for it once, then start "
			+ "it from Steam (`fm24ctl install` says how, and when Steam has saved the option):\n"
		setup.standIn.set(["STATUS": "1", "STDERR": explanation])
		model.retryStart()
		wait(model)
		checkEqual(
			model.status,
			"FM24 isn't running with harness \(FixtureHandle.protocolVersion).\nFootball Manager 2024 "
				+ "is not running."
		)
		checkEqual(setup.monitor.state, .notRunning)
		// Retry, with the game ready.
		setup.standIn.set(["FIXTURE": setup.scratch.file("next.json", plainJSON()).path])
		model.retryStart()
		wait(model)
		check(model.isTemporary)
		checkEqual(model.status, "")
		checkEqual(model.retry, nil)
		checkEqual(setup.standIn.runs.count, 3)
		check(setup.prompts.errors.isEmpty)
		// A model without fm24ctl says so.
		let alone = ViewerModel(
			log: ViewerLog(echo: false),
			work: WorkFiles(directory: setup.temporary)
		)
		alone.exportNextFixture()
		checkEqual(alone.status, ViewerModel.noHarness)
		checkEqual(alone.retry, .export)
		check(!alone.isBusy)
	}

	static func cancelsStart() throws {
		guard let setup = Setup() else {
			return
		}
		let model = setup.model
		let waiting = [
			"STDERR": HarnessResult.waitingNotice + "\n", "DELAY_MS": "1500",
			"FIXTURE": setup.scratch.file("next.json", plainJSON()).path,
		]
		// A binary fixture's decode, as the first open, waiting behind another job.
		let binary = setup.scratch.file("first.bin", Data([1, 2, 3]))
		setup.standIn.set(waiting)
		model.open(binary)
		check(waitUntil { model.activity == ViewerModel.waitingActivity }, "the decode did not wait")
		check(model.canCancelStart)
		checkEqual(model.closeCheck(), .close)
		model.cancelStart()
		check(!model.isBusy && !model.canCancelStart)
		checkEqual(model.activity, nil)
		checkEqual(model.status, ViewerModel.openCancelled)
		checkEqual(model.retry, .open(binary))
		// Its fm24ctl has ended: nothing arrives, and nothing is left in the work folder.
		RunLoop.main.run(until: Date().addingTimeInterval(2))
		check(model.fixture == nil)
		checkEqual(model.status, ViewerModel.openCancelled)
		checkEqual(setup.scratch.names("T/" + model.work.folder.lastPathComponent), [])
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")

		// The startup export, likewise; quitting cancels it too (AppDelegate).
		setup.standIn.set(waiting)
		model.exportNextFixture()
		check(waitUntil { model.activity == ViewerModel.waitingActivity }, "the export did not wait")
		var closes: [Bool] = []
		model.confirmClose { closes.append($0) }
		checkEqual(closes, [true])
		model.cancelStart()
		check(!model.isBusy)
		checkEqual(model.status, ViewerModel.exportCancelled)
		checkEqual(model.retry, .export)
		RunLoop.main.run(until: Date().addingTimeInterval(2))
		check(model.fixture == nil)
		check(
			!FileManager.default.fileExists(atPath: model.work.temporaryFixture.path),
			"the cancelled export was written"
		)
		check(model.log.text.contains(ViewerModel.exportCancelled), model.log.text)
		// Retry exports it.
		setup.standIn.set(["FIXTURE": setup.scratch.file("next.json").path])
		model.retryStart()
		wait(model)
		check(model.isTemporary)
		checkEqual(model.status, "")

		// With a fixture on show, a decode is a file operation like any other, as on Windows.
		setup.standIn.set(waiting)
		model.open(binary)
		check(!model.canCancelStart)
		checkEqual(
			model.closeCheck(),
			.blocked("A fixture file operation is running. Please wait before closing.")
		)
		model.cancelStart()
		checkEqual(model.busy, .file)
		wait(model)
		checkEqual(model.source?.kind, .binary)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	static func showsWaiting() {
		guard let setup = Setup() else {
			return
		}
		let model = setup.model
		setup.standIn.set([
			"FIXTURE": setup.scratch.file("next.json", plainJSON()).path,
			"STDERR": HarnessResult.waitingNotice + "\n",
			"DELAY_MS": "600",
		])
		model.exportNextFixture()
		check(
			waitUntil { model.activity == ViewerModel.waitingActivity },
			"the activity did not say it was waiting"
		)
		check(model.isBusy)
		wait(model)
		checkEqual(model.activity, nil)
		check(model.isTemporary)
	}

	static func decodes() throws {
		guard let setup = Setup() else {
			return
		}
		let model = setup.model
		let json = plainJSON()
		let bytes = try FixtureHandle.write(json: json, playerEdits: nil, venue: nil, binary: true)
		let binary = setup.scratch.file("Next Fixture.bin", bytes)
		var noted: [URL] = []
		model.noteFile = { noted.append($0) }
		setup.standIn.set(["FIXTURE": setup.scratch.file("decoded.json", json).path, "DELAY_MS": "300"])
		model.open(binary)
		check(waitUntil { model.activity == ViewerModel.decodingActivity }, "no decoding activity")
		wait(model)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(model.source?.kind, .binary)
		checkEqual(model.source?.url, binary)
		checkEqual(model.source?.original, bytes)
		checkEqual(model.source?.json, json)
		checkEqual(model.source?.temporary, false)
		checkEqual(model.heading?.name, "Next Fixture.bin")
		checkEqual(model.fixture?.teamName(side: 1), "Bristol City")
		checkEqual(noted, [binary])
		// The game decoded a copy in the viewer's folder, which is gone now.
		let folder = model.work.folder.path
		checkEqual(
			setup.standIn.runs,
			[["decode", folder + "/in-1.bin", folder + "/decoded-2.json"]]
		)
		setup.checkTemporaryOnly()
		checkEqual(setup.scratch.names("T/" + model.work.folder.lastPathComponent), [])
		// Saving needs no second decode: as JSON from the JSON kept, as binary from its bytes.
		let saved = setup.scratch.file("saved.json")
		model.save(to: saved, binary: false)
		wait(model)
		checkEqual(try Data(contentsOf: saved), json)
		model.open(binary)
		wait(model)
		let copy = setup.scratch.file("copy.bin")
		model.save(to: copy, binary: true)
		wait(model)
		checkEqual(try Data(contentsOf: copy), bytes)
		checkEqual(setup.standIn.runs.count, 2)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	static func failedDecodes() {
		guard let setup = Setup() else {
			return
		}
		let model = setup.model
		let shown = setup.scratch.file("shown.json", plainJSON())
		model.open(shown)
		wait(model)
		let binary = setup.scratch.file("next.bin", Data([1, 2, 3]))
		setup.standIn.set(["STATUS": "1", "STDOUT": "error: the input is not a match config\n"])
		model.open(binary)
		wait(model)
		checkEqual(setup.prompts.lastError, "error: the input is not a match config")
		checkEqual(model.source?.url, shown)
		checkEqual(model.retry, nil)
		// Nothing to say: the Windows viewer's message, pointing at the log.
		setup.standIn.set(["STATUS": "1", "STDOUT": ""])
		model.open(binary)
		wait(model)
		checkEqual(
			setup.prompts.lastError,
			"cannot decode binary fixture; FM24 must be running and idle. See the log and harness.log."
		)
		// A decode that writes nothing.
		setup.standIn.set([
			"STDOUT": "ok\n", "FIXTURE": setup.scratch.url.appendingPathComponent("none").path,
		])
		model.open(binary)
		wait(model)
		checkEqual(setup.prompts.lastError, "error: cannot write the fixture")
		// A file that cannot be read never reaches the game.
		model.open(setup.scratch.url.appendingPathComponent("missing.bin"))
		wait(model)
		checkEqual(setup.prompts.lastError, "cannot read fixture")
		checkEqual(setup.standIn.runs.count, 3)
		checkEqual(setup.scratch.names("T/" + model.work.folder.lastPathComponent), [])
		checkEqual(model.source?.url, shown)
	}

	static func exports() throws {
		guard let setup = Setup() else {
			return
		}
		let model = setup.model
		var noted: [URL] = []
		model.noteFile = { noted.append($0) }
		startup(setup, plainJSON())
		check(model.canExport)
		// Save offers the export's name, in the export folder once it exists.
		setup.prompts.destination = nil
		model.requestSave()
		checkEqual(setup.prompts.offered?.name, exportName)
		checkEqual(setup.prompts.offered?.directory, nil)
		try FileManager.default.createDirectory(at: setup.exports, withIntermediateDirectories: true)
		model.requestSave()
		checkEqual(setup.prompts.offered?.directory?.path, setup.exports.path)
		try FileManager.default.removeItem(at: setup.exports)

		// The export link: the folder is made, and the fixture counts as saved.
		model.export()
		checkEqual(model.busy, .file)
		check(!model.canExport)
		wait(model)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		let exported = setup.exports.appendingPathComponent(exportName)
		checkEqual(try Data(contentsOf: exported), plainJSON())
		checkEqual(setup.scratch.names("Exports"), [exportName])
		check(!model.isTemporary && !model.canExport)
		checkEqual(model.heading, SummaryHeading(name: exportName, marker: ""))
		checkEqual(model.source?.url, exported)
		checkEqual(noted, [exported])
		// Exporting again does nothing: the fixture is saved.
		model.export()
		check(!model.isBusy)
		checkEqual(setup.standIn.runs.count, 1)

		// The export folder is chosen with a folder panel.
		let chosen = setup.scratch.url.appendingPathComponent("Chosen")
		setup.prompts.folderToChoose = chosen
		model.requestExportFolder()
		checkEqual(setup.prompts.offeredFolder?.path, setup.exports.path)
		checkEqual(model.settings.exportFolder.path, chosen.path)
		setup.prompts.folderToChoose = nil
		model.requestExportFolder()
		checkEqual(model.settings.exportFolder.path, chosen.path)
	}

	static func exportsWithoutOverwriting() throws {
		guard let setup = Setup() else {
			return
		}
		let model = setup.model
		startup(setup, plainJSON())
		try FileManager.default.createDirectory(at: setup.exports, withIntermediateDirectories: true)
		let existing = setup.exports.appendingPathComponent(exportName)
		try Data("theirs".utf8).write(to: existing)
		model.export()
		wait(model)
		checkEqual(
			setup.prompts.lastError,
			"Cannot export fixture (File exists). No file was overwritten."
		)
		checkEqual(try Data(contentsOf: existing), Data("theirs".utf8))
		checkEqual(setup.scratch.names("Exports"), [exportName])
		check(model.isTemporary)
		// A folder that cannot be made.
		try FileManager.default.removeItem(at: existing)
		try Data().write(to: setup.scratch.url.appendingPathComponent("File"))
		model.settings.exportFolder = setup.scratch.url.appendingPathComponent("File/Exports")
		model.export()
		wait(model)
		checkEqual(
			setup.prompts.lastError,
			"Cannot create the export folder \(setup.scratch.url.path)/File/Exports."
		)
		check(model.isTemporary)
		// Not during a file operation.
		model.settings.exportFolder = setup.exports
		model.open(setup.scratch.file("other.json", plainJSON()))
		model.export()
		checkEqual(
			setup.prompts.lastError,
			"Please wait for the fixture file operation to finish before exporting."
		)
		wait(model)
	}

	static func exportsToExFAT() throws {
		let scratch = Scratch()
		// renamex_np's RENAME_EXCL fails so on exFAT: the file is created in place instead.
		var renames = 0
		let unsupported = { (_: String, _: String) -> Int32 in
			renames += 1
			return ENOTSUP
		}
		let target = scratch.file("export.json")
		try FixtureFiles.create(target, with: Data("ours".utf8), renameNew: unsupported)
		checkEqual(renames, 1)
		checkEqual(try Data(contentsOf: target), Data("ours".utf8))
		checkEqual(scratch.names(), ["export.json"])
		// Still never over a file that is there.
		do {
			try FixtureFiles.create(target, with: Data("again".utf8), renameNew: unsupported)
			check(false, "an existing file was replaced")
		} catch {
			checkEqual(
				(error as? ViewerError)?.message,
				"Cannot export fixture (File exists). No file was overwritten."
			)
		}
		checkEqual(try Data(contentsOf: target), Data("ours".utf8))
		checkEqual(scratch.names(), ["export.json"])
		// Any other failure to rename is the export's, leaving nothing behind.
		do {
			try FixtureFiles.create(scratch.file("other.json"), with: Data("x".utf8)) { _, _ in EACCES }
			check(false, "the export did not fail")
		} catch {
			checkEqual(
				(error as? ViewerError)?.message,
				"Cannot export fixture (Permission denied). No file was overwritten."
			)
		}
		checkEqual(scratch.names(), ["export.json"])
	}

	static func exportsEdits() throws {
		guard let setup = Setup() else {
			return
		}
		let model = setup.model
		// The labelled sample's pitch length is 0, which the venue checks refuse.
		startup(setup, sampleJSON(FIXTURE_SAMPLE_LABELLED))
		model.export()
		checkEqual(setup.prompts.lastError, "Invalid venue: enter a finite positive pitch dimension.")
		checkEqual(model.tab, .venue)
		check(!model.isBusy && model.isTemporary)
		checkEqual(setup.scratch.names("Exports"), [])

		model.tab = .squads
		startup(setup, plainJSON())
		guard let fixture = model.fixture else {
			check(false, "no fixture")
			return
		}
		model.select(side: 0, blocks: [0])
		model.maxPlayers(.happiness, side: 0)
		check(model.hasUnsavedEdits)
		let edits = fixture.playerEdits
		model.export()
		wait(model)
		check(setup.prompts.errors.count == 1, "\(setup.prompts.errors)")
		let written = try FixtureHandle.write(
			json: plainJSON(),
			playerEdits: edits,
			venue: fixture.venue,
			binary: false
		)
		checkEqual(try Data(contentsOf: setup.exports.appendingPathComponent(exportName)), written)
		check(!model.hasUnsavedEdits && !model.isTemporary)
		checkEqual(model.source?.json, written)
		checkEqual(fixture.cell(side: 0, row: 0, column: 5), "20")
	}

	static func opensTemporaryFixture() {
		guard let setup = Setup() else {
			return
		}
		let model = setup.model
		// What fm24ctl with no arguments exported and handed over.
		let exported = setup.temporary.appendingPathComponent("temp-fixture.json")
		try? plainJSON().write(to: exported)
		model.requestOpen(exported)
		wait(model)
		check(model.isTemporary)
		checkEqual(setup.standIn.runs.count, 0)
		// Another open asks first, as for unsaved edits.
		let other = setup.scratch.file("other.json", plainJSON())
		model.requestOpen(other)
		checkEqual(setup.prompts.questions.count, 1)
		checkEqual(setup.prompts.questions.first?.message, ViewerModel.openDiscardMessage)
		check(model.isTemporary)
		setup.prompts.answers = [true]
		model.requestOpen(other)
		wait(model)
		check(!model.isTemporary)
		checkEqual(model.heading?.name, "other.json")
		// The same export, by its folder's other spelling, then by the Open panel.
		let resolved = setup.temporary.resolvingSymlinksInPath().path
		let respelt =
			resolved.hasPrefix("/private/") ? String(resolved.dropFirst(8)) : "/private" + resolved
		model.requestOpen(URL(fileURLWithPath: respelt + "/temp-fixture.json"))
		wait(model)
		check(model.isTemporary)
	}
}
