// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import SwiftUI

/// A developer check of the layout without showing a window:
///   fm24viewer --render-snapshot <fixture.json|-> <squads|venue|simulation|team|swap|start|log|
///       launch|install> <out.png> [light|dark] [<width>x<height>] [edited] [temporary] [busy]
///       [failed] [simulating] [simulated] [replacing] [filtered] [unclear]
/// lays the window's content out offscreen, renders it to a PNG and exits ("-": no fixture).
/// `team` is the home team's editor, as its popover shows it, and `swap` the swap prompt for the
/// first away player, as its sheet shows it, `launch` the Launch FM24 sheet over a few made-up
/// saves and `install` the Install Harness sheet after a made-up install, each at its own size
/// unless one is given. On `launch`, `busy` shows a launch in progress, `failed` one that FM24's
/// other save stopped and `unclear` the sheet when Steam's launch option sets FM24's data folder in
/// a way fm24ctl does not follow; on `install`, `failed` shows a failed install.
/// `edited` first selects the first three players of each side that can be selected, maximises
/// their happiness and condition where they can be, and changes the capacity if it is stored.
/// `temporary` shows the fixture as an export not yet saved. `busy` shows a harness job running
/// (the startup export, or over a fixture a decode), and `failed` the start panel after the
/// startup export failed with FM24 not running. `simulating` shows a simulation of seeds 1-100
/// with 37 results in, and `simulated` one that has finished, with its results in a folder of the
/// snapshot's own. `replacing` shows the away team being replaced. On `team`, `filtered` types "u"
/// and `failed` shows the game's refusal; on `swap`, `filtered` types "ode". The team and player
/// lists are a few made-up names. None runs fm24ctl or looks at the game, and none writes beside
/// the fixture or into the viewer's settings.
@MainActor
enum Snapshot {
	static let argument = "--render-snapshot"
	static let options: Set<String> = [
		"edited", "temporary", "busy", "failed", "simulating", "simulated", "replacing", "filtered",
		"unclear",
	]
	private static let usage =
		"usage: fm24viewer \(argument) <fixture.json|-> <squads|venue|simulation|team|swap|start|log|"
		+ "launch|install> <out.png> [light|dark] [<width>x<height>] [edited] [temporary] [busy]"
		+ " [failed] [simulating] [simulated] [replacing] [filtered] [unclear]"

