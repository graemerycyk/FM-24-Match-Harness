// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import Foundation

/// The harness bridge: fm24ctl's runs (against its stand-in), how their failures read, the files
/// the game may touch, the export folder and following the game.
@MainActor
enum HarnessTests {
	static func register() {
		Tests.test("harness failures read in the viewer's order of preference", explainsFailures)
		Tests.test("fm24ctl is the bundle's or FM24CTL's", locates)
		Tests.test("fm24ctl runs with exact arguments and no input", runsExactly)
		Tests.test("fm24ctl's output is read in full however much there is", drainsPipes)
		Tests.test("fm24ctl's result comes back while a child holds its pipes", ignoresChildren)
		Tests.test("fm24ctl that cannot start says why", reportsStartFailure)
		Tests.test("cancelling a job ends its fm24ctl", endsCancelledJobs)
		Tests.test("work files stay in the user's temporary directory", keepsWorkFiles)
		Tests.test("old work folders of viewers that have gone are removed", sweepsWorkFolders)
		Tests.test("the export folder is a setting", keepsExportFolder)
		Tests.test("the game monitor follows the game", followsGame)
	}

	/// Runs `job` and waits for its result.
	private static func result(_ job: @escaping () async -> HarnessResult) -> HarnessResult? {
		var result: HarnessResult?
		Task { result = await job() }
		guard waitUntil({ result != nil }) else {
			check(false, "fm24ctl did not finish")
			return nil
		}
		return result
	}

