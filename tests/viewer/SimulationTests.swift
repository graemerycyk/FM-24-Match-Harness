// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import FixtureSamples
import Foundation

/// The Simulation tab: its entries and checks, reading a run's progress from its results, and
/// simulations through fm24ctl's stand-in, which writes results slowly, with the Windows viewer's
/// rules and messages, the game kept to the temporary directory and the results moved into place.
@MainActor
enum SimulationTests {
	static func register() {
		Tests.test("simulation entries are checked as on Windows", checksEntries)
		Tests.test("a run's progress is read from its results as they are written", readsProgress)
		Tests.test("the progress bar and Dock badge follow the run", showsProgress)
		Tests.test("viewer simulates a fixture with live progress", simulates)
		Tests.test("viewer plays edits from a copy and never writes the fixture", playsEdits)
		Tests.test("viewer saves the detailed reports beside the results", savesReports)
		Tests.test("viewer plays a binary fixture's own bytes", playsBinary)
		Tests.test("a fixture not yet saved has its results in the export folder", exportsResults)
		Tests.test("a failed simulation says why and keeps earlier results", reportsFailures)
		Tests.test("viewer refuses to simulate invalid entries", refusesEntries)
		Tests.test("a seed pattern fm24ctl refuses shows its reason", refusesSeeds)
		Tests.test("nothing but an export runs during a simulation", locksDuringSimulation)
		Tests.test("results that cannot be saved stay in the temporary folder", keepsUnsavedResults)
		Tests.test("viewer shows the results in Finder", revealsResults)
		Tests.test("viewer shows the harness waiting before a simulation", showsWaiting)
	}

	private static let threads = SimulationForm.threadLimit
	private static let zero = "n = 0: home 0 (0.0%) / draw 0 (0.0%) / away 0 (0.0%)"
	private static let header = "seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n"
	private static let exportStem = "aston-villa-v-bristol-city-1700000000"

	/// The stand-in's results for `rows` matches.
	private static func results(_ rows: Int) -> String {
		var text = header
		for row in 0..<rows {
			let rng = String(format: "%016llx", UInt64(row) &* 2_654_435_761)
			text += "\(row + 1),\(row % 3),1,\(23000 + row),1,\(rng),\(4000 + row).0\n"
		}
		return text
	}

	private static func text(_ url: URL) -> String? {
		try? String(contentsOf: url, encoding: .utf8)
	}

	private static func open(_ setup: HarnessSetup, _ url: URL) {
		setup.model.open(url)
		check(waitUntil { !setup.model.isBusy }, "the open did not finish")
	}

	/// Starts a simulation of `seeds` and waits for it to end.
	private static func simulate(_ setup: HarnessSetup, seeds: String) {
		setup.model.setSeeds(seeds)
		setup.model.simulate()
		check(setup.model.simulation.running, "the simulation did not start")
		check(waitUntil { !setup.model.simulation.running }, "the simulation did not finish")
	}

	/// The files left in the viewer's own folder.
	private static func workFiles(_ setup: HarnessSetup) -> [String] {
		setup.scratch.names("T/" + setup.model.work.folder.lastPathComponent)
	}