	static func run(_ arguments: [String]) -> Int32 {
		guard arguments.count >= 3, arguments.count <= 14 else {
			return fail(usage)
		}
		var size = NSSize(width: 1480, height: 980)
		var sized = false
		var appearance = NSAppearance(named: .aqua)
		var chosen: Set<String> = []
		for option in arguments.dropFirst(3) {
			if options.contains(option) {
				chosen.insert(option)
			} else if option == "dark" {
				appearance = NSAppearance(named: .darkAqua)
			} else if option != "light" {
				let parts = option.split(separator: "x").compactMap { Double($0) }
				guard parts.count == 2, parts[0] >= 100, parts[1] >= 100 else {
					return fail("unknown option \(option)")
				}
				size = NSSize(width: parts[0], height: parts[1])
				sized = true
			}
		}
		let app = NSApplication.shared
		app.setActivationPolicy(.prohibited)
		app.appearance = appearance
		let scratch = FileManager.default.temporaryDirectory
			.appendingPathComponent("fm24viewer-snapshot-\(getpid())", isDirectory: true)
		defer {
			try? FileManager.default.removeItem(at: scratch)
		}
		let log = ViewerLog(echo: false)
		let monitor = GameMonitor(
			isRunning: { false },
			isServing: { false },
			launchOptionSet: { true }
		)
		let simulates = chosen.contains("simulating") || chosen.contains("simulated")
		let harness = SnapshotHarness(
			finishes: chosen.contains("failed") || chosen.contains("simulated")
		)
		let model = ViewerModel(
			log: log,
			harness: harness,
			work: WorkFiles(directory: scratch),
			settings: ViewerSettings(defaults: SnapshotSettings(), documents: scratch),
			monitor: monitor
		)
		model.beginActivity = { _ in {} }
		model.progressInterval = 0.02
		let view: NSView
		switch arguments[1] {
		case "squads", "venue", "simulation", "team", "swap":
			guard arguments[0] != "-" else {
				return fail("\(arguments[1]) needs a fixture")
			}
			do {
				var fixture = URL(fileURLWithPath: arguments[0])
				// A simulation's results go beside the fixture: here, beside a copy.
				if simulates {
					try FileManager.default.createDirectory(at: scratch, withIntermediateDirectories: true)
					let copy = scratch.appendingPathComponent(fixture.lastPathComponent)
					try FileManager.default.copyItem(at: fixture, to: copy)
					fixture = copy
				}
				let (source, handle) = try FixtureFiles.load(
					fixture,
					temporary: chosen.contains("temporary")
				)
				model.show(source, handle)
				if chosen.contains("edited") {
					edit(model, handle)
				}
				if chosen.contains("busy") {
					let binary = scratch.appendingPathComponent("next.bin")
					try FileManager.default.createDirectory(at: scratch, withIntermediateDirectories: true)
					try Data([0]).write(to: binary)
					model.open(binary)
				}
				if simulates {
					model.setSeeds("1-100")
					model.simulate()
				}
				if chosen.contains("replacing") {
					model.editTeam(side: 1)
					model.teamEditor?.type("677")
					model.confirmTeam()
				}
			} catch {
				return fail((error as? ViewerError)?.message ?? "\(error)")
			}
			let tabs: [String: ViewerModel.Tab] = ["venue": .venue, "simulation": .simulation]
			model.tab = tabs[arguments[1]] ?? .squads
			switch arguments[1] {
			case "team":
				guard let editor = team(model, chosen) else {
					return fail("the team editor cannot open")
				}
				view = NSHostingView(
					rootView: TeamPickerView(model: model, editor: editor, teams: model.teams)
						.background(Color(nsColor: .windowBackgroundColor))
				)
			case "swap":
				guard let prompt = swap(model, chosen) else {
					return fail("the swap prompt cannot open")
				}
				view = NSHostingView(
					rootView: SwapSheet(model: model, prompt: prompt, players: model.players)
						.background(Color(nsColor: .windowBackgroundColor))
				)
			default:
				view = NSHostingView(rootView: ViewerView(model: model))
			}
			if ["team", "swap"].contains(arguments[1]), !sized {
				size = view.fittingSize
			}
		case "start":
			if chosen.contains("busy") || chosen.contains("failed") {
				model.exportNextFixture()
			}
			view = NSHostingView(rootView: ViewerView(model: model))
		case "log":
			log.add("Opening /Users/example/next-fixture.json")
			log.error("cannot read fixture")
			view = NSHostingView(rootView: LogView(log: log))
		case "launch":
			guard let prompt = launch(model, chosen, scratch: scratch) else {
				return fail("the Launch FM24 sheet cannot open")
			}
			view = NSHostingView(
				rootView: LaunchSheet(model: model, prompt: prompt)
					.background(Color(nsColor: .windowBackgroundColor))
			)
		case "install":
			model.requestInstall()
			turn { model.installState != .installing }
			view = NSHostingView(
				rootView: InstallSheet(model: model)
					.background(Color(nsColor: .windowBackgroundColor))
			)
		default:
			return fail("unknown view \(arguments[1])")
		}
		if ["launch", "install"].contains(arguments[1]), !sized {
			size = view.fittingSize
		}
		let window = NSWindow(
			contentRect: NSRect(origin: .zero, size: size),
			styleMask: [.titled, .resizable],
			backing: .buffered,
			defer: false
		)
		window.appearance = appearance
		window.contentView = view
		window.setContentSize(size)
		// SwiftUI and the tables lay out, and draw their layers, over a few turns of the run loop;
		// a simulation's results arrive, or it finishes, within a few more.
		let deadline = Date().addingTimeInterval(5)
		var turns = 0
		while turns < 10
			|| (chosen.contains("simulating") && !model.simulation.started && Date() < deadline)
			|| (chosen.contains("simulated") && model.simulation.running && Date() < deadline)
		{
			view.layoutSubtreeIfNeeded()
			view.display()
			RunLoop.main.run(until: Date().addingTimeInterval(0.05))
			turns += 1
		}
		guard let image = view.bitmapImageRepForCachingDisplay(in: view.bounds) else {
			return fail("cannot render the view")
		}
		view.cacheDisplay(in: view.bounds, to: image)
		// Translucent controls, such as the tab bar, show the window's background through them: put
		// it behind them, as the window does, or they come out transparent.
		if let context = NSGraphicsContext(bitmapImageRep: image) {
			NSGraphicsContext.saveGraphicsState()
			NSGraphicsContext.current = context
			(appearance ?? NSAppearance.currentDrawing()).performAsCurrentDrawingAppearance {
				NSColor.windowBackgroundColor.setFill()
				NSRect(origin: .zero, size: image.size).fill(using: .destinationOver)
			}
			NSGraphicsContext.restoreGraphicsState()
		}
		guard let png = image.representation(using: .png, properties: [:]) else {
			return fail("cannot encode the PNG")
		}
		do {
			try png.write(to: URL(fileURLWithPath: arguments[2]))
		} catch {
			return fail("cannot write \(arguments[2]): \(error.localizedDescription)")
		}
		let rendered = "\(Int(view.bounds.width))x\(Int(view.bounds.height))"
		print("rendered \(arguments[1]) at \(rendered) to \(arguments[2])")
		return 0
	}

