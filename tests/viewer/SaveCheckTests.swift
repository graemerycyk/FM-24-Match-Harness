// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureSamples
import Foundation

/// The viewer with harness 1.3.0, through fm24ctl's stand-in: an export's "save" and "game_date"
/// kept in what the viewer writes (not in a binary copy) and shown in its summary, fm24ctl's check
/// of the fixture's save before a simulation, a replacement or a swap (another save loaded, with
/// Run Anyway, FM24 started with the fixture's save, or busy until it is idle, with fm24ctl's
/// progress), the harness refusing other jobs while the game loads or processes, and a run that
/// fails after fm24ctl's notices, each in fm24ctl's own words.
@MainActor
enum SaveCheckTests {
	static func register() {
		Tests.test("viewer keeps an export's save in what it writes and plays", keepsSave)
		Tests.test("viewer shows the save a fixture names", showsSave)
		Tests.test("viewer asks to run anyway while another save is loaded", stopsForOtherSave)
		Tests.test("viewer shows FM24 starting with the fixture's save", startsGame)
		Tests.test("viewer shows the harness refusing jobs while the game is busy", showsBusy)
		Tests.test("viewer shows fm24ctl waiting for FM24 to be idle with the save", waitsWhileBusy)
		Tests.test("a binary copy of an export names no save", dropsSaveFromBinary)
		Tests.test("fm24ctl's notices before a job do not explain its failure", skipsNotices)
	}

	private static let otherSave = "OTHER_FM24"
	private static let otherSaveRefusal =
		"FM24 has '\(otherSave)' loaded, not '\(FIXTURE_SAMPLE_SAVE)'; fm24ctl will not switch saves "
		+ "in a running game (unsaved progress). Quit FM24 (saving first only if you loaded that save "
		+ "yourself and have done no harness work since), then run this again, or pass --any-save "
		+ "to use the loaded save."
	private static let otherSaveQuestion =
		"FM24 has '\(otherSave)' loaded, not '\(FIXTURE_SAMPLE_SAVE)'; fm24ctl will not switch saves "
		+ "in a running game (unsaved progress). Quit FM24 (saving first only if you loaded that save "
		+ "yourself and have done no harness work since), then run this again, or choose Run Anyway "
		+ "to use the loaded save."
	private static let busyRefusal = "error: FM24 is busy (loading); wait until it is idle"

	private static func open(_ setup: HarnessSetup, _ url: URL) {
		setup.model.open(url)
		wait(setup.model)
	}

	private static func wait(_ model: ViewerModel) {
		check(waitUntil { !model.isBusy }, "the operation did not finish")
	}

	/// Starts a simulation of `seeds` and waits for it to end.
	private static func simulate(_ setup: HarnessSetup, seeds: String) {
		setup.model.setSeeds(seeds)
		setup.model.simulate()
		check(setup.model.simulation.running, "the simulation did not start")
		check(waitUntil { !setup.model.simulation.running }, "the simulation did not finish")
	}

	/// Replaces the away team with Luton Town (677) and waits for the replacement to end.
	private static func replaceAway(_ setup: HarnessSetup) {
		let model = setup.model
		model.editTeam(side: 1)
		check(waitUntil { model.teams.state != .loading }, "the team list did not load")
		model.teamEditor?.type("677")
		model.confirmTeam()
		wait(model)
	}

	/// Swaps away block 2 for the player 40347, once the player list has loaded (or failed to), and
	/// waits for the swap to end.
	private static func swapAway(_ setup: HarnessSetup) {
		let model = setup.model
		model.select(side: 1, blocks: [2])
		model.requestSwap(side: 1)
		guard let prompt = model.swapPrompt else {
			check(false, "no swap prompt")
			return
		}
		check(waitUntil { model.players.state != .loading }, "the player list did not load")
		prompt.type("40347")
		model.confirmSwap()
		wait(model)
	}