	static func explainsFailures() {
		let version = FixtureHandle.protocolVersion
		var asked = 0
		let serving = { () -> Bool in
			asked += 1
			return true
		}
		let notServing = { () -> Bool in
			asked += 1
			return false
		}
		let fallback = "fallback"
		checkEqual(HarnessResult(status: 0).problem(fallback: fallback, serving: notServing), nil)
		checkEqual(asked, 0)
		// The harness's last error line first, without asking about the socket.
		var failed = HarnessResult(
			status: 1,
			output: ["ok: halfway", "error: first", "error: no upcoming fixture", "done"],
			errors: ["cannot connect"]
		)
		checkEqual(
			failed.problem(fallback: fallback, serving: notServing),
			"error: no upcoming fixture"
		)
		checkEqual(asked, 0)
		// No harness serving: what fm24ctl's first line of errors says, past its waiting notice.
		failed.output = ["ok"]
		failed.errors = [
			HarnessResult.waitingNotice,
			"",
			"Football Manager 2024 (pid 42) is running without the harness: on macOS it loads when the "
				+ "game starts.",
			"Quit the game and start it again with the harness.",
			"  /bin/sh \"launcher\" %command%",
		]
		checkEqual(
			failed.problem(fallback: fallback, serving: notServing),
			"FM24 isn't running with harness \(version).\nFootball Manager 2024 (pid 42) is running "
				+ "without the harness: on macOS it loads when the game starts."
		)
		checkEqual(asked, 1)
		// A harness serving: fm24ctl's last line of errors, other than its notice.
		failed.errors = [
			"cannot read input.json", "  timed out waiting for the other job  ",
			HarnessResult.waitingNotice,
		]
		checkEqual(
			failed.problem(fallback: fallback, serving: serving),
			"timed out waiting for the other job"
		)
		checkEqual(asked, 2)
		failed.errors = [HarnessResult.waitingNotice, " "]
		checkEqual(failed.problem(fallback: fallback, serving: serving), fallback)
		checkEqual(
			HarnessResult(status: 2).problem(fallback: fallback, serving: notServing),
			"FM24 isn't running with harness \(version)."
		)
		// fm24ctl did not start: why, whatever the socket.
		let unstarted = HarnessResult(
			status: -1,
			errors: ["cannot run fm24ctl (missing)"],
			started: false
		)
		checkEqual(
			unstarted.problem(fallback: fallback, serving: notServing),
			"cannot run fm24ctl (missing)"
		)
		checkEqual(asked, 4)
		// The harness refusing a job while the game loads or processes: its error line.
		let busy = "error: FM24 is busy (processing kind 2); wait until it is idle"
		let refused = HarnessResult(status: 1, output: ["job submitted; waiting...", busy])
		checkEqual(refused.problem(fallback: fallback, serving: notServing), busy)
		// Exit status 2, arguments fm24ctl refuses (such as a seed range that runs backwards): its
		// reason, not its usage line, without asking about the socket.
		let reason = "the seed range 10-1 runs backwards: write 1-10"
		let usage = HarnessResult(
			status: HarnessResult.usage,
			errors: [reason, "usage: fm24ctl run <input.bin|.json> [seeds] [output.csv]"]
		)
		checkEqual(
			usage.diagnosis(fallback: fallback, serving: notServing),
			HarnessProblem(message: reason)
		)
		// Exit status 3, the game not in the state the job needs: fm24ctl's last line of errors,
		// which says why, without asking about the socket; then as other failures.
		let otherSave =
			"FM24 has 'A' loaded, not 'B'; fm24ctl will not switch saves in a running game (unsaved "
			+ "progress). Quit FM24 (saving first only if you loaded that save yourself and have done "
			+ "no harness work since), then run this again, or pass --any-save to use the loaded save."
		var wrongState = HarnessResult(
			status: HarnessResult.wrongState,
			output: ["FM24 state: busy"],
			errors: ["the harness cannot read which save FM24 has loaded", otherSave]
		)
		checkEqual(wrongState.problem(fallback: fallback, serving: notServing), otherSave)
		checkEqual(asked, 4)
		// Which Run Anyway answers: another save loaded, not the start screen or a launch's refusal,
		// which has no --any-save.
		check(wrongState.refusedOtherSave)
		checkEqual(
			ViewerModel.otherSaveQuestion(otherSave),
			"FM24 has 'A' loaded, not 'B'; fm24ctl will not switch saves in a running game (unsaved "
				+ "progress). Quit FM24 (saving first only if you loaded that save yourself and have "
				+ "done no harness work since), then run this again, or choose Run Anyway to use the "
				+ "loaded save."
		)
		for errors in [
			[
				"FM24 (pid 7) is at its start screen with no save loaded: load B in FM24, or quit FM24 and "
					+ "run this again."
			],
			[String(otherSave.dropLast(HarnessResult.anySaveHint.count + 1)) + "."],
		] {
			check(!HarnessResult(status: 3, errors: errors).refusedOtherSave, "\(errors)")
		}
		check(!HarnessResult(status: 1, errors: [otherSave]).refusedOtherSave)
		wrongState.errors = [HarnessResult.waitingNotice]
		checkEqual(wrongState.problem(fallback: fallback, serving: serving), fallback)
		checkEqual(asked, 5)
		check(!wrongState.refusedOtherSave)
		// Only a harness that does not serve offers to launch the game.
		checkEqual(
			HarnessResult(status: 1, errors: ["not running"])
				.diagnosis(fallback: fallback, serving: notServing),
			HarnessProblem(message: "\(HarnessResult.notServing)\nnot running", offersLaunch: true)
		)
		checkEqual(
			HarnessResult(status: 1, errors: ["cannot read it"])
				.diagnosis(fallback: fallback, serving: serving),
			HarnessProblem(message: "cannot read it")
		)
		checkEqual(
			HarnessResult(status: 1, output: ["error: x"]).diagnosis(
				fallback: fallback,
				serving: notServing
			),
			HarnessProblem(message: "error: x")
		)
		// fm24ctl's notices before a job never explain its failure: run's warning of another date,
		// and the save it could not read or check, before the job's own reason or none.
		let notices = [
			"warning: the save is at 2024 day 130 09:00, the fixture was exported at 2024 day 123 "
				+ "15:00; the results reflect the save",
			"the harness cannot read which save FM24 has loaded; taking it to be LIV_MAIN_FM24",
			"cannot check which save FM24 has loaded; going ahead",
		]
		let lost = HarnessResult(
			status: 1,
			output: ["results: /T/run.csv", "job submitted; waiting..."],
			errors: notices + ["cannot read a complete result; see harness.log"]
		)
		checkEqual(
			lost.diagnosis(fallback: fallback, serving: notServing),
			HarnessProblem(
				message: "\(HarnessResult.notServing)\ncannot read a complete result; see harness.log",
				offersLaunch: true
			)
		)
		checkEqual(
			lost.problem(fallback: fallback, serving: serving),
			"cannot read a complete result; see harness.log"
		)
		let noticesOnly = HarnessResult(status: 1, errors: notices)
		checkEqual(
			noticesOnly.diagnosis(fallback: fallback, serving: notServing),
			HarnessProblem(message: HarnessResult.notServing, offersLaunch: true)
		)
		checkEqual(noticesOnly.problem(fallback: fallback, serving: serving), fallback)
		checkEqual(
			HarnessResult(status: 3, errors: [notices[0], otherSave])
				.problem(fallback: fallback, serving: serving),
			otherSave
		)
		// launch and install: every line of errors, whatever the socket.
		checkEqual(HarnessResult(status: 0).explanation(fallback: fallback), nil)
		checkEqual(
			HarnessResult(status: 1, output: ["loading"], errors: ["no save named X", " ", "saves: Y"])
				.explanation(fallback: fallback),
			"no save named X\nsaves: Y"
		)
		checkEqual(
			HarnessResult(status: 1, output: ["error: busy", "done"]).explanation(fallback: fallback),
			"error: busy"
		)
		checkEqual(HarnessResult(status: 2).explanation(fallback: fallback), fallback)
		checkEqual(unstarted.explanation(fallback: fallback), "cannot run fm24ctl (missing)")
	}