	/// Opens the home team's editor once the team list has loaded, with "u" typed if `filtered`,
	/// or the game's refusal if `failed`.
	private static func team(_ model: ViewerModel, _ chosen: Set<String>) -> TeamEditor? {
		model.editTeam(side: 0)
		turn { model.teams.state != .loading }
		guard let editor = model.teamEditor else {
			return nil
		}
		if chosen.contains("filtered") {
			editor.type("u")
		}
		if chosen.contains("failed") {
			editor.error = "error: that is the away team"
		}
		return editor
	}

	/// Opens the swap prompt for the first away player once the player list has loaded, with "ode"
	/// typed if `filtered`.
	private static func swap(_ model: ViewerModel, _ chosen: Set<String>) -> SwapPrompt? {
		guard let fixture = model.fixture,
			let block = (0..<fixture.rowCount(side: 1)).lazy.compactMap({
				fixture.rowBlock(side: 1, row: $0)
			}).first
		else {
			return nil
		}
		model.select(side: 1, blocks: [block])
		model.requestSwap(side: 1)
		turn { model.players.state != .loading }
		guard let prompt = model.swapPrompt else {
			return nil
		}
		if chosen.contains("filtered") {
			prompt.type("ode")
			turn { !prompt.filterPending }
		}
		return prompt
	}

	/// Opens the Launch FM24 sheet over a few made-up saves in a home folder of the snapshot's own,
	/// with a launch running if `busy`, or one that failed if `failed`; or, if `unclear`, as it is
	/// when Steam's launch option sets the data folder in a way fm24ctl does not follow.
	private static func launch(
		_ model: ViewerModel,
		_ chosen: Set<String>,
		scratch: URL
	) -> LaunchPrompt? {
		let home = scratch.appendingPathComponent("home", isDirectory: true)
		if chosen.contains("unclear") {
			return LaunchPrompt(
				// Only Steam's launch option for the launcher gives the game a data folder.
				saves: LaunchSaves(
					data: .unclear,
					folder: nil,
					saves: [],
					home: home.path,
					optionSet: true
				),
				offersRosetta: true,
				remembered: nil,
				rosetta: false
			)
		}
		let data = home.appendingPathComponent(
			"Library/Application Support/Sports Interactive/Football Manager 2024",
			isDirectory: true
		)
		let saves: [(String, TimeInterval)] = [
			("cloud/games/LIV_MAIN_FM24.fm", 0), ("games/Sandbox Copy.fm", 3 * 86_400),
			("cloud/games/Luton Rebuild.fm", 9 * 86_400),
		]
		for (file, age) in saves {
			let url = data.appendingPathComponent(file)
			try? FileManager.default.createDirectory(
				at: url.deletingLastPathComponent(),
				withIntermediateDirectories: true
			)
			try? Data("fmf.".utf8).write(to: url)
			let date = Date(timeIntervalSince1970: 1_792_000_000 - age)
			try? FileManager.default.setAttributes([.modificationDate: date], ofItemAtPath: url.path)
		}
		model.home = home.path
		model.appleSilicon = true
		model.requestLaunch()
		guard let prompt = model.launchPrompt else {
			return nil
		}
		if chosen.contains("busy") || chosen.contains("failed") {
			model.confirmLaunch()
			turn { chosen.contains("busy") ? prompt.lines.count >= 4 : !prompt.isLaunching }
		}
		return prompt
	}