	static func keepsSave() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		// The startup export of the macOS harness records the save: the copy the game plays keeps it,
		// with the fixture's edits.
		setup.standIn.set([
			"FIXTURE": setup.scratch.file("next.json", plainSampleJSON(saved: true)).path
		])
		model.exportNextFixture()
		wait(model)
		check(model.isTemporary)
		checkEqual(SaveOrigin(model.source?.json), .sample)
		model.select(side: 0, blocks: [0])
		model.maxPlayers(.happiness, side: 0)
		let played = setup.scratch.url.appendingPathComponent("played.json")
		setup.standIn.set(["INPUT": played.path])
		simulate(setup, seeds: "1")
		checkEqual(model.simulation.outcome, .complete)
		checkEqual(setup.standIn.runs.last?[1].hasSuffix(".json"), true)
		checkEqual(SaveOrigin(try? Data(contentsOf: played)), .sample)
		// So do the copies a swap and a replacement give the game, with the other edits, and the
		// harness's result, which keeps the input's, is the fixture on show.
		let given = setup.scratch.url.appendingPathComponent("given.json")
		let result = setup.scratch.file("result.json", plainSampleJSON(saved: true))
		setup.standIn.set(["INPUT": given.path, "FIXTURE": result.path])
		swapAway(setup)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(SaveOrigin(try? Data(contentsOf: given)), .sample)
		checkEqual(SaveOrigin(model.source?.json), .sample)
		model.select(side: 0, blocks: [0])
		model.maxPlayers(.condition, side: 0)
		try FileManager.default.removeItem(at: given)
		replaceAway(setup)
		checkEqual(model.teamEditor?.error, nil)
		checkEqual(SaveOrigin(try? Data(contentsOf: given)), .sample)
		check(model.isTemporary)
		// Exporting and saving with edits keep it.
		model.select(side: 0, blocks: [0])
		model.maxPlayers(.happiness, side: 0)
		model.export()
		wait(model)
		let exported = setup.scratch.names("Exports").filter { $0.hasSuffix(".json") }
		checkEqual(exported.count, 1)
		checkEqual(
			SaveOrigin(try? Data(contentsOf: setup.exports.appendingPathComponent(exported[0]))),
			.sample
		)
		model.select(side: 0, blocks: [1])
		model.maxPlayers(.condition, side: 0)
		let saved = setup.scratch.file("saved.json")
		model.save(to: saved, binary: false)
		wait(model)
		check(!model.hasUnsavedEdits)
		checkEqual(SaveOrigin(try? Data(contentsOf: saved)), .sample)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")