	static func locates() {
		let scratch = Scratch()
		let plain = Bundle(path: scratch.url.path)!
		checkEqual(Fm24ctl.locate(environment: [:], bundle: plain), nil)
		checkEqual(
			Fm24ctl.locate(environment: ["FM24CTL": "/opt/stand-in"], bundle: plain)?.path,
			"/opt/stand-in"
		)
		// Inside an app bundle: its own copy, never a build folder's.
		let app = scratch.url.appendingPathComponent("FM24 Viewer.app")
		let executables = app.appendingPathComponent("Contents/MacOS")
		try? FileManager.default.createDirectory(at: executables, withIntermediateDirectories: true)
		let info: [String: Any] = [
			"CFBundleIdentifier": "io.github.graemerycyk.fm24-match-harness.viewer-tests",
			"CFBundleExecutable": "fm24viewer",
			"CFBundlePackageType": "APPL",
		]
		let plist = try? PropertyListSerialization.data(
			fromPropertyList: info,
			format: .xml,
			options: 0
		)
		try? plist?.write(to: app.appendingPathComponent("Contents/Info.plist"))
		for name in ["fm24viewer", "fm24ctl"] {
			let file = executables.appendingPathComponent(name)
			FileManager.default.createFile(atPath: file.path, contents: Data("#!/bin/sh\n".utf8))
			try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: file.path)
		}
		guard let bundle = Bundle(url: app) else {
			check(false, "cannot make a bundle")
			return
		}
		checkEqual(
			Fm24ctl.locate(environment: [:], bundle: bundle)?.resolvingSymlinksInPath().path,
			executables.appendingPathComponent("fm24ctl").resolvingSymlinksInPath().path
		)
		checkEqual(Fm24ctl.locate(environment: ["FM24CTL": ""], bundle: plain), nil)
	}

	static func runsExactly() {
		let scratch = Scratch()
		guard let standIn = StandIn(scratch) else {
			return
		}
		let harness = standIn.harness
		var lines: [HarnessLine] = []
		var ran: [[String]] = []
		harness.onRun = { ran.append($0) }
		let output = scratch.url.appendingPathComponent("T/temp-fixture.json")
		try? FileManager.default.createDirectory(
			at: output.deletingLastPathComponent(),
			withIntermediateDirectories: true
		)
		standIn.set(["STDIN": "1", "STDERR": "a notice\n", "STDOUT": "ok: exported\nlast"])
		let exported = result { await harness.exportNextFixture(to: output) { lines.append($0) } }
		checkEqual(exported?.status, 0)
		checkEqual(exported?.output, ["stdin: 0 bytes", "ok: exported", "last"])
		checkEqual(exported?.errors, ["a notice"])
		// Each stream's lines come in order, before the result (the streams may interleave).
		checkEqual(
			lines.filter { if case .output = $0 { return true } else { return false } },
			[.output("stdin: 0 bytes"), .output("ok: exported"), .output("last")]
		)
		checkEqual(lines.filter { $0 == .error("a notice") }.count, 1)
		checkEqual(lines.count, 4)
		check(FileManager.default.fileExists(atPath: output.path), "the export was not written")

		standIn.set(["STATUS": "3", "STDOUT": "error: not a binary fixture\n"])
		let input = scratch.file("in-1.bin", Data([1]))
		let decoded = scratch.url.appendingPathComponent("decoded 2.json")
		let failed = result { await harness.decode(input, to: decoded) { _ in } }
		checkEqual(failed?.status, 3)
		checkEqual(failed?.output, ["error: not a binary fixture"])
		check(!FileManager.default.fileExists(atPath: decoded.path))
		standIn.set([:])
		let played = scratch.file("in 3.json", plainSampleJSON())
		let results = scratch.url.appendingPathComponent("run 4.csv")
		let options = SimulationOptions(
			threads: 4,
			seeds: "1,3-4",
			detailed: true,
			keepBuild: false,
			total: 3
		)
		let simulated = result {
			await harness.simulate(played, options: options, output: results, anySave: false) { _ in }
		}
		checkEqual(simulated?.status, 0)
		checkEqual(simulated?.output.first, "results: \(results.path)")
		check(FileManager.default.fileExists(atPath: results.path), "no results were written")
		var kept = options
		kept.keepBuild = true
		kept.detailed = false
		_ = result {
			await harness.simulate(played, options: kept, output: results, anySave: true) { _ in }
		}
		let replaced = scratch.url.appendingPathComponent("out 5.json")
		_ = result {
			await harness.replaceTeam(
				side: 1,
				input: played,
				team: "Luton Town",
				output: replaced,
				anySave: true
			) { _ in }
		}
		_ = result {
			await harness.injectPlayer(
				input: played,
				block: "home.player[3]",
				person: "40347",
				output: replaced,
				allowDuplicate: true,
				anySave: true
			) { _ in }
		}
		_ = result { await harness.launch(save: "LIV_MAIN_FM24", rosetta: false) { _ in } }
		_ = result { await harness.launch(save: nil, rosetta: false) { _ in } }
		_ = result { await harness.launch(save: "/Saves/A Copy.fm", rosetta: true) { _ in } }
		_ = result { await harness.install { _ in } }
		let expected = [
			["get-next-fixture", output.path],
			["decode", input.path, decoded.path],
			["run", played.path, "1,3-4", results.path, "--threads", "4", "--detailed"],
			["run", played.path, "1,3-4", results.path, "--threads", "4", "--keep-build", "--any-save"],
			["replace-away", played.path, "Luton Town", replaced.path, "--any-save"],
			[
				"inject-player", played.path, "home.player[3]", "40347", replaced.path, "--allow-duplicate",
				"--any-save",
			],
			["launch", "--save", "LIV_MAIN_FM24"],
			["launch"],
			["launch", "--save", "/Saves/A Copy.fm", "--rosetta"],
			["install"],
		]
		checkEqual(standIn.runs, expected)
		checkEqual(ran, expected)
	}

	static func drainsPipes() {
		let scratch = Scratch()
		guard let standIn = StandIn(scratch) else {
			return
		}
		// About 1.3 MB on each pipe, far past what a pipe holds unread.
		standIn.set(["FLOOD": "20000", "STATUS": "1"])
		var delivered = [0, 0]
		var lastError = ""
		let flooded = result {
			await standIn.harness.decode(scratch.file("in.bin", Data([1])), to: scratch.file("out.json"))
			{
				line in
				switch line {
				case .output: delivered[0] += 1
				case .error(let text):
					delivered[1] += 1
					lastError = text
				}
			}
		}
		guard let flooded else {
			return
		}
		checkEqual(flooded.status, 1)
		checkEqual(delivered, [20_001, 20_000])
		checkEqual(lastError, "err 20000: the harness wrote a line of errors to fill the pipe")
		// The first line and the latest ones are kept.
		check(flooded.output.count <= 20_000 && flooded.errors.count <= 20_000)
		checkEqual(flooded.output.first, "out 1: the harness wrote a line of output to fill the pipe")
		checkEqual(flooded.output.last, "ok: stand-in")
		checkEqual(flooded.errors.first, "err 1: the harness wrote a line of errors to fill the pipe")
		checkEqual(flooded.errors.last, lastError)
		checkEqual(
			flooded.errors.dropLast().last,
			"err 19999: the harness wrote a line of errors to fill the pipe"
		)
	}

	static func ignoresChildren() {
		let scratch = Scratch()
		guard let standIn = StandIn(scratch) else {
			return
		}
		// As the game does after launch, a child keeps fm24ctl's pipes open after it exits.
		standIn.set(["LINGER": "4000", "STDOUT": "state: idle\nok"])
		let started = Date()
		let launched = result { await standIn.harness.launch(save: nil, rosetta: false) { _ in } }
		check(Date().timeIntervalSince(started) < 2.5, "waited for the child")
		checkEqual(launched?.status, 0)
		checkEqual(launched?.output, ["state: idle", "ok"])
	}

	static func reportsStartFailure() {
		let scratch = Scratch()
		let missing = Fm24ctl(executable: scratch.url.appendingPathComponent("missing")) { false }
		let failed = result { await missing.launch(save: nil, rosetta: false) { _ in } }
		checkEqual(failed?.started, false)
		check(failed?.succeeded == false)
		let problem = failed?.problem(fallback: "fallback") { false } ?? ""
		check(problem.hasPrefix("cannot run fm24ctl ("), problem)
	}

	static func endsCancelledJobs() {
		let scratch = Scratch()
		guard let standIn = StandIn(scratch) else {
			return
		}
		// Waiting for another job, as the startup export may for up to ten minutes.
		standIn.set(["STDERR": HarnessResult.waitingNotice + "\n", "DELAY_MS": "5000"])
		let output = scratch.file("next.json")
		var lines: [HarnessLine] = []
		var result: HarnessResult?
		let job = Task {
			result = await standIn.harness.exportNextFixture(to: output) { lines.append($0) }
		}
		check(waitUntil { !lines.isEmpty }, "fm24ctl did not start")
		let cancelled = Date()
		job.cancel()
		check(waitUntil(seconds: 4) { result != nil }, "fm24ctl did not end")
		check(Date().timeIntervalSince(cancelled) < 2, "fm24ctl ran on")
		checkEqual(result?.status, 128 + SIGTERM)
		checkEqual(lines, [.error(HarnessResult.waitingNotice)])
		check(!FileManager.default.fileExists(atPath: output.path), "the export was written")
		// Cancelled before fm24ctl has started: it ends as soon as it starts.
		var early: HarnessResult?
		let before = Task {
			early = await standIn.harness.exportNextFixture(to: output) { _ in }
		}
		before.cancel()
		let started = Date()
		check(waitUntil(seconds: 4) { early != nil }, "fm24ctl did not end")
		check(Date().timeIntervalSince(started) < 2, "fm24ctl ran on")
		checkEqual(early?.status, 128 + SIGTERM)
		check(!FileManager.default.fileExists(atPath: output.path), "the export was written")
	}

	static func keepsWorkFiles() {
		let scratch = Scratch()
		let work = WorkFiles(directory: scratch.url, processID: 4242)
		checkEqual(work.folder.path, scratch.url.appendingPathComponent("fm24-viewer-4242").path)
		checkEqual(
			work.temporaryFixture.path,
			scratch.url.appendingPathComponent("temp-fixture.json").path
		)
		let first = try? work.file("in", extension: "bin")
		let second = try? work.file("decoded", extension: "json")
		checkEqual(first?.lastPathComponent, "in-1.bin")
		checkEqual(second?.lastPathComponent, "decoded-2.json")
		checkEqual(first?.deletingLastPathComponent().path, work.folder.path)
		let permissions =
			(try? FileManager.default.attributesOfItem(atPath: work.folder.path))?[
				.posixPermissions
			] as? Int
		checkEqual(permissions, 0o700)
		if let first, let second {
			try? Data([1]).write(to: first)
			work.remove(first, second)
			checkEqual(scratch.names("fm24-viewer-4242"), [])
		}
		work.removeFolder()
		checkEqual(scratch.names(), [])

		// The export that fm24ctl with no arguments writes, however its folder is spelt.
		check(work.isTemporaryFixture(scratch.url.appendingPathComponent("temp-fixture.json")))
		check(!work.isTemporaryFixture(scratch.url.appendingPathComponent("next.json")))
		let real = WorkFiles()
		let temporary = real.temporaryFixture.path
		let other =
			temporary.hasPrefix("/private/") ? String(temporary.dropFirst(8)) : "/private" + temporary
		check(real.isTemporaryFixture(URL(fileURLWithPath: other)), other)
		// The same directory as the job socket's: fm24ctl and the harness share it.
		var socket = [CChar](repeating: 0, count: 1100)
		let size = socket.count
		check(channel_socketPath(&socket, size))
		checkEqual(
			URL(fileURLWithPath: String(cString: socket)).deletingLastPathComponent().standardizedFileURL
				.path,
			WorkFiles.userTemporaryDirectory().standardizedFileURL.path
		)
	}

	static func sweepsWorkFolders() {
		let scratch = Scratch()
		let old = Date(timeIntervalSinceNow: -2 * WorkFiles.staleAge)
		for name in [
			"fm24-viewer-101", "fm24-viewer-102", "fm24-viewer-103", "fm24-viewer-x", "other-104",
		] {
			let folder = scratch.url.appendingPathComponent(name)
			try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
			try? Data([1]).write(to: folder.appendingPathComponent("in-1.bin"))
			let date = name == "fm24-viewer-103" ? Date() : old
			try? FileManager.default.setAttributes([.modificationDate: date], ofItemAtPath: folder.path)
		}
		// 101 has gone and is old; 102 still runs; 103 has gone but is recent.
		WorkFiles.sweep(scratch.url) { $0 == 102 }
		checkEqual(
			scratch.names(),
			["fm24-viewer-102", "fm24-viewer-103", "fm24-viewer-x", "other-104"]
		)
		check(WorkFiles.processAlive(getpid()))
		check(!WorkFiles.processAlive(999_999))
	}

	static func keepsExportFolder() {
		let store = MemorySettings()
		let documents = URL(fileURLWithPath: "/Users/example/Documents")
		let settings = ViewerSettings(defaults: store, documents: documents)
		checkEqual(settings.exportFolder.path, "/Users/example/Documents/FM24 Match Harness")
		settings.exportFolder = URL(fileURLWithPath: "/Volumes/Data/Fixtures")
		checkEqual(store.values[ViewerSettings.exportFolderKey] as? String, "/Volumes/Data/Fixtures")
		checkEqual(ViewerSettings(defaults: store).exportFolder.path, "/Volumes/Data/Fixtures")
		store.set("relative", forKey: ViewerSettings.exportFolderKey)
		checkEqual(settings.exportFolder, settings.defaultExportFolder)
		check(ViewerSettings().defaultExportFolder.path.hasSuffix("/Documents/FM24 Match Harness"))
	}

	static func followsGame() {
		var running = false
		var serving = false
		var optionSet = true
		var checks = 0
		let monitor = GameMonitor(
			isRunning: { running },
			isServing: {
				checks += 1
				return serving
			},
			launchOptionSet: { optionSet },
			interval: 0.02,
			checks: 50
		)
		var quits = 0
		monitor.onQuit = { quits += 1 }
		checkEqual(monitor.state, .unknown)
		checkEqual(monitor.description, nil)
		checkEqual(checks, 0)
		monitor.refresh()
		checkEqual(monitor.state, .notRunning)
		checkEqual(
			monitor.description,
			"FM24 is not running. Choose Launch FM24… to start it with the harness and a save, or start "
				+ "it from Steam with the harness and load a save, then choose Retry."
		)
		// Without Steam's launch option, which is read with the game's state, setting it comes
		// first: Launch FM24… would start the game directly.
		let directStart =
			"Launch FM24… would open it without the option, which has not been verified: the game "
			+ "crashed in its one attempt."
		optionSet = false
		monitor.refresh()
		check(!monitor.optionSet)
		checkEqual(
			monitor.description,
			"FM24 is not running. Set Steam's launch option for it once (File > Install Harness…), then "
				+ "start it from Steam, load a save and choose Retry. \(directStart)"
		)
		checkEqual(
			GameState.runningWithoutHarness.description(optionSet: false),
			"FM24 is running without harness \(FixtureHandle.protocolVersion): quit it, set Steam's "
				+ "launch option for it once (File > Install Harness…), then start it from Steam. "
				+ directStart
		)
		optionSet = true
		// Started: checked until its harness serves, then no more.
		running = true
		monitor.gameStarted()
		checkEqual(monitor.state, .runningWithoutHarness)
		check(monitor.optionSet)
		checkEqual(
			monitor.description,
			"FM24 is running without harness \(FixtureHandle.protocolVersion): quit it, then choose "
				+ "Launch FM24… to start it with the harness."
		)
		check(waitUntil { checks >= 4 }, "the harness was not looked for")
		serving = true
		check(waitUntil { monitor.state == .ready })
		checkEqual(
			monitor.description,
			"FM24 is running with harness \(FixtureHandle.protocolVersion)."
		)
		checkEqual(
			GameState.ready.description(optionSet: false),
			"FM24 is running with harness \(FixtureHandle.protocolVersion)."
		)
		let settled = checks
		RunLoop.main.run(until: Date().addingTimeInterval(0.2))
		checkEqual(checks, settled)
		// A check already made counts.
		monitor.refresh(serving: false)
		checkEqual(monitor.state, .runningWithoutHarness)
		checkEqual(checks, settled)
		running = false
		serving = false
		monitor.gameQuit()
		checkEqual(monitor.state, .notRunning)
		checkEqual(quits, 1)
		checkEqual(GameState(running: false, serving: true), .ready)
	}
}