	/// Runs the main run loop until `done`, for up to two seconds.
	private static func turn(until done: () -> Bool) {
		let deadline = Date().addingTimeInterval(2)
		while !done() && Date() < deadline {
			RunLoop.main.run(until: Date().addingTimeInterval(0.02))
		}
	}

	/// Edits as a user might, to show the edit markers and edited cells.
	private static func edit(_ model: ViewerModel, _ fixture: FixtureHandle) {
		for side in 0..<FixtureHandle.sideCount {
			let blocks = (0..<fixture.rowCount(side: side)).compactMap {
				fixture.rowBlock(side: side, row: $0)
			}
			model.select(side: side, blocks: Set(blocks.prefix(3)))
			for edit in [PlayerEdit.happiness, .condition]
			where fixture.editProblem(edit, side: side, blocks: model.selection[side]) == nil {
				model.maxPlayers(edit, side: side)
			}
		}
		let capacity = 4
		model.setVenueText("50000", field: capacity)
	}

	private static func fail(_ message: String) -> Int32 {
		FileHandle.standardError.write(Data("fm24viewer: \(message)\n".utf8))
		return 2
	}
}

/// A harness for snapshots, which never runs fm24ctl: its jobs never finish, or fail at once as
/// fm24ctl does when FM24 is not running.
@MainActor
private final class SnapshotHarness: HarnessService {
	private let finishes: Bool
	/// The jobs that never finish.
	private var waiting: [CheckedContinuation<Void, Never>] = []

	init(finishes: Bool) {
		self.finishes = finishes
	}

	func exportNextFixture(
		to output: URL,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		await job()
	}

	func decode(
		_ input: URL,
		to output: URL,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		await job()
	}

	/// Writes 37 results of seeds 1-100 and never finishes, or all of them and succeeds.
	func simulate(
		_ input: URL,
		options: SimulationOptions,
		output: URL,
		anySave: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		var csv = "seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n"
		for seed in 1...(finishes ? 100 : 37) {
			csv += "\(seed),\(seed % 3),1,23000,1,0000000000000000,4000.0\n"
		}
		try? Data(csv.utf8).write(to: output)
		guard finishes else {
			return await job()
		}
		return HarnessResult(status: 0, output: ["100 runs"])
	}

	func replaceTeam(
		side: Int,
		input: URL,
		team: String,
		output: URL,
		anySave: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		await job()
	}

	func injectPlayer(
		input: URL,
		block: String,
		person: String,
		output: URL,
		allowDuplicate: Bool,
		anySave: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		await job()
	}

	/// A few made-up teams, as list-teams writes them.
	func listTeams(to output: URL, onLine: @escaping (HarnessLine) -> Void) async -> HarnessResult {
		let teams = [
			"677\tLuton Town", "12\tAston Villa", "99\tUnited", "691\tNorwich City", "98\tUnited",
			"1\tAFC Utd", "2\tDundee United", "3\tFC United of Manchester",
		]
		return list(teams, output)
	}

