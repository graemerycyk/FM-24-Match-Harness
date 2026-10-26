// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureSamples
import Foundation

/// Launching FM24 from the viewer (the Launch FM24 sheet and --launch-save) and installing the
/// harness, through fm24ctl's stand-in: the saves listed from made-up save folders, which fm24ctl's
/// own tests search too (tests/save_samples.h), fm24ctl's exact arguments, its progress, Cancel,
/// what follows success and failure, the remembered choice, and the install's launch option.
@MainActor
enum LaunchTests {
	static func register() {
		Tests.test("the Launch FM24 sheet lists the saves fm24ctl finds", listsSaves)
		Tests.test("the sheet offers no save where fm24ctl cannot load one", offersNoSave)
		Tests.test("Launch FM24 starts the game with a save, then exports its fixture", launches)
		Tests.test("Launch FM24 remembers what it last loaded", remembers)
		Tests.test("Launch FM24 over a fixture reports how the launch ended", reportsOutcome)
		Tests.test("Cancel ends fm24ctl launch", cancels)
		Tests.test("--launch-save starts the game, then exports its fixture", launchesFromArgument)
		Tests.test("the viewer's arguments are read", readsArguments)
		Tests.test("Install Harness shows the Steam launch option to copy", installs)
		Tests.test(
			"the Launch FM24 sheet installs the harness instead without Steam's option",
			installsFromSheet
		)
		Tests.test(
			"errors offer Launch FM24, or Install Harness without Steam's option, when no harness serves",
			offersLaunch
		)
	}

	private static let readyDate = "2024 day 123 15:00"
	private static let unclearNote =
		"Steam's launch option for FM24 sets its data folder (user_data_location) in a way fm24ctl "
		+ "does not follow, so FM24 Viewer cannot load a save: launch FM24 at its start screen and "
		+ "load the save there, or give the folder in the option as "
		+ "--user_data_location=<absolute folder>."

	private static func wait(_ model: ViewerModel) {
		check(waitUntil { !model.isBusy }, "the operation did not finish")
	}

	/// Waits until the launch has ended.
	private static func waitForLaunch(_ model: ViewerModel) {
		check(waitUntil { !model.launchRunning }, "fm24ctl launch did not end")
		wait(model)
	}

	/// The made-up saves for `option` in the setup's home folder.
	private static func makeSaves(_ setup: HarnessSetup, _ option: SaveSamplesOption) -> Bool {
		let made = saveSamples_make(setup.home.path, Int32(option.rawValue))
		check(made, "cannot make the save folders")
		return made
	}

	static func listsSaves() {
		for option in 0..<Int32(SAVE_SAMPLES_OPTION_COUNT.rawValue) {
			let scratch = Scratch()
			let home = scratch.url.appendingPathComponent("Home")
			guard saveSamples_make(home.path, option) else {
				check(false, "cannot make the save folders")
				return
			}
			let saves = LaunchSaves.read(home: home.path)
			var data = [CChar](repeating: 0, count: 2048)
			let size = data.count
			check(saveSamples_dataFolder(home.path, option, &data, size))
			let folder = String(cString: data)
			let unclear = option == Int32(SAVE_SAMPLES_UNCLEAR.rawValue)
			let steam = option == Int32(SAVE_SAMPLES_STEAM_FOLDER.rawValue)
			checkEqual(saves.data, unclear ? .unclear : steam ? .steam : .usual)
			checkEqual(saves.folder?.path, unclear ? nil : folder)
			let expected =
				unclear
				? []
				: (0..<Int(SAVE_SAMPLES_SAVES)).map { index -> LaunchSave in
					let sample = withUnsafePointer(to: saveSamples_saves) {
						$0.withMemoryRebound(to: SaveSample.self, capacity: Int(SAVE_SAMPLES_SAVES)) {
							$0[index]
						}
					}
					let folderName = String(cString: sample.folder)
					return LaunchSave(
						name: String(cString: sample.name),
						url: URL(fileURLWithPath: "\(folder)/\(folderName)/\(String(cString: sample.file))"),
						folder: folderName,
						modified: Date(
							timeIntervalSince1970: TimeInterval(SAVE_SAMPLES_TIME) + TimeInterval(sample.age)
						)
					)
				}
			checkEqual(saves.saves, expected)
			let notes = [
				"Saves in ~/Library/Application Support/Sports Interactive/Football Manager 2024, newest "
					+ "first.",
				"Saves in ~/FM Data, the data folder Steam's launch option gives FM24, newest first.",
				unclearNote,
			]
			checkEqual(saves.note, notes[unclear ? 2 : steam ? 1 : 0])
			// Whether Steam's launch option runs the harness's launcher, for the sheet's warning
			// before a direct start.
			let otherLauncher = option == Int32(SAVE_SAMPLES_OTHER_LAUNCHER.rawValue)
			let noOption = option == Int32(SAVE_SAMPLES_NO_OPTION.rawValue)
			checkEqual(saves.optionSet, !noOption && !otherLauncher)
			checkEqual(LaunchSaves.launchOptionSet(home: home.path), saves.optionSet)
		}
		// No saves, or no home folder.
		let scratch = Scratch()
		let empty = LaunchSaves.read(home: scratch.url.path)
		checkEqual(empty.data, .usual)
		checkEqual(empty.saves, [])
		check(!empty.optionSet)
		check(!LaunchSaves.read(home: "relative").optionSet)
		check(empty.note.hasPrefix("No saves in cloud/games or games of "), empty.note)
		checkEqual(LaunchSaves.read(home: "relative").data, .noHome)
		checkEqual(LaunchSaves.read(home: "").folder, nil)
		// Paths in the home folder are shown from ~.
		checkEqual(LaunchSaves.abbreviated("/Users/a/Library/x.fm", home: "/Users/a"), "~/Library/x.fm")
		checkEqual(LaunchSaves.abbreviated("/Users/a/x.fm", home: "/Users/a/"), "~/x.fm")
		checkEqual(LaunchSaves.abbreviated("/Users/ab/x.fm", home: "/Users/a"), "/Users/ab/x.fm")
		checkEqual(LaunchSaves.abbreviated("/Users/a", home: "/Users/a"), "/Users/a")
		checkEqual(LaunchSaves.abbreviated("/x.fm", home: ""), "/x.fm")
	}