	static func checksEntries() {
		var form = SimulationForm()
		checkEqual(form.threads, String(threads))
		check(threads >= 1 && threads <= 16)
		checkEqual(form.seeds, "")
		check(!form.detailed && form.keepBuild)
		checkEqual(SimulationForm.threadsLabel, "Number of threads (1-\(threads)):")
		checkEqual(
			form.options(),
			.success(
				SimulationOptions(
					threads: threads,
					seeds: "keep",
					detailed: false,
					keepBuild: true,
					total: 1
				)
			)
		)
		let threadsProblem = SimulationForm.Problem(
			field: .threads,
			message: "Number of threads must be between 1 and \(threads)."
		)
		for text in ["", "0", String(threads + 1), "1x", " 1", "99999999999999999999"] {
			form.setThreads(text)
			checkEqual(form.options(), .failure(threadsProblem))
		}
		checkEqual(form.threads.count, 15)
		form.setThreads("001")
		checkEqual((try? form.options().get())?.threads, 1)
		// The stepper keeps within 1 and the limit, from the number in the field or the limit.
		form.stepThreads(by: -1)
		checkEqual(form.threads, "1")
		form.setThreads("x")
		form.stepThreads(by: 1)
		checkEqual(form.threads, String(threads))
		form.stepThreads(by: -1)
		checkEqual(form.threads, String(max(threads - 1, 1)))
		form.setThreads("0")
		form.stepThreads(by: 1)
		checkEqual(form.threads, String(min(2, threads)))

		let seedsProblem = SimulationForm.Problem(
			field: .seeds,
			message: "Enter a seed pattern such as 1, 42, 1-1000, 1,3,5 or 1,4,7-70; or leave blank "
				+ "to use the fixture's seed."
		)
		form.setThreads("1")
		for text in ["1;2", "1 - 3", "Keep", "1\u{0}2"] {
			form.setSeeds(text)
			checkEqual(form.options(), .failure(seedsProblem))
		}
		form.setSeeds("1-1000,2001-3000")
		form.detailed = true
		form.keepBuild = false
		checkEqual(
			form.options(),
			.success(
				SimulationOptions(
					threads: 1,
					seeds: "1-1000,2001-3000",
					detailed: true,
					keepBuild: false,
					total: 2000
				)
			)
		)
		form.setSeeds("keep")
		checkEqual((try? form.options().get())?.total, 1)
		// Accepted by the form, refused by fm24ctl, as the harness would refuse it: the total is not
		// known.
		form.setSeeds("5-1")
		checkEqual((try? form.options().get())?.seeds, "5-1")
		checkEqual((try? form.options().get())?.total, nil)
		form.setSeeds(String(repeating: "1", count: 300))
		checkEqual(form.seeds.count, 255)
	}

	static func readsProgress() throws {
		let scratch = Scratch()
		let csv = scratch.url.appendingPathComponent("run-2.csv")
		let progress = RunProgress(url: csv, total: 4)
		// Nothing yet: the game has not opened the file.
		check(!progress.read())
		check(!progress.started)
		checkEqual(progress.text, zero)
		let file = try FileHandle(forWritingTo: scratch.file("run-2.csv", Data()))
		defer { try? file.close() }
		func write(_ text: String) {
			try? file.write(contentsOf: Data(text.utf8))
		}
		check(!progress.read())
		// Whole lines only: a row arrives in pieces.
		write("seed,home_goals,away_")
		check(progress.read())
		check(!progress.started)
		write("goals,clock_ticks,finished,rng_final,ms\n1,2,1,23000,1,00ff,4000.0\n2,0,")
		progress.read()
		check(progress.started)
		checkEqual(progress.completed, 1)
		checkEqual(progress.text, "n = 1: home 1 (100.0%) / draw 0 (0.0%) / away 0 (0.0%)")
		write("0,23001,1,00fe,4001.0\r\n3,0,2,23002,1,00fd,4002.0\n")
		progress.read()
		checkEqual(progress.completed, 3)
		check(!progress.unreadable)
		checkEqual(progress.text, "n = 3: home 1 (33.3%) / draw 1 (33.3%) / away 1 (33.3%)")
		// A line that is not a result.
		write("not,a,result\n4,1,1,23003,1,00fc,4003.0\n")
		progress.read()
		check(progress.unreadable)
		checkEqual(progress.completed, 4)

		// A file that is not results, and endless text without a line break.
		let other = RunProgress(url: scratch.file("other.csv", Data("name,value\n".utf8)), total: 1)
		other.read()
		check(other.unreadable && !other.started)
		let endless = RunProgress(
			url: scratch.file("endless.csv", Data((header + String(repeating: "9", count: 70_000)).utf8)),
			total: 1
		)
		endless.read()
		check(endless.started && endless.unreadable)
	}

	static func showsProgress() {
		var state = SimulationState()
		checkEqual(state.badge, nil)
		checkEqual(state.fraction, nil)
		state.running = true
		state.total = 8
		// Indeterminate until results arrive.
		checkEqual(state.fraction, nil)
		checkEqual(state.badge, "…")
		state.started = true
		checkEqual(state.fraction, 0)
		checkEqual(state.badge, "0%")
		state.completed = 3
		checkEqual(state.fraction, 0.375)
		checkEqual(state.badge, "37%")
		state.completed = 9
		checkEqual(state.badge, "100%")
		state.completed = 3
		state.unreadable = true
		checkEqual(state.fraction, nil)
		checkEqual(state.badge, "…")
		state.unreadable = false
		state.total = nil
		checkEqual(state.badge, "…")
		state.total = 0
		checkEqual(state.fraction, nil)
		state.running = false
		checkEqual(state.badge, nil)
	}