	/// A few made-up players, as list-players writes them.
	func listPlayers(to output: URL, onLine: @escaping (HarnessLine) -> Void) async -> HarnessResult {
		let players = [
			"40347\tMartin\tØdegaard\t\tArsenal", "1001\tEdson\tArantes do Nascimento\tPelé\t",
			"2002\tThomas\tMüller\t\tFC Bayern", "5005\tJan\tBodegaard\t\t",
			"6006\tOdette\tLang\t\tExample Town",
		]
		return list(players, output)
	}

	private func list(_ lines: [String], _ output: URL) -> HarnessResult {
		let written = (try? Data((lines.joined(separator: "\n") + "\n").utf8).write(to: output)) != nil
		return HarnessResult(status: written ? 0 : 1)
	}

	/// fm24ctl's first lines as it starts FM24 through Steam, then a wait that never ends; or, if
	/// it finishes, its refusal with another save loaded.
	func launch(
		save: String?,
		rosetta: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		let name = save.map { ($0 as NSString).lastPathComponent }.map {
			$0.hasSuffix(".fm") ? String($0.dropLast(3)) : $0
		}
		guard !finishes else {
			let refusal =
				"FM24 has 'Other Career' loaded, not '\(name ?? "")'; fm24ctl will not switch saves in a "
				+ "running game (unsaved progress). Quit FM24 (saving first only if you loaded that "
				+ "save yourself and have done no harness work since), then run this again."
			return HarnessResult(status: HarnessResult.wrongState, errors: [refusal])
		}
		let lines = [
			"loading \(name ?? ""): \(save ?? "")",
			"starting Football Manager 2024 through Steam (its launch option loads the harness)",
			"waiting for the game to start the harness...",
			"still waiting (60 s)...",
		]
		for line in lines {
			onLine(.output(line))
		}
		return await job()
	}

	/// A made-up install, as fm24ctl reports it.
	func install(onLine: @escaping (HarnessLine) -> Void) async -> HarnessResult {
		let folder = "/Users/you/Library/Application Support/fm24-harness"
		guard !finishes else {
			return HarnessResult(status: 1, errors: ["cannot write \(folder)/launch-fm24.sh"])
		}
		let version = FixtureHandle.protocolVersion
		let output = [
			"installed harness \(version): \(folder)/fm24-harness-\(version).dylib",
			"launcher: \(folder)/launch-fm24.sh", "log: \(folder)/harness.log",
			"Football Manager 2024: /Users/you/Library/Application Support/Steam/steamapps/common/"
				+ "Football Manager 2024/fm.app",
			"", "To start Football Manager 2024 from Steam with the harness, set its launch option once:",
			"  Steam > Library > Football Manager 2024 > Properties > General > Launch Options:",
			"  /bin/sh \"\(folder)/launch-fm24.sh\" %command%",
			"Keep any arguments of your own after %command%. Steam may save the option only when it "
				+ "quits: run `fm24ctl install` again until it says that Steam's launch option uses "
				+ "this launcher, quitting Steam if it does not. Until then, `fm24ctl launch` starts "
				+ "the game without the launch option, which has not been verified: the game crashed "
				+ "in its one attempt.", "",
			"Run `fm24ctl install` again after updating fm24ctl: it updates the harness and the "
				+ "launcher.",
		]
		return HarnessResult(status: 0, output: output)
	}

	func isServing() -> Bool {
		false
	}

	private func job() async -> HarnessResult {
		guard finishes else {
			// The snapshot is taken, and the process ends, while this waits.
			await withCheckedContinuation { waiting.append($0) }
			return HarnessResult(status: 1)
		}
		let explanation = [
			"Football Manager 2024 is not running.",
			"Set Steam's launch option for it once, then start it from Steam (`fm24ctl install` says "
				+ "how, and when Steam has saved the option):",
		]
		return HarnessResult(status: 1, errors: explanation)
	}
}

/// Settings for snapshots, kept in memory: a snapshot never changes the viewer's own.
private final class SnapshotSettings: SettingsStore {
	private var values: [String: Any] = [:]

	func string(forKey key: String) -> String? {
		values[key] as? String
	}

	func bool(forKey key: String) -> Bool {
		values[key] as? Bool ?? false
	}

	func set(_ value: Any?, forKey key: String) {
		values[key] = value
	}
}