	static func offersNoSave() {
		guard let setup = HarnessSetup(), makeSaves(setup, SAVE_SAMPLES_UNCLEAR) else {
			return
		}
		let model = setup.model
		// Steam's launch option sets the data folder in a way fm24ctl does not follow, so it refuses
		// any save: the sheet offers only the start screen, whatever was last chosen, and Other…
		// chooses nothing.
		let remembered = setup.scratch.file("Remembered.fm", Data("fmf.".utf8))
		setup.settings.launchSave = remembered.path
		model.requestLaunch()
		guard let prompt = model.launchPrompt else {
			check(false, "the sheet did not open")
			return
		}
		check(!prompt.saves.loadsSaves)
		checkEqual(prompt.saves.note, unclearNote)
		checkEqual(prompt.choice, .startScreen)
		checkEqual(prompt.other, nil)
		check(!prompt.offersOther)
		setup.prompts.saveToChoose = remembered
		model.chooseOtherSave()
		checkEqual(setup.prompts.offeredSaveFolder, nil)
		checkEqual(prompt.choice, .startScreen)
		model.chooseLaunch(.save(remembered))
		checkEqual(prompt.choice, .startScreen)
		setup.standIn.set(["UNCLEAR": "1"])
		model.confirmLaunch()
		waitForLaunch(model)
		checkEqual(setup.standIn.runs, [["launch"]])
		// The forced choice is not remembered.
		checkEqual(setup.settings.launchSave, remembered.path)
		checkEqual(model.status, ViewerModel.launchedWithoutSave)
		// --launch-save still passes a save on, and fm24ctl's refusal says what the viewer can do,
		// in place of its --user-data.
		guard let failing = HarnessSetup(), makeSaves(failing, SAVE_SAMPLES_UNCLEAR) else {
			return
		}
		failing.standIn.set(["UNCLEAR": "1"])
		failing.model.launch(save: remembered.path, rosetta: false)
		waitForLaunch(failing.model)
		let refusal =
			"Steam's launch option for FM24 sets its data folder (user_data_location) in a way fm24ctl "
			+ "does not follow; give it in Steam's launch option as --user_data_location=<absolute "
			+ "folder>, or launch FM24 at its start screen and load the save there"
		checkEqual(failing.model.launchPrompt?.phase, .failed(refusal))
		checkEqual(failing.standIn.runs, [["launch", "--save", remembered.path]])
		check(failing.prompts.errors.isEmpty, "\(failing.prompts.errors)")
	}