	static func simulates() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let fixture = setup.scratch.file("Next Fixture.json", plainSampleJSON())
		open(setup, fixture)
		checkEqual(model.simulationStatus, ViewerModel.resultsBeside)
		check(model.canSimulate)
		let copy = setup.scratch.url.appendingPathComponent("played.json")
		setup.standIn.set(["ROW_MS": "120", "INPUT": copy.path])
		// Three workers where the machine allows them: CI's runners may allow only two.
		let workers = String(min(3, SimulationForm.threadLimit))
		model.setThreads(workers)
		model.setSeeds("1-6")
		model.simulate()
		// Running: indeterminate until the first results, the Mac kept awake.
		checkEqual(model.busy, .simulation)
		check(!model.canSimulate)
		checkEqual(model.simulation.summary, zero)
		checkEqual(model.simulation.total, 6)
		checkEqual(model.simulation.fraction, nil)
		checkEqual(model.simulation.badge, "…")
		checkEqual(
			model.simulationStatus,
			"Simulating fixture... Please wait before closing the window."
		)
		checkEqual(setup.activities.begun, ["Simulating a fixture with FM24"])
		checkEqual(setup.activities.ended, 0)
		checkEqual(
			model.closeCheck(),
			.blocked("A simulation is running. Please wait for it to finish before closing.")
		)
		// The summary, bar and badge follow the results as they are written.
		check(
			waitUntil { model.simulation.completed >= 2 || !model.simulation.running },
			"no progress"
		)
		let completed = model.simulation.completed
		check(model.simulation.running && completed < 6, "the progress was not seen while running")
		checkEqual(model.simulation.fraction, Double(completed) / 6)
		checkEqual(model.simulation.badge, "\(completed * 100 / 6)%")
		check(model.simulation.summary.hasPrefix("n = \(completed): "), model.simulation.summary)
		var seen: Set<Int> = []
		check(
			waitUntil {
				seen.insert(model.simulation.completed)
				return !model.simulation.running
			},
			"the simulation did not finish"
		)
		check(seen.count >= 3, "progress seen at \(seen.sorted())")
		// Done: the results beside the fixture, the work files gone.
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(model.busy, .none)
		checkEqual(model.simulation.outcome, .complete)
		checkEqual(
			model.simulation.summary,
			"n = 6: home 2 (33.3%) / draw 2 (33.3%) / away 2 (33.3%)"
		)
		checkEqual(model.simulation.badge, nil)
		checkEqual(
			model.simulationStatus,
			"Simulation complete. Results were saved beside the fixture; see the log for paths."
		)
		let saved = setup.scratch.url.appendingPathComponent("Next Fixture.csv")
		checkEqual(model.simulation.results, [saved])
		checkEqual(text(saved), results(6))
		checkEqual(setup.activities.ended, 1)
		let folder = model.work.folder.path
		checkEqual(
			setup.standIn.runs,
			[
				[
					"run", folder + "/in-1.json", "1-6", folder + "/run-2.csv", "--threads", workers,
					"--keep-build",
				]
			]
		)
		setup.checkTemporaryOnly()
		checkEqual(workFiles(setup), [])
		// With nothing to change, the game plays a copy of the fixture as it is.
		checkEqual(try? Data(contentsOf: copy), plainSampleJSON())
		checkEqual(try? Data(contentsOf: fixture), plainSampleJSON())
		check(model.log.text.contains("Results: \(saved.path)"), model.log.text)
		check(model.log.text.contains("fm24ctl: results: \(folder)/run-2.csv"), model.log.text)
		// Opening another fixture starts the tab afresh; its entries stay.
		open(setup, setup.scratch.file("other.json", plainSampleJSON()))
		checkEqual(model.simulation, SimulationState())
		checkEqual(model.simulationStatus, ViewerModel.resultsBeside)
		checkEqual(model.simulationForm.seeds, "1-6")
		checkEqual(model.simulationForm.threads, workers)
	}

	static func playsEdits() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let fixture = setup.scratch.file("next.json", plainSampleJSON())
		open(setup, fixture)
		guard let handle = model.fixture else {
			check(false, "no fixture")
			return
		}
		model.select(side: 0, blocks: [0])
		model.maxPlayers(.happiness, side: 0)
		let edits = handle.playerEdits
		let copy = setup.scratch.url.appendingPathComponent("played.json")
		setup.standIn.set(["INPUT": copy.path])
		simulate(setup, seeds: "")
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(
			try Data(contentsOf: copy),
			try FixtureHandle.write(
				json: plainSampleJSON(),
				playerEdits: edits,
				venue: nil,
				binary: false
			)
		)
		checkEqual(try Data(contentsOf: fixture), plainSampleJSON())
		// The edits stay to be saved.
		check(model.hasUnsavedEdits)
		checkEqual(model.heading?.marker, " (unsaved player edits)")
		checkEqual(text(setup.scratch.url.appendingPathComponent("next.csv")), results(1))

		// A fixture that stores venue values plays them written, with derived values recomputed.
		let venueJSON = sampleJSON(FIXTURE_SAMPLE_VENUE)
		let venueFixture = setup.scratch.file("venue.json", venueJSON)
		setup.prompts.answers = [true]
		model.requestOpen(venueFixture)
		check(waitUntil { !model.isBusy })
		guard let venue = model.fixture?.venue else {
			check(false, "no venue fixture")
			return
		}
		simulate(setup, seeds: "keep")
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(
			try Data(contentsOf: copy),
			try FixtureHandle.write(json: venueJSON, playerEdits: nil, venue: venue, binary: false)
		)
		checkEqual(try Data(contentsOf: venueFixture), venueJSON)
		let folder = model.work.folder.path
		checkEqual(setup.standIn.runs.last?[1], folder + "/in-3.json")
		checkEqual(setup.standIn.runs.last?[2], "keep")
		checkEqual(workFiles(setup), [])
	}

	static func savesReports() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		open(setup, setup.scratch.file("cup.tie.json", plainSampleJSON()))
		model.setDetailed(true)
		model.setKeepBuild(false)
		model.setThreads("1")
		simulate(setup, seeds: "1,3-4")
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		let folder = model.work.folder.path
		checkEqual(
			setup.standIn.runs,
			[
				[
					"run", folder + "/in-1.json", "1,3-4", folder + "/run-2.csv", "--threads", "1",
					"--detailed",
				]
			]
		)
		let saved = ["cup.tie.csv", "cup.tie-players.csv", "cup.tie-teams.csv"].map {
			setup.scratch.url.appendingPathComponent($0)
		}
		checkEqual(model.simulation.results, saved)
		checkEqual(text(saved[0]), results(3))
		checkEqual(text(saved[1]), "stand-in-players.csv\n")
		checkEqual(text(saved[2]), "stand-in-teams.csv\n")
		checkEqual(workFiles(setup), [])
		// Results already there are replaced, as the game replaces them on Windows.
		simulate(setup, seeds: "7")
		checkEqual(text(saved[0]), results(1))
		checkEqual(model.simulation.outcome, .complete)
	}

	static func playsBinary() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let json = plainSampleJSON()
		let bytes = try FixtureHandle.write(json: json, playerEdits: nil, venue: nil, binary: true)
		let binary = setup.scratch.file("Next.bin", bytes)
		setup.standIn.set(["FIXTURE": setup.scratch.file("decoded.json", json).path])
		open(setup, binary)
		checkEqual(model.source?.kind, .binary)
		let copy = setup.scratch.url.appendingPathComponent("played.bin")
		setup.standIn.set(["INPUT": copy.path])
		simulate(setup, seeds: "1-2")
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		let folder = model.work.folder.path
		checkEqual(setup.standIn.runs.last?[1], folder + "/in-3.bin")
		checkEqual(try Data(contentsOf: copy), bytes)
		checkEqual(text(setup.scratch.url.appendingPathComponent("Next.csv")), results(2))
		setup.checkTemporaryOnly()
	}

	static func exportsResults() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		setup.standIn.set(["FIXTURE": setup.scratch.file("next.json", plainSampleJSON()).path])
		model.exportNextFixture()
		check(waitUntil { !model.isBusy })
		check(model.isTemporary)
		checkEqual(
			model.simulationStatus,
			"Results are saved in the export folder, using the fixture's export name with .csv."
		)
		setup.standIn.set([:])
		simulate(setup, seeds: "1-3")
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		let saved = setup.exports.appendingPathComponent(exportStem + ".csv")
		checkEqual(model.simulation.results, [saved])
		checkEqual(text(saved), results(3))
		checkEqual(
			model.simulationStatus,
			"Simulation complete. Results were saved in the export folder; see the log for paths."
		)
		check(model.isTemporary, "the fixture itself is not exported")
		checkEqual(setup.scratch.names("Exports"), [exportStem + ".csv"])
		setup.checkTemporaryOnly()

		// Exported while it runs, which is allowed: the results go beside the export.
		setup.standIn.set(["ROW_MS": "150"])
		model.setSeeds("1-4")
		model.simulate()
		check(model.canExport)
		model.now = { Date(timeIntervalSince1970: 1_700_000_005) }
		model.export()
		checkEqual(model.busy, .file)
		check(waitUntil { !model.fileOperation }, "the export did not finish")
		check(model.simulation.running, "the simulation ended before the export")
		checkEqual(model.busy, .simulation)
		check(!model.isTemporary)
		check(waitUntil { !model.simulation.running })
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		let exported = "aston-villa-v-bristol-city-1700000005"
		let beside = setup.exports.appendingPathComponent(exported + ".csv")
		checkEqual(model.simulation.results, [beside])
		checkEqual(text(beside), results(4))
		checkEqual(model.simulationStatus, ViewerModel.completeBeside)
		checkEqual(
			setup.scratch.names("Exports"),
			[exportStem + ".csv", exported + ".csv", exported + ".json"]
		)
	}

	static func reportsFailures() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		open(setup, setup.scratch.file("next.json", plainSampleJSON()))
		let saved = setup.scratch.url.appendingPathComponent("next.csv")
		try Data("earlier results\n".utf8).write(to: saved)
		// The harness refuses: its error line, and the earlier results stay.
		let refusal =
			"error: cannot rebuild per seed: the fixture is not the next one (--keep-build plays the "
			+ "input with only its seed changed)"
		setup.standIn.set(["STATUS": "1", "STDOUT": refusal + "\n"])
		simulate(setup, seeds: "1-3")
		checkEqual(setup.prompts.lastError, refusal)
		checkEqual(setup.prompts.errors.last?.title, "FM24 Match Harness")
		checkEqual(model.simulation.outcome, .failed)
		checkEqual(
			model.simulationStatus,
			"Simulation failed. Check the log and harness.log for details."
		)
		checkEqual(model.simulation.summary, zero)
		checkEqual(model.simulation.results, [])
		checkEqual(text(saved), "earlier results\n")
		checkEqual(setup.activities.ended, 1)
		checkEqual(workFiles(setup), [])
		// Failed with only the header written: the earlier results stay, the header is removed.
		setup.standIn.set(["FAIL_AT": "0"])
		simulate(setup, seeds: "1-3")
		checkEqual(setup.prompts.lastError, "error: the stand-in stopped after 0 matches")
		checkEqual(model.simulation.results, [])
		checkEqual(text(saved), "earlier results\n")
		checkEqual(workFiles(setup), [])
		// Nothing to say: the Windows message.
		setup.standIn.set(["STATUS": "1", "STDOUT": ""])
		simulate(setup, seeds: "1")
		checkEqual(setup.prompts.lastError, ViewerModel.simulationFailed)
		// Stopped part way: its results so far, as the game leaves them on Windows.
		setup.standIn.set(["FAIL_AT": "2"])
		simulate(setup, seeds: "1-5")
		checkEqual(setup.prompts.lastError, "error: the stand-in stopped after 2 matches")
		checkEqual(model.simulation.outcome, .failed)
		checkEqual(model.simulation.results, [saved])
		checkEqual(text(saved), results(2))
		checkEqual(
			model.simulation.summary,
			"n = 2: home 0 (0.0%) / draw 1 (50.0%) / away 1 (50.0%)"
		)
		checkEqual(workFiles(setup), [])
		// A line that is not a result: the run completes, the bar no longer says how far.
		let errors = setup.prompts.errors.count
		setup.standIn.set(["BAD_ROW": "1", "ROW_MS": "100"])
		model.setSeeds("1-4")
		model.simulate()
		check(waitUntil { model.simulation.unreadable || !model.simulation.running })
		check(model.simulation.running, "the line was not seen while running")
		checkEqual(model.simulation.fraction, nil)
		checkEqual(model.simulation.badge, "…")
		check(waitUntil { !model.simulation.running })
		checkEqual(model.simulation.outcome, .complete)
		checkEqual(model.simulation.completed, 3)
		check(model.log.text.contains("has a line that is not a result"), model.log.text)
		checkEqual(setup.prompts.errors.count, errors)
		checkEqual(setup.activities.begun.count, 5)
		checkEqual(setup.activities.ended, 5)
	}

	/// A pattern the form takes but fm24ctl refuses, as the harness would (exit status 2): fm24ctl's
	/// reason, not its usage line, without Launch FM24… or Install Harness…, though no harness
	/// serves, as no job reached one; earlier results stay.
	static func refusesSeeds() throws {
		guard let setup = HarnessSetup(serving: false) else {
			return
		}
		let model = setup.model
		open(setup, setup.scratch.file("next.json", plainSampleJSON()))
		let saved = setup.scratch.url.appendingPathComponent("next.csv")
		try Data("earlier results\n".utf8).write(to: saved)
		// fm24ctl's reasons, as the runner's tests have them.
		let reasons = [
			("5-1", "the seed range 5-1 runs backwards: write 1-5"),
			("1,5-1", "the seed range 5-1 runs backwards: write 1-5"),
			("1,,5", "a seed is missing next to a comma in 1,,5"),
			("1,2-", "2- is not a seed or a range of seeds (first-last)"),
			("4294967296", "seeds must be numbers from 0 to 4294967295, not 4294967296"),
			("0-10000001", "the seed range 0-10000001 has more than 10000001 seeds: split it"),
		]
		for (seeds, reason) in reasons {
			simulate(setup, seeds: seeds)
			checkEqual(setup.prompts.lastError, reason)
		}
		checkEqual(setup.prompts.launchOffers.count, 0)
		checkEqual(setup.standIn.runs.map { $0.first ?? "" }, reasons.map { _ in "run" })
		checkEqual(model.simulation.outcome, .failed)
		checkEqual(text(saved), "earlier results\n")
		checkEqual(workFiles(setup), [])
	}

	static func refusesEntries() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		open(setup, setup.scratch.file("next.json", plainSampleJSON()))
		model.setThreads("0")
		model.simulate()
		checkEqual(setup.prompts.lastError, "Number of threads must be between 1 and \(threads).")
		checkEqual(model.tab, .simulation)
		checkEqual(model.simulationFocus?.field, .threads)
		if let request = model.simulationFocus {
			model.focusedSimulation(
				ViewerModel.SimulationFocus(field: request.field, serial: request.serial - 1)
			)
			checkEqual(model.simulationFocus, request)
			model.focusedSimulation(request)
		}
		checkEqual(model.simulationFocus, nil)
		model.setThreads("1")
		model.setSeeds("1 2")
		model.tab = .squads
		model.simulate()
		checkEqual(setup.prompts.lastError, SimulationForm.seedsProblem)
		checkEqual(model.tab, .simulation)
		checkEqual(model.simulationFocus?.field, .seeds)
		// The venue is checked after the entries, as on Windows.
		model.setSeeds("")
		open(setup, setup.scratch.file("labelled.json", sampleJSON(FIXTURE_SAMPLE_LABELLED)))
		model.simulate()
		checkEqual(setup.prompts.lastError, "Invalid venue: enter a finite positive pitch dimension.")
		checkEqual(model.tab, .venue)
		check(!model.simulation.running)
		checkEqual(setup.standIn.runs, [])
		checkEqual(setup.activities.begun, [])
		// Without fm24ctl.
		let prompts = FakePrompts()
		let alone = ViewerModel(log: ViewerLog(echo: false), prompts: prompts)
		alone.open(setup.scratch.file("alone.json", plainSampleJSON()))
		check(waitUntil { !alone.isBusy })
		alone.simulate()
		checkEqual(prompts.lastError, ViewerModel.noHarness)
		check(!alone.simulation.running)
	}

	static func locksDuringSimulation() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let prompts = setup.prompts
		open(setup, setup.scratch.file("venue.json", sampleJSON(FIXTURE_SAMPLE_VENUE)))
		let other = setup.scratch.file("other.json", plainSampleJSON())
		setup.standIn.set(["ROW_MS": "200"])
		model.setSeeds("1-3")
		model.simulate()
		check(model.simulation.running)
		let busy = "Please wait for the current fixture operation to finish."
		model.requestOpen(other)
		checkEqual(prompts.lastError, busy)
		model.open(other)
		model.requestSave()
		model.save(to: setup.scratch.file("saved.json"), binary: false)
		model.simulate()
		checkEqual(prompts.errors.map(\.message), Array(repeating: busy, count: 5))
		check(!model.canSimulate && !model.canExport && !model.simulationEditable)
		model.setThreads("1")
		model.setSeeds("9")
		model.setDetailed(true)
		model.setKeepBuild(false)
		model.stepThreads(by: -1)
		checkEqual(model.simulationForm.seeds, "1-3")
		checkEqual(model.simulationForm.threads, String(threads))
		check(!model.simulationForm.detailed && model.simulationForm.keepBuild)
		check(!(0..<FixtureHandle.venueFieldCount).contains(where: model.venueEditable))
		model.select(side: 0, blocks: [0])
		checkEqual(model.selection, [[], []])
		check(!model.canEditPlayers(side: 0))
		var closes: [Bool] = []
		model.confirmClose { closes.append($0) }
		checkEqual(closes, [false])
		checkEqual(
			prompts.lastError,
			"A simulation is running. Please wait for it to finish before closing."
		)
		check(waitUntil { !model.simulation.running })
		checkEqual(model.source?.name, "venue.json")
		checkEqual(setup.standIn.runs.count, 1)
		check(model.canSimulate && model.simulationEditable)
		check((0..<FixtureHandle.venueFieldCount).contains(where: model.venueEditable))
		checkEqual(model.closeCheck(), .close)
	}

	static func keepsUnsavedResults() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let locked = setup.scratch.url.appendingPathComponent("Locked")
		try FileManager.default.createDirectory(at: locked, withIntermediateDirectories: true)
		let fixture = locked.appendingPathComponent("next.json")
		try plainSampleJSON().write(to: fixture)
		open(setup, fixture)
		try FileManager.default.setAttributes([.posixPermissions: 0o555], ofItemAtPath: locked.path)
		defer {
			try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: locked.path)
		}
		simulate(setup, seeds: "1-2")
		let kept = model.work.folder.appendingPathComponent("run-2.csv")
		checkEqual(
			setup.prompts.lastError,
			"Cannot save the results as \(locked.path)/next.csv (Permission denied). It is in "
				+ "\(kept.path) until FM24 Viewer quits."
		)
		checkEqual(model.simulation.outcome, .failed)
		checkEqual(model.simulation.results, [kept])
		checkEqual(text(kept), results(2))
		checkEqual(workFiles(setup), ["run-2.csv"])
		model.work.removeFolder()
	}

	static func revealsResults() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		var revealed: [[URL]] = []
		model.reveal = { revealed.append($0) }
		open(setup, setup.scratch.file("next.json", plainSampleJSON()))
		model.setDetailed(true)
		simulate(setup, seeds: "1")
		model.revealResults()
		checkEqual(revealed, [model.simulation.results])
		checkEqual(revealed.first?.count, 3)
		for file in model.simulation.results {
			try? FileManager.default.removeItem(at: file)
		}
		model.revealResults()
		checkEqual(revealed.count, 1)
		checkEqual(setup.prompts.lastError, "The last simulation's results cannot be found.")
	}

	static func showsWaiting() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		open(setup, setup.scratch.file("next.json", plainSampleJSON()))
		setup.standIn.set([
			"STDERR": HarnessResult.waitingNotice + "\n", "DELAY_MS": "400", "ROW_MS": "150",
		])
		model.setSeeds("1-3")
		model.simulate()
		checkEqual(model.activity, nil)
		check(
			waitUntil { model.activity == ViewerModel.waitingActivity },
			"the activity did not say it was waiting"
		)
		// The game has started on the run once its results arrive.
		check(waitUntil { model.simulation.started || !model.simulation.running })
		checkEqual(model.activity, nil)
		check(waitUntil { !model.simulation.running })
		checkEqual(model.activity, nil)
		checkEqual(model.simulation.outcome, .complete)
	}
}