		// A fixture that stores venue values: the copy the game plays, the result of a replacement
		// with the venue written into it, and a save.
		let venueJSON = savedSampleJSON(FIXTURE_SAMPLE_VENUE)
		setup.prompts.answers = [true]
		model.requestOpen(setup.scratch.file("venue.json", venueJSON))
		wait(model)
		let capacity = 4
		model.setVenueText("50000", field: capacity)
		try FileManager.default.removeItem(at: given)
		simulate(setup, seeds: "keep")
		checkEqual(model.simulation.outcome, .complete)
		checkEqual(SaveOrigin(try? Data(contentsOf: given)), .sample)
		setup.standIn.set([
			"INPUT": given.path, "SAVED": "1", "SAMPLE": String(FIXTURE_SAMPLE_VENUE.rawValue),
		])
		replaceAway(setup)
		checkEqual(model.teamEditor?.error, nil)
		checkEqual(model.fixture?.venue[capacity].value, 50000)
		checkEqual(SaveOrigin(model.source?.json), .sample)
		let venueSaved = setup.scratch.file("venue-saved.json")
		model.save(to: venueSaved, binary: false)
		wait(model)
		checkEqual(SaveOrigin(try? Data(contentsOf: venueSaved)), .sample)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		// What fm24ctl reads as no save at all.
		checkEqual(SaveOrigin(plainSampleJSON()), SaveOrigin(save: nil, date: nil))
		setup.checkTemporaryOnly()
	}

	static func showsSave() {
		let saved = try? FixtureHandle(json: savedSampleJSON(FIXTURE_SAMPLE_LABELLED))
		checkEqual(saved?.saveLine, "Save: \(FIXTURE_SAMPLE_SAVE), \(FIXTURE_SAMPLE_GAME_DATE)")
		checkEqual(try? FixtureHandle(json: plainSampleJSON()).saveLine, Optional<String>.none)
		let path = replacing(
			savedSampleJSON(FIXTURE_SAMPLE_LABELLED),
			"\"save\":\"\(FIXTURE_SAMPLE_SAVE)\"",
			with: "\"save\":\"/Saves/Sandbox Copy.fm\""
		)
		checkEqual(
			try? FixtureHandle(json: path).saveLine,
			"Save: Sandbox Copy, \(FIXTURE_SAMPLE_GAME_DATE)"
		)
	}

	/// The last run's arguments, and whether they end with --any-save.
	private static func ranAnySave(_ setup: HarnessSetup) -> Bool {
		setup.standIn.runs.last?.last == "--any-save"
	}

	static func stopsForOtherSave() throws {
		// fm24ctl's own message, whether or not a harness serves the job socket, with Run Anyway in
		// place of its --any-save: Cancel changes nothing, and Run Anyway runs the job again with
		// --any-save, in the save FM24 has loaded.
		for serving in [true, false] {
			guard let setup = HarnessSetup(serving: serving) else {
				return
			}
			let model = setup.model
			let prompts = setup.prompts
			let fixture = setup.scratch.file("next.json", plainSampleJSON(saved: true))
			open(setup, fixture)
			let earlier = setup.scratch.url.appendingPathComponent("next.csv")
			try Data("earlier results\n".utf8).write(to: earlier)
			// The fixtures a replacement or swap writes are for the fixture's save too.
			let result = setup.scratch.file("result.json", plainSampleJSON(saved: true))
			setup.standIn.set(["LOADED": otherSave, "FIXTURE": result.path])
			simulate(setup, seeds: "1-3")
			checkEqual(prompts.questions.count, 1)
			checkEqual(prompts.questions.last?.title, ViewerModel.errorTitle)
			checkEqual(prompts.questions.last?.message, otherSaveQuestion)
			checkEqual(prompts.questions.last?.action, ViewerModel.runAnywayAction)
			check(prompts.errors.isEmpty, "\(prompts.errors)")
			checkEqual(model.simulation.outcome, nil)
			checkEqual(model.simulation.results, [])
			checkEqual(model.activity, nil)
			checkEqual(try String(contentsOf: earlier, encoding: .utf8), "earlier results\n")
			check(!ranAnySave(setup))
			check(model.log.text.contains("Error: \(otherSaveRefusal)"), model.log.text)
			prompts.answers = [true]
			simulate(setup, seeds: "1-3")
			checkEqual(prompts.questions.count, 2)
			check(ranAnySave(setup))
			checkEqual(setup.standIn.runs.suffix(2).map { $0.first ?? "" }, ["run", "run"])
			checkEqual(model.simulation.outcome, .complete)
			checkEqual(model.simulation.completed, 3)
			check(
				model.log.text.contains(
					"fm24ctl: FM24 has '\(otherSave)' loaded, not '\(FIXTURE_SAMPLE_SAVE)'; using it, as "
						+ "--any-save asks"
				),
				model.log.text
			)
			check(try String(contentsOf: earlier, encoding: .utf8) != "earlier results\n")
			// A replacement: Cancel leaves the fixture, with the team editor open; Run Anyway
			// replaces the team.
			replaceAway(setup)
			checkEqual(prompts.questions.count, 3)
			checkEqual(prompts.questions.last?.message, otherSaveQuestion)
			checkEqual(model.teamEditor?.error, nil)
			check(model.teamEditor != nil)
			checkEqual(model.source?.url, fixture)
			check(!ranAnySave(setup))
			prompts.answers = [true]
			model.confirmTeam()
			wait(model)
			checkEqual(prompts.questions.count, 4)
			check(ranAnySave(setup))
			checkEqual(setup.standIn.runs.last?.first, "replace-away")
			check(model.isTemporary)
			checkEqual(model.teamEditor?.error, nil)
			// A swap, likewise.
			let replaced = model.source?.url
			swapAway(setup)
			checkEqual(prompts.questions.count, 5)
			checkEqual(prompts.questions.last?.message, otherSaveQuestion)
			checkEqual(model.source?.url, replaced)
			check(!ranAnySave(setup))
			prompts.answers = [true]
			swapAway(setup)
			checkEqual(prompts.questions.count, 6)
			check(ranAnySave(setup))
			checkEqual(setup.standIn.runs.last?.first, "inject-player")
			check(model.source?.url != replaced)
			check(prompts.errors.isEmpty, "\(prompts.errors)")
			// Each job is refused before Run Anyway runs it again.
			checkEqual(
				setup.standIn.runs.map { $0.first ?? "" },
				[
					"run", "run", "run", "list-teams", "replace-away", "replace-away", "replace-away",
					"list-players", "inject-player", "inject-player", "inject-player",
				]
			)
			// The fixture's own save goes ahead without asking.
			setup.standIn.set(["LOADED": FIXTURE_SAMPLE_SAVE])
			simulate(setup, seeds: "1")
			checkEqual(model.simulation.outcome, .complete)
			checkEqual(prompts.questions.count, 6)
			check(!ranAnySave(setup))
			// Another refusal of the same code, the game at its start screen, is an error as before.
			setup.standIn.set([
				"STATUS": "3", "STDOUT": "",
				"STDERR":
					"FM24 (pid 7) is at its start screen with no save loaded: load LIV_MAIN_FM24 in FM24, or "
					+ "quit FM24 and run this again.\n",
			])
			simulate(setup, seeds: "1")
			checkEqual(model.simulation.outcome, .failed)
			checkEqual(prompts.questions.count, 6)
			checkEqual(prompts.errors.count, 1)
		}
	}

	static func startsGame() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		open(setup, setup.scratch.file("next.json", plainSampleJSON(saved: true)))
		// FM24 is closed: fm24ctl starts it with the fixture's save, at a later date, and plays.
		setup.standIn.set([
			"CLOSED": "1", "LOAD_MS": "400", "ROW_MS": "100", "DATE": "2023 day 181 09:00",
		])
		model.setSeeds("1-3")
		model.simulate()
		check(
			waitUntil { model.activity == ViewerModel.startingGameActivity },
			"the activity did not say FM24 was starting"
		)
		// fm24ctl's progress shows under it as it comes, for as long as the game takes.
		check(
			waitUntil { model.activityDetail == "FM24 state: loading" },
			"the activity did not show fm24ctl's progress"
		)
		checkEqual(model.activity, ViewerModel.startingGameActivity)
		check(!model.simulation.started)
		check(waitUntil { model.simulation.started || !model.simulation.running })
		checkEqual(model.activity, nil)
		checkEqual(model.activityDetail, nil)
		check(waitUntil { !model.simulation.running })
		checkEqual(model.simulation.outcome, .complete)
		checkEqual(model.simulation.completed, 3)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		let starting =
			"fm24ctl: \(HarnessResult.startingGamePrefix)\(FIXTURE_SAMPLE_SAVE) (--no-launch "
			+ "turns this off)"
		for line in [
			starting,
			"fm24ctl: ready: \(FIXTURE_SAMPLE_SAVE) loaded (pid ",
			"fm24ctl: warning: the save is at 2023 day 181 09:00, the fixture was exported at "
				+ "\(FIXTURE_SAMPLE_GAME_DATE); the results reflect the save",
		] {
			check(model.log.text.contains(line), "the log lacks \(line)")
		}
		// A replacement shows it too, until it ends.
		setup.standIn.set([
			"CLOSED": "1", "LOAD_MS": "300",
			"FIXTURE": setup.scratch.file("luton.json", plainSampleJSON(saved: true)).path,
		])
		model.editTeam(side: 1)
		check(waitUntil { model.teams.state != .loading }, "the team list did not load")
		model.teamEditor?.type("677")
		model.confirmTeam()
		check(
			waitUntil { model.activity == ViewerModel.startingGameActivity },
			"the activity did not say FM24 was starting"
		)
		wait(model)
		checkEqual(model.activity, nil)
		check(model.isTemporary)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		// A fixture without a save is played without starting anything.
		let started = model.log.text.components(separatedBy: starting).count
		open(setup, setup.scratch.file("plain.json", plainSampleJSON()))
		simulate(setup, seeds: "1")
		checkEqual(model.simulation.outcome, .complete)
		checkEqual(model.log.text.components(separatedBy: starting).count, started)
	}

	static func showsBusy() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		// The startup export: on the start panel, with Retry.
		setup.standIn.set(["BUSY": "loading"])
		model.exportNextFixture()
		wait(model)
		check(model.fixture == nil)
		checkEqual(model.status, busyRefusal)
		checkEqual(model.retry, .export)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		// Over a fixture: decoding a binary one, a swap in a fixture that names no save, and
		// simulating a binary fixture, which has none: with no save to check, fm24ctl submits each
		// job, which the harness refuses, in an error box. (For a fixture that names its save,
		// fm24ctl waits instead: waitsWhileBusy.)
		let json = plainSampleJSON(saved: true)
		let fixture = setup.scratch.file("next.json", plainSampleJSON())
		setup.standIn.set([:])
		open(setup, fixture)
		let bytes = try FixtureHandle.write(json: json, playerEdits: nil, venue: nil, binary: true)
		let binary = setup.scratch.file("next.bin", bytes)
		setup.standIn.set(["BUSY": "loading"])
		model.open(binary)
		wait(model)
		checkEqual(setup.prompts.lastError, busyRefusal)
		checkEqual(model.source?.url, fixture)
		swapAway(setup)
		checkEqual(setup.prompts.lastError, busyRefusal)
		check(waitUntil { model.players.state != .loading }, "the player list did not finish")
		checkEqual(model.players.state, .unloaded)
		setup.standIn.set(["FIXTURE": setup.scratch.file("decoded.json", plainSampleJSON()).path])
		open(setup, binary)
		checkEqual(model.source?.kind, .binary)
		setup.standIn.set(["BUSY": "processing kind 3"])
		simulate(setup, seeds: "1-2")
		checkEqual(
			setup.prompts.lastError,
			"error: FM24 is busy (processing kind 3); wait until it is idle"
		)
		checkEqual(model.simulation.outcome, .failed)
		checkEqual(setup.prompts.errors.count, 3)
	}

	static func waitsWhileBusy() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		open(setup, setup.scratch.file("next.json", plainSampleJSON(saved: true)))
		// FM24 is loading: fm24ctl waits for it, showing its progress as it comes, then plays.
		setup.standIn.set(["BUSY": "loading", "BUSY_MS": "800", "ROW_MS": "50"])
		model.setSeeds("1-3")
		model.simulate()
		check(
			waitUntil { model.activity == ViewerModel.idleWaitActivity },
			"the activity did not say fm24ctl waits for FM24"
		)
		checkEqual(model.activityDetail, "FM24 state: loading")
		check(
			waitUntil { model.activityDetail == "still waiting (60 s)..." },
			"the activity did not show fm24ctl's progress"
		)
		checkEqual(model.activity, ViewerModel.idleWaitActivity)
		check(!model.simulation.started)
		check(waitUntil { !model.simulation.running }, "the simulation did not finish")
		checkEqual(model.simulation.outcome, .complete)
		checkEqual(model.simulation.completed, 3)
		checkEqual(model.activity, nil)
		checkEqual(model.activityDetail, nil)
		check(
			model.log.text.contains("fm24ctl: ready: \(FIXTURE_SAMPLE_SAVE) is already loaded (pid "),
			model.log.text
		)
		// A swap shows it too, then goes on as a swap.
		setup.standIn.set([
			"BUSY": "processing kind 3", "BUSY_MS": "600",
			"FIXTURE": setup.scratch.file("result.json", plainSampleJSON(saved: true)).path,
		])
		let shown = model.source?.url
		model.select(side: 1, blocks: [2])
		model.requestSwap(side: 1)
		check(waitUntil { model.players.state != .loading }, "the player list did not load")
		model.swapPrompt?.type("40347")
		model.confirmSwap()
		check(
			waitUntil { model.activity == ViewerModel.idleWaitActivity },
			"the activity did not say fm24ctl waits for FM24"
		)
		checkEqual(model.activityDetail, "FM24 state: busy")
		wait(model)
		check(model.source?.url != shown, "the swap's result is not on show")
		checkEqual(model.activity, nil)
		checkEqual(model.activityDetail, nil)
		checkEqual(
			model.log.text.components(separatedBy: "fm24ctl: FM24 state: ready").count - 1,
			2
		)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		// FM24 stays busy past fm24ctl's wait: its reason, in an error box.
		let timedOut =
			"FM24 was still busy after 900 s, without \(FIXTURE_SAMPLE_SAVE) ready. Check the game "
			+ "window."
		setup.standIn.set(["BUSY": "processing kind 3"])
		simulate(setup, seeds: "1")
		checkEqual(model.simulation.outcome, .failed)
		checkEqual(setup.prompts.lastError, timedOut)
		checkEqual(model.activity, nil)
		// A replacement shows it in its editor.
		setup.standIn.set(["BUSY": "loading"])
		model.editTeam(side: 1)
		model.teamEditor?.type("677")
		model.confirmTeam()
		wait(model)
		checkEqual(model.teamEditor?.error, timedOut.replacingOccurrences(of: "busy", with: "loading"))
		checkEqual(setup.prompts.errors.count, 1)
	}

	static func dropsSaveFromBinary() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		// An export that stores venue values, so that a simulation of its binary copy gives the game
		// the JSON kept with it.
		open(setup, setup.scratch.file("venue.json", savedSampleJSON(FIXTURE_SAMPLE_VENUE)))
		checkEqual(model.fixture?.saveLine, "Save: \(FIXTURE_SAMPLE_SAVE), \(FIXTURE_SAMPLE_GAME_DATE)")
		let withSave = setup.scratch.file("with-save.json")
		model.save(to: withSave, binary: false)
		wait(model)
		checkEqual(SaveOrigin(try? Data(contentsOf: withSave)), .sample)
		let binary = setup.scratch.file("venue.bin")
		model.save(to: binary, binary: true)
		wait(model)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(model.source?.url, binary)
		// As when the .bin is opened again: no save in the summary or the JSON kept with it, which
		// is otherwise the export's.
		checkEqual(model.fixture?.saveLine, nil)
		checkEqual(SaveOrigin(model.source?.json), SaveOrigin(save: nil, date: nil))
		checkEqual(model.source?.json, try FixtureHandle.withoutSave(Data(contentsOf: withSave)))
		// So fm24ctl checks no save: another loaded one plays it without asking.
		let played = setup.scratch.url.appendingPathComponent("played.json")
		setup.standIn.set(["LOADED": otherSave, "INPUT": played.path])
		simulate(setup, seeds: "keep")
		checkEqual(model.simulation.outcome, .complete)
		checkEqual(setup.prompts.questions.count, 0)
		checkEqual(setup.standIn.runs.last?[1].hasSuffix(".json"), true)
		checkEqual(SaveOrigin(try? Data(contentsOf: played)), SaveOrigin(save: nil, date: nil))
		// Saving it as JSON again writes no save either.
		let json = setup.scratch.file("again.json")
		model.save(to: json, binary: false)
		wait(model)
		checkEqual(SaveOrigin(try? Data(contentsOf: json)), SaveOrigin(save: nil, date: nil))
		checkEqual(model.fixture?.saveLine, nil)
		// A JSON save keeps it, as before.
		open(setup, setup.scratch.file("next.json", plainSampleJSON(saved: true)))
		let kept = setup.scratch.file("kept.json")
		model.save(to: kept, binary: false)
		wait(model)
		checkEqual(SaveOrigin(try? Data(contentsOf: kept)), .sample)
		check(model.fixture?.saveLine != nil)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	static func skipsNotices() {
		// FM24 crashes during a run, after fm24ctl's warning of another date and its notice that the
		// harness cannot name the loaded save: the box gives fm24ctl's reason, offering Launch FM24…
		// as no harness serves any more.
		let lost = "cannot read a complete result; see harness.log"
		for serving in [false, true] {
			guard let setup = HarnessSetup(serving: serving) else {
				return
			}
			let model = setup.model
			open(setup, setup.scratch.file("next.json", plainSampleJSON(saved: true)))
			setup.standIn.set(["DATE": "2023 day 181 09:00", "UNNAMED": "1", "LOST_AT": "1"])
			simulate(setup, seeds: "1-3")
			checkEqual(model.simulation.outcome, .failed)
			for notice in [
				"fm24ctl: the harness cannot read which save FM24 has loaded; taking it to be "
					+ FIXTURE_SAMPLE_SAVE,
				"fm24ctl: warning: the save is at 2023 day 181 09:00",
			] {
				check(model.log.text.contains(notice), "the log lacks \(notice)")
			}
			if serving {
				checkEqual(setup.prompts.lastError, lost)
				checkEqual(setup.prompts.launchOffers.count, 0)
			} else {
				checkEqual(
					setup.prompts.launchOffers.last?.message,
					"\(HarnessResult.notServing)\n\(lost)\n\n\(ViewerModel.setOptionFirst)"
				)
				checkEqual(setup.prompts.errors.count, 0)
			}
		}
	}
}