	static func launches() {
		guard let setup = HarnessSetup(), makeSaves(setup, SAVE_SAMPLES_LAUNCHER) else {
			return
		}
		let model = setup.model
		setup.standIn.set([
			"LAUNCH_MS": "800", "LOAD_MS": "400", "DATE": readyDate,
			"FIXTURE": setup.scratch.file("next.json", plainSampleJSON(saved: true)).path,
		])
		model.requestLaunch()
		guard let prompt = model.launchPrompt else {
			check(false, "the sheet did not open")
			return
		}
		check(model.isAsking)
		check(prompt.offersRosetta)
		check(!prompt.rosetta)
		// The newest save, first.
		let newest = prompt.saves.saves.first
		checkEqual(newest?.name, "LIV_MAIN_FM24")
		checkEqual(newest?.folder, "cloud/games")
		checkEqual(prompt.choice, newest.map { .save($0.url) })
		checkEqual(prompt.phase, .choosing)
		model.confirmLaunch()
		checkEqual(prompt.phase, .launching)
		check(model.launchRunning)
		// fm24ctl's progress, line by line as it comes.
		check(
			waitUntil {
				prompt.lines.contains("waiting for the game to start the harness...")
			},
			"no progress arrived"
		)
		check(prompt.isLaunching)
		check(
			waitUntil { prompt.lines.contains("still waiting (60 s)...") },
			"the wait's progress did not arrive"
		)
		check(prompt.isLaunching)
		check(waitUntil { prompt.lines.contains("FM24 state: loading") }, "the load did not start")
		check(prompt.isLaunching)
		// Ready: the sheet closes and the startup export shows the next fixture.
		waitForLaunch(model)
		check(model.launchPrompt == nil)
		check(model.isTemporary)
		checkEqual(model.fixture?.saveLine, "Save: \(FIXTURE_SAMPLE_SAVE), \(FIXTURE_SAMPLE_GAME_DATE)")
		let path = newest?.url.path ?? ""
		checkEqual(
			setup.standIn.runs,
			[
				["launch", "--save", path],
				["get-next-fixture", setup.temporary.appendingPathComponent("temp-fixture.json").path],
			]
		)
		let ready = prompt.lines.last ?? ""
		check(
			ready.hasPrefix("ready: LIV_MAIN_FM24 loaded (pid ") && ready.hasSuffix(", \(readyDate))"),
			ready
		)
		check(model.log.text.contains("fm24ctl: FM24 state: ready"), model.log.text)
		checkEqual(setup.settings.launchSave, path)
		checkEqual(setup.settings.launchRosetta, false)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	static func remembers() {
		guard let setup = HarnessSetup(), makeSaves(setup, SAVE_SAMPLES_LAUNCHER) else {
			return
		}
		let model = setup.model
		setup.standIn.set(["RUNNING": "1"])
		// Nothing is remembered until a launch starts.
		model.requestLaunch()
		guard let first = model.launchPrompt, first.saves.saves.count > 1 else {
			check(false, "the sheet did not open")
			return
		}
		let second = first.saves.saves[1].url
		model.chooseLaunch(.save(second))
		model.setLaunchRosetta(true)
		model.cancelLaunch()
		check(model.launchPrompt == nil)
		checkEqual(setup.settings.launchSave, nil)
		checkEqual(setup.settings.launchRosetta, false)
		// The start screen, under Rosetta: FM24 runs with the harness, and the start panel says to
		// load a save, as nothing is exported without one.
		model.requestLaunch()
		checkEqual(
			model.launchPrompt?.launchingText,
			"Starting FM24 with LIV_MAIN_FM24; this can take several minutes."
		)
		model.chooseLaunch(.startScreen)
		checkEqual(
			model.launchPrompt?.launchingText,
			"Starting FM24 at its start screen; this can take several minutes."
		)
		model.setLaunchRosetta(true)
		model.confirmLaunch()
		waitForLaunch(model)
		checkEqual(setup.standIn.runs.last, ["launch", "--rosetta"])
		check(
			model.log.text.contains("Launching FM24 at its start screen, under Rosetta"),
			model.log.text
		)
		check(model.launchPrompt == nil)
		check(model.fixture == nil)
		checkEqual(model.status, ViewerModel.launchedWithoutSave)
		checkEqual(model.retry, .export)
		checkEqual(setup.settings.launchSave, "")
		checkEqual(setup.settings.launchRosetta, true)
		model.requestLaunch()
		checkEqual(model.launchPrompt?.choice, .startScreen)
		checkEqual(model.launchPrompt?.rosetta, true)
		// Another save, natively: it then exports the next fixture.
		model.chooseLaunch(.save(second))
		model.setLaunchRosetta(false)
		model.confirmLaunch()
		waitForLaunch(model)
		checkEqual(
			setup.standIn.runs.suffix(2).map { $0.first ?? "" },
			["launch", "get-next-fixture"]
		)
		checkEqual(setup.standIn.runs.dropLast().last, ["launch", "--save", second.path])
		check(model.isTemporary)
		checkEqual(setup.settings.launchSave, second.path)
		checkEqual(setup.settings.launchRosetta, false)
		model.requestLaunch()
		checkEqual(model.launchPrompt?.choice, .save(second))
		checkEqual(model.launchPrompt?.rosetta, false)
		// Other…: a .fm file anywhere, offered from the save folder; it stays offered later.
		let copy = setup.scratch.file("Sandbox Copy.fm", Data("fmf.".utf8))
		setup.prompts.saveToChoose = copy
		model.chooseOtherSave()
		checkEqual(
			setup.prompts.offeredSaveFolder?.path,
			model.launchPrompt?.saves.folder?.appendingPathComponent("cloud/games").path
		)
		checkEqual(model.launchPrompt?.choice, .save(copy))
		checkEqual(model.launchPrompt?.other, copy)
		setup.prompts.saveToChoose = nil
		model.chooseOtherSave()
		checkEqual(model.launchPrompt?.choice, .save(copy))
		model.confirmLaunch()
		waitForLaunch(model)
		checkEqual(setup.standIn.runs.last, ["launch", "--save", copy.path])
		checkEqual(setup.settings.launchSave, copy.path)
		model.cancelLaunch()
		check(model.launchPrompt == nil)
		model.requestLaunch()
		checkEqual(model.launchPrompt?.other, copy)
		checkEqual(model.launchPrompt?.choice, .save(copy))
		model.cancelLaunch()
		// A remembered file that has gone: the newest save.
		try? FileManager.default.removeItem(at: copy)
		model.requestLaunch()
		checkEqual(model.launchPrompt?.other, nil)
		checkEqual(
			model.launchPrompt?.choice,
			model.launchPrompt?.saves.saves.first.map { .save($0.url) }
		)
		model.cancelLaunch()
		// Without Apple silicon there is no Rosetta to offer.
		setup.settings.launchRosetta = true
		model.appleSilicon = false
		model.requestLaunch()
		checkEqual(model.launchPrompt?.offersRosetta, false)
		checkEqual(model.launchPrompt?.rosetta, false)
		model.setLaunchRosetta(true)
		checkEqual(model.launchPrompt?.rosetta, false)
		model.cancelLaunch()
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	static func reportsOutcome() {
		guard let setup = HarnessSetup(), makeSaves(setup, SAVE_SAMPLES_NO_OPTION) else {
			return
		}
		let model = setup.model
		let fixture = setup.scratch.file("next.json", plainSampleJSON(saved: true))
		model.open(fixture)
		wait(model)
		// The save is already loaded: the sheet says so, and nothing is exported.
		setup.standIn.set(["RUNNING": "1", "LOADED": "LIV_MAIN_FM24", "DATE": readyDate])
		model.requestLaunch()
		guard let prompt = model.launchPrompt else {
			check(false, "the sheet did not open")
			return
		}
		model.confirmLaunch()
		waitForLaunch(model)
		guard case .succeeded(let ready) = prompt.phase else {
			check(false, "the launch did not succeed: \(prompt.phase)")
			return
		}
		check(ready.hasPrefix("ready: LIV_MAIN_FM24 is already loaded (pid "), ready)
		check(model.launchPrompt === prompt)
		checkEqual(model.source?.url, fixture)
		checkEqual(setup.standIn.runs.map { $0.first ?? "" }, ["launch"])
		model.cancelLaunch()
		check(model.launchPrompt == nil)
		// Failures: fm24ctl's reason in the sheet, which stays open to try again.
		let otherSave =
			"FM24 has 'Other Career' loaded, not 'LIV_MAIN_FM24'; fm24ctl will not switch saves in a "
			+ "running game (unsaved progress). Quit FM24 (saving first only if you loaded that save "
			+ "yourself and have done no harness work since), then run this again."
		let cases: [([String: String], Bool, String)] = [
			(["RUNNING": "1", "LOADED": "Other Career"], false, otherSave),
			(
				["RUNNING": "1", "NATIVE": "1"], true,
				"natively, not under Rosetta; fm24ctl will not restart a running game. Quit it, then run "
					+ "this again."
			),
			(
				[
					"STATUS": "1", "STDOUT": "",
					"STDERR":
						"no save named LIV in /Data/cloud/games\nsaves whose names start with it: LIV_MAIN_FM24\n",
				],
				false,
				"no save named LIV in /Data/cloud/games\nsaves whose names start with it: LIV_MAIN_FM24"
			),
			(["STATUS": "1", "STDOUT": ""], false, ViewerModel.launchFailed),
		]
		for (settings, rosetta, problem) in cases {
			setup.standIn.set(settings)
			model.requestLaunch()
			guard let failing = model.launchPrompt else {
				check(false, "the sheet did not open")
				return
			}
			model.setLaunchRosetta(rosetta)
			model.confirmLaunch()
			waitForLaunch(model)
			guard case .failed(let reason) = failing.phase else {
				check(false, "the launch did not fail: \(failing.phase)")
				return
			}
			check(reason.hasSuffix(problem), reason)
			check(model.launchPrompt === failing)
			check(model.log.text.contains("Error: \(reason)"), model.log.text)
			model.cancelLaunch()
		}
		checkEqual(setup.standIn.runs.count, 5)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	static func cancels() {
		guard let setup = HarnessSetup(), makeSaves(setup, SAVE_SAMPLES_LAUNCHER) else {
			return
		}
		let model = setup.model
		setup.standIn.set(["LAUNCH_MS": "6000"])
		model.requestLaunch()
		guard let prompt = model.launchPrompt else {
			check(false, "the sheet did not open")
			return
		}
		model.confirmLaunch()
		check(
			waitUntil { prompt.lines.contains("waiting for the game to start the harness...") },
			"fm24ctl did not start"
		)
		checkEqual(model.closeCheck(), .close)
		let cancelled = Date()
		model.cancelLaunch()
		checkEqual(prompt.phase, .stopped)
		check(model.launchPrompt === prompt)
		check(waitUntil(seconds: 3) { !model.launchRunning }, "fm24ctl launch ran on")
		check(Date().timeIntervalSince(cancelled) < 2, "fm24ctl launch ran on")
		// Nothing more arrives from it, and nothing is exported.
		let lines = prompt.lines
		RunLoop.main.run(until: Date().addingTimeInterval(0.5))
		checkEqual(prompt.lines, lines)
		checkEqual(prompt.phase, .stopped)
		checkEqual(setup.standIn.runs.map { $0.first ?? "" }, ["launch"])
		check(model.log.text.contains(ViewerModel.launchStopped), model.log.text)
		check(model.fixture == nil)
		checkEqual(model.status, ViewerModel.launchStopped)
		checkEqual(model.retry, .export)
		// It can launch again, or close.
		model.cancelLaunch()
		check(model.launchPrompt == nil)
		// Quitting ends a launch too (AppDelegate).
		model.requestLaunch()
		model.confirmLaunch()
		check(model.launchRunning)
		let quit = Date()
		model.stopJobs()
		check(waitUntil(seconds: 3) { !model.launchRunning }, "fm24ctl launch ran on")
		check(Date().timeIntervalSince(quit) < 2, "fm24ctl launch ran on")
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	static func launchesFromArgument() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		setup.standIn.set([
			"LOAD_MS": "200",
			"FIXTURE": setup.scratch.file("next.json", plainSampleJSON(saved: true)).path,
		])
		// As the app starts: the launch, showing its progress in the sheet, then the export.
		let arguments = ViewerArguments.parse(["--launch-save", "LIV_MAIN_FM24", "--rosetta"])
		model.launch(save: arguments.launchSave ?? "", rosetta: arguments.rosetta)
		guard let prompt = model.launchPrompt else {
			check(false, "the sheet did not open")
			return
		}
		checkEqual(prompt.choice, .named("LIV_MAIN_FM24"))
		checkEqual(prompt.phase, .launching)
		waitForLaunch(model)
		check(model.launchPrompt == nil)
		check(model.isTemporary)
		checkEqual(setup.standIn.runs.first, ["launch", "--save", "LIV_MAIN_FM24", "--rosetta"])
		checkEqual(setup.standIn.runs.last?.first, "get-next-fixture")
		check(
			prompt.lines.contains(
				"starting Football Manager 2024 through Steam (its launch option loads the harness and "
					+ "starts the x86_64 slice under Rosetta)"
			),
			"\(prompt.lines)"
		)
		// A save given on the command line is not remembered.
		checkEqual(setup.settings.launchSave, nil)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		// A failure stays in the sheet, over the start panel, which keeps the reason once the sheet
		// has closed, with the game's state and Retry for the export.
		guard let failing = HarnessSetup() else {
			return
		}
		failing.standIn.set([
			"STATUS": "1", "STDOUT": "", "STDERR": "cannot find the save /x/Gone.fm\n",
		])
		failing.model.launch(save: "/x/Gone.fm", rosetta: false)
		waitForLaunch(failing.model)
		checkEqual(failing.model.launchPrompt?.phase, .failed("cannot find the save /x/Gone.fm"))
		check(failing.model.fixture == nil)
		checkEqual(failing.standIn.runs, [["launch", "--save", "/x/Gone.fm"]])
		failing.model.cancelLaunch()
		check(failing.model.launchPrompt == nil)
		checkEqual(failing.model.status, "cannot find the save /x/Gone.fm")
		checkEqual(failing.model.retry, .export)
		// So does Cancel.
		failing.standIn.set(["LAUNCH_MS": "6000"])
		failing.model.launch(save: "LIV_MAIN_FM24", rosetta: false)
		check(waitUntil { failing.model.launchPrompt?.lines.isEmpty == false }, "fm24ctl did not start")
		failing.model.cancelLaunch()
		checkEqual(failing.model.status, ViewerModel.launchStopped)
		checkEqual(failing.model.retry, .export)
		check(waitUntil { !failing.model.launchRunning }, "fm24ctl launch ran on")
		failing.model.cancelLaunch()
		failing.model.retryStart()
		wait(failing.model)
		checkEqual(failing.standIn.runs.last?.first, "get-next-fixture")
		check(failing.model.isTemporary)
		// --rosetta needs Apple silicon, as fm24ctl does: elsewhere it is not passed on.
		guard let intel = HarnessSetup() else {
			return
		}
		intel.model.appleSilicon = false
		intel.standIn.set([
			"FIXTURE": intel.scratch.file("next.json", plainSampleJSON(saved: true)).path
		])
		intel.model.launch(save: "LIV_MAIN_FM24", rosetta: true)
		checkEqual(intel.model.launchPrompt?.rosetta, false)
		waitForLaunch(intel.model)
		checkEqual(intel.standIn.runs.first, ["launch", "--save", "LIV_MAIN_FM24"])
		check(intel.model.log.text.contains(ViewerModel.rosettaIgnored), intel.model.log.text)
		check(intel.model.log.text.contains("Launching FM24 with LIV_MAIN_FM24"), intel.model.log.text)
		check(!intel.model.log.text.contains("under Rosetta"), intel.model.log.text)
		check(intel.model.isTemporary)
		check(intel.prompts.errors.isEmpty, "\(intel.prompts.errors)")
	}

	static func readsArguments() {
		func parse(_ arguments: String...) -> ViewerArguments {
			ViewerArguments.parse(arguments)
		}
		checkEqual(parse(), ViewerArguments())
		checkEqual(parse("next.json").fixture?.lastPathComponent, "next.json")
		check(parse("next.json").fixture?.path.hasPrefix("/") == true)
		let launch = parse("--launch-save", "LIV_MAIN_FM24")
		checkEqual(launch.launchSave, "LIV_MAIN_FM24")
		check(!launch.rosetta && launch.fixture == nil && launch.problems.isEmpty)
		let rosetta = parse("--rosetta", "--launch-save", "/Saves/A Copy.fm")
		checkEqual(rosetta.launchSave, "/Saves/A Copy.fm")
		check(rosetta.rosetta && rosetta.problems.isEmpty)
		// The value is the save's, whatever it looks like.
		let named = parse("--launch-save", "fixture.json", "other.bin")
		checkEqual(named.launchSave, "fixture.json")
		checkEqual(named.fixture?.lastPathComponent, "other.bin")
		// Without a value, or with a .fm path that is not absolute, which open would make relative
		// to "/", it launches nothing: the start panel says why.
		checkEqual(parse("--launch-save").launchProblem, ViewerArguments.missingSave)
		checkEqual(parse("--launch-save", "").launchSave, nil)
		checkEqual(parse("--launch-save", "").launchProblem, ViewerArguments.missingSave)
		for relative in ["Career.fm", "Saves/Career", "./A.FM"] {
			let refused = parse("--launch-save", relative, "--rosetta")
			checkEqual(refused.launchSave, nil)
			checkEqual(refused.launchProblem, ViewerArguments.relativeSave(relative))
			check(!refused.rosetta && refused.problems.isEmpty)
		}
		checkEqual(
			ViewerArguments.relativeSave("Career.fm"),
			"--launch-save takes a save's name or the absolute path of its .fm file, not Career.fm."
		)
		checkEqual(parse("--launch-save", "My Career").launchSave, "My Career")
		checkEqual(parse("--launch-save", "/Saves/Career.FM").launchSave, "/Saves/Career.FM")
		check(launch.launchProblem == nil && rosetta.launchProblem == nil)
		let alone = parse("--rosetta")
		check(!alone.rosetta)
		checkEqual(alone.problems, [ViewerArguments.rosettaAlone])
		// AppKit's own settings and anything that is not a fixture are left alone.
		let appKit = parse("-NSDocumentRevisionsDebugMode", "YES", "-psn_0_42", "a.bin", "b.json")
		checkEqual(appKit.fixture?.lastPathComponent, "a.bin")
		check(appKit.launchSave == nil && appKit.problems.isEmpty)
		checkEqual(parse("notes.txt", "-x").fixture, nil)
	}

	static func installs() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		var copied: [String] = []
		model.copyText = { copied.append($0) }
		let folder = setup.home.path + "/Library/Application Support/fm24-harness"
		let launcher = folder + "/launch-fm24.sh"
		let option = "/bin/sh \"\(launcher)\" %command%"
		let version = FixtureHandle.protocolVersion
		model.requestInstall()
		checkEqual(model.installState, .installing)
		check(model.isAsking)
		model.closeInstall()
		checkEqual(model.installState, .installing)
		check(waitUntil { model.installState != .installing }, "install did not finish")
		let game =
			setup.home.path
			+ "/Library/Application Support/Steam/steamapps/common/Football Manager 2024/fm.app"
		let expected = InstallReport(
			version: version,
			folder: folder,
			launcher: launcher,
			game: game,
			optionSet: false,
			launchOption: option
		)
		checkEqual(model.installState, .installed(expected))
		checkEqual(setup.standIn.runs, [["install"]])
		check(!model.installCopied)
		model.copyLaunchOption()
		checkEqual(copied, [option])
		check(model.installCopied)
		model.closeInstall()
		checkEqual(model.installState, nil)
		check(!model.isAsking)
		// Steam's option already set, and no game in Steam's libraries: where install looked, in
		// fm24ctl's words, as the stand-in prints them (viewer_stand_in_texts finds them in
		// macos_game.c).
		let steamFolder = setup.home.path + "/Library/Application Support/Steam"
		let noGame =
			"Install Football Manager 2024 (24.4.2) in Steam, then run `fm24ctl install` again; "
			+ "fm24ctl looks in the Steam libraries that \(steamFolder)/steamapps/libraryfolders.vdf "
			+ "lists."
		setup.standIn.set(["OPTION_SET": "1", "NO_GAME": "1"])
		model.requestInstall()
		check(!model.installCopied)
		check(waitUntil { model.installState != .installing }, "install did not finish")
		var set = expected
		set.optionSet = true
		set.game = nil
		set.notes = [noGame]
		checkEqual(model.installState, .installed(set))
		model.closeInstall()
		// No Steam, and the account's own option.
		let noSteam =
			"Steam is not in \(steamFolder), where fm24ctl looks for it: install Steam and Football "
			+ "Manager 2024 (24.4.2), then run `fm24ctl install` again."
		let own = "-windowed %command% -skip_intro"
		setup.standIn.set(["NO_STEAM": "1", "CURRENT_OPTION": own])
		model.requestInstall()
		check(waitUntil { model.installState != .installing }, "install did not finish")
		var another = expected
		another.game = nil
		another.currentOption = own
		another.notes = [noSteam]
		checkEqual(model.installState, .installed(another))
		model.closeInstall()
		// The option set, with a data folder fm24ctl does not follow.
		let unclear =
			"It sets FM24's data folder (user_data_location) in a way fm24ctl does not follow, "
			+ "which stops `fm24ctl launch --save` and the commands that start the game for a "
			+ "fixture's save: write it as --user_data_location=<absolute folder>."
		setup.standIn.set(["OPTION_SET": "1", "UNCLEAR": "1"])
		model.requestInstall()
		check(waitUntil { model.installState != .installing }, "install did not finish")
		var unfollowed = expected
		unfollowed.optionSet = true
		unfollowed.notes = [unclear]
		checkEqual(model.installState, .installed(unfollowed))
		model.closeInstall()
		// Failures: fm24ctl's reason.
		setup.standIn.set(["STATUS": "1", "STDOUT": "", "STDERR": "cannot write \(launcher)\n"])
		model.requestInstall()
		check(waitUntil { model.installState != .installing }, "install did not finish")
		checkEqual(model.installState, .failed("cannot write \(launcher)"))
		model.copyLaunchOption()
		checkEqual(copied.count, 1)
		model.closeInstall()
		setup.standIn.set(["STDOUT": "ok\n"])
		model.requestInstall()
		check(waitUntil { model.installState != .installing }, "install did not finish")
		checkEqual(model.installState, .failed(ViewerModel.installUnread))
		model.closeInstall()
		// A development build's harness, as fm24ctl reports it.
		let report = InstallReport.parse([
			"installed harness 1.3.0 (the development build next to fm24ctl): /H/f/fm24-harness-1.3.0.dylib",
			"launcher: /H/f/launch-fm24.sh", "log: /H/f/harness.log",
			"Football Manager 2024 was not found in your Steam libraries",
		])
		checkEqual(report?.version, "1.3.0")
		checkEqual(report?.folder, "/H/f")
		checkEqual(report?.launchOption, "/bin/sh \"/H/f/launch-fm24.sh\" %command%")
		checkEqual(report?.game, nil)
		checkEqual(report?.currentOption, nil)
		checkEqual(report?.notes, [])
		// Every piece of advice, in the order install gives it.
		let advised = InstallReport.parse([
			"installed harness 1.3.0: /H/f/fm24-harness-1.3.0.dylib", "launcher: /H/f/launch-fm24.sh",
			"Football Manager 2024 was not found in your Steam libraries", noSteam, noGame, "",
			"Steam's launch option for Football Manager 2024 is now: \(own)", "", unclear,
		])
		checkEqual(advised?.currentOption, own)
		checkEqual(advised?.notes, [noSteam, noGame, unclear])
		checkEqual(InstallReport.parse(["launcher: relative"]), nil)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	/// Without Steam's launch option, the sheet's Install Harness… closes it, as File > Install
	/// Harness… waits while it is open, and installs the harness; not while a launch runs. With the
	/// option, the sheet has no Install Harness….
	static func installsFromSheet() {
		guard let setup = HarnessSetup(serving: false) else {
			return
		}
		let model = setup.model
		setup.standIn.set(["LAUNCH_MS": "6000"])
		model.requestLaunch()
		guard let prompt = model.launchPrompt else {
			check(false, "the sheet did not open")
			return
		}
		check(!prompt.saves.optionSet && prompt.offersInstall)
		model.requestInstall()
		checkEqual(model.installState, nil)
		checkEqual(setup.prompts.lastError, ViewerModel.askingMessage)
		model.confirmLaunch()
		check(
			waitUntil { prompt.lines.contains("waiting for the game to start the harness...") },
			"fm24ctl did not start"
		)
		model.installInsteadOfLaunch()
		check(model.launchPrompt === prompt && model.installState == nil)
		model.cancelLaunch()
		checkEqual(prompt.phase, .stopped)
		check(waitUntil(seconds: 3) { !model.launchRunning }, "fm24ctl launch ran on")
		model.installInsteadOfLaunch()
		check(model.launchPrompt == nil)
		checkEqual(model.installState, .installing)
		check(waitUntil { model.installState != .installing }, "install did not finish")
		guard case .installed(let report) = model.installState else {
			check(false, "install did not report: \(String(describing: model.installState))")
			return
		}
		check(!report.optionSet)
		checkEqual(setup.standIn.runs.map { $0.first ?? "" }, ["launch", "install"])
		model.closeInstall()
		check(!model.isAsking)
		checkEqual(setup.prompts.errors.count, 1)
		guard makeSaves(setup, SAVE_SAMPLES_LAUNCHER) else {
			return
		}
		model.requestLaunch()
		guard let set = model.launchPrompt else {
			check(false, "the sheet did not open")
			return
		}
		check(set.saves.optionSet && !set.offersInstall)
		model.installInsteadOfLaunch()
		check(model.launchPrompt === set && model.installState == nil)
		model.cancelLaunch()
		checkEqual(setup.standIn.runs.count, 2)
	}

	static func offersLaunch() {
		// No harness serving, and Steam's launch option not set: the error box says to set it
		// first, and offers Install Harness…, which opens that sheet, as Launch FM24… would start
		// the game directly.
		guard let setup = HarnessSetup(serving: false) else {
			return
		}
		let model = setup.model
		model.open(setup.scratch.file("plain.json", plainSampleJSON()))
		wait(model)
		setup.standIn.set([
			"STATUS": "1", "STDOUT": "",
			"STDERR": "Football Manager 2024 is not running.\n",
		])
		model.setSeeds("1")
		model.simulate()
		check(waitUntil { !model.simulation.running }, "the simulation did not finish")
		let message =
			"FM24 isn't running with harness \(FixtureHandle.protocolVersion).\nFootball Manager 2024 is "
			+ "not running."
		checkEqual(setup.prompts.launchOffers.count, 1)
		checkEqual(
			setup.prompts.launchOffers.last?.message,
			"\(message)\n\n\(ViewerModel.setOptionFirst)"
		)
		checkEqual(setup.prompts.launchOffers.last?.title, ViewerModel.errorTitle)
		checkEqual(setup.prompts.launchOffers.last?.action, ViewerModel.installAction)
		check(!setup.monitor.optionSet)
		check(model.launchPrompt == nil && model.installState == nil)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		setup.prompts.launchAnswers = [true]
		model.simulate()
		check(waitUntil { !model.simulation.running }, "the simulation did not finish")
		checkEqual(setup.prompts.launchOffers.count, 2)
		check(model.launchPrompt == nil)
		check(model.installState != nil, "Install Harness… did not open the sheet")
		check(waitUntil { model.installState != .installing }, "install did not finish")
		model.closeInstall()
		// Once Steam has saved the option, an install that says so updates the start panel at once:
		// Launch FM24… in place of Install Harness….
		guard makeSaves(setup, SAVE_SAMPLES_LAUNCHER) else {
			return
		}
		check(!setup.monitor.optionSet, "the start panel read Steam's option before install")
		setup.standIn.set(["OPTION_SET": "1"])
		model.requestInstall()
		check(waitUntil { model.installState != .installing }, "install did not finish")
		check(setup.monitor.optionSet, "install did not update the start panel")
		checkEqual(setup.monitor.description, GameState.notRunning.description(optionSet: true))
		model.closeInstall()
		// With the option set, Launch FM24…, which opens the sheet.
		setup.standIn.set([
			"STATUS": "1", "STDOUT": "",
			"STDERR": "Football Manager 2024 is not running.\n",
		])
		model.simulate()
		check(waitUntil { !model.simulation.running }, "the simulation did not finish")
		checkEqual(setup.prompts.launchOffers.count, 3)
		checkEqual(setup.prompts.launchOffers.last?.message, message)
		checkEqual(setup.prompts.launchOffers.last?.action, ViewerModel.launchAction)
		check(setup.monitor.optionSet)
		check(model.launchPrompt == nil)
		setup.prompts.launchAnswers = [true]
		model.simulate()
		check(waitUntil { !model.simulation.running }, "the simulation did not finish")
		checkEqual(setup.prompts.launchOffers.count, 4)
		check(model.launchPrompt != nil, "Launch FM24… did not open the sheet")
		check(model.launchPrompt?.saves.optionSet == true)
		model.cancelLaunch()
		// With a harness serving, a plain error box.
		guard let serving = HarnessSetup(serving: true) else {
			return
		}
		serving.model.open(serving.scratch.file("plain.json", plainSampleJSON()))
		wait(serving.model)
		serving.standIn.set(["STATUS": "1", "STDOUT": "", "STDERR": "cannot read the input\n"])
		serving.model.simulate()
		check(waitUntil { !serving.model.simulation.running }, "the simulation did not finish")
		checkEqual(serving.prompts.launchOffers.count, 0)
		checkEqual(serving.prompts.lastError, "cannot read the input")
	}
}
