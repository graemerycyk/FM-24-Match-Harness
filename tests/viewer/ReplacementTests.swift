// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import FixtureSamples
import Foundation

/// Team replacement and player swaps through fm24ctl's stand-in: the team editor and swap prompt,
/// the lists they search, the edits and venue values a replacement carries, its errors, and what
/// waits while it runs, with the Windows viewer's rules and messages and the game kept to the
/// temporary directory.
@MainActor
enum ReplacementTests {
	static func register() {
		Tests.test("viewer replaces a team through the game", replacesTeams)
		Tests.test("the team editor follows the Windows viewer's rules", editsTeams)
		Tests.test("a replacement carries the venue and converts binary fixtures", carriesVenue)
		Tests.test("viewer swaps a player through the game", swapsPlayers)
		Tests.test("the swap prompt searches player names after a pause", searchesSwaps)
		Tests.test("the team and player lists are dropped when the game quits", dropsLists)
		Tests.test("viewer warns before saving replaced teams as binary", warnsBinaryTeams)
		Tests.test("a swap after a line-up change gives the player's new place", swapsAfterLineup)
		Tests.test("a player already in the side's squad is not swapped in", refusesSquadSwaps)
		Tests.test("a team replacement drops the replaced side's new line-up", dropsReplacedLineup)
		Tests.test("a team replacement keeps the other side's changed line-up", keepsLineup)
		Tests.test("a home replacement keeps the away side's changed line-up", keepsAwayLineup)
		Tests.test("an away swap after a line-up change gives the new place", swapsAfterAwayLineup)
	}

	private static let happiness = UInt8(FIXTURE_VIEW_MAX_HAPPINESS.rawValue)

	private static let homeEdit = 0
	private static let awayEdits = [2, 3]
	private static let luton = "Luton Town"

	/// The plain sample with Bristol City replaced: what the stand-in writes back.
	private static func replacedJSON() -> Data {
		replacing(plainSampleJSON(), "Bristol City", with: luton)
	}

	private static func open(_ setup: HarnessSetup, _ url: URL) {
		setup.model.open(url)
		wait(setup.model)
	}

	private static func wait(_ model: ViewerModel) {
		check(waitUntil { !model.isBusy }, "the operation did not finish")
	}

	private static func workFiles(_ setup: HarnessSetup) -> [String] {
		setup.scratch.names("T/" + setup.model.work.folder.lastPathComponent)
	}

	/// Opens the plain sample and maximises the happiness of home block 0 and away blocks 2 and 3.
	private static func openEdited(_ setup: HarnessSetup) -> URL {
		let url = setup.scratch.file("next.json", plainSampleJSON())
		open(setup, url)
		let model = setup.model
		model.select(side: 0, blocks: [homeEdit])
		model.maxPlayers(.happiness, side: 0)
		model.select(side: 1, blocks: Set(awayEdits))
		model.maxPlayers(.happiness, side: 1)
		check(model.fixture?.hasPlayerEdits == true, "the edits did not apply")
		return url
	}

	/// The edit flags with happiness maximised in these blocks.
	private static func flags(home: [Int], away: [Int]) -> [UInt8] {
		var flags = [UInt8](repeating: 0, count: 2 * FixtureHandle.blockCount)
		for block in home {
			flags[block] = UInt8(FIXTURE_VIEW_MAX_HAPPINESS.rawValue)
		}
		for block in away {
			flags[FixtureHandle.blockCount + block] = UInt8(FIXTURE_VIEW_MAX_HAPPINESS.rawValue)
		}
		return flags
	}

	/// The team list, once it has loaded.
	private static func loadTeams(_ model: ViewerModel) {
		check(waitUntil { model.teams.state != .loading }, "the team list did not load")
	}

	static func replacesTeams() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let url = openEdited(setup)
		let folder = model.work.folder.path
		model.tab = .venue
		check(model.canEditTeam(side: 1))
		model.editTeam(side: 1)
		// The editor starts with the team's name, listed by it, on the Squads tab.
		checkEqual(model.tab, .squads)
		guard let editor = model.teamEditor else {
			check(false, "no team editor")
			return
		}
		checkEqual(editor.side, 1)
		checkEqual(editor.title, "Replace away team")
		checkEqual(editor.text, "Bristol City")
		checkEqual(editor.matches, [])
		checkEqual(model.teams.state, .loading)
		loadTeams(model)
		checkEqual(model.teams.state, .loaded)
		checkEqual(editor.matches, [])
		checkEqual(setup.standIn.runs, [["list-teams", folder + "/list-1.txt"]])
		editor.type("U")
		checkEqual(editor.matches, ["United (98)", "United (99)", "Luton Town (677)"])
		// Opening the other side's editor closes this one; the list is not fetched again.
		model.editTeam(side: 0)
		checkEqual(model.teamEditor?.side, 0)
		checkEqual(model.teamEditor?.text, "Aston Villa")
		checkEqual(model.teamEditor?.matches, ["Aston Villa (12)"])
		model.editTeam(side: 1)
		checkEqual(setup.standIn.runs.count, 1)
		guard let editor = model.teamEditor else {
			return
		}

		// The game refuses: the error shows in red in the editor, and nothing changes.
		let input = setup.scratch.file("input.json")
		setup.standIn.set([
			"STATUS": "1", "STDOUT": "error: that is the home team\n", "INPUT": input.path,
		])
		editor.type("Aston Villa")
		model.confirmTeam()
		wait(model)
		checkEqual(editor.error, "error: that is the home team")
		check(model.teamEditor === editor)
		checkEqual(model.source?.url, url)
		check(model.fixture?.hasPlayerEdits == true)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(workFiles(setup), [])

		// A label picked from the list gives its ID; the input carries the home side's edits only.
		setup.standIn.set([
			"FIXTURE": setup.scratch.file("replaced.json", replacedJSON()).path,
			"INPUT": input.path,
			"DELAY_MS": "300",
		])
		editor.pick("Luton Town (677)")
		checkEqual(editor.matches, ["Aston Villa (12)"])
		model.confirmTeam()
		checkEqual(model.busy, .replacement)
		checkEqual(model.activity, "Replacing the away team with FM24…")
		checkEqual(editor.error, nil)
		wait(model)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(
			setup.standIn.runs.last,
			["replace-away", folder + "/in-4.json", "677", folder + "/out-5.json"]
		)
		setup.checkTemporaryOnly()
		checkEqual(
			try Data(contentsOf: input),
			try FixtureHandle.write(
				json: plainSampleJSON(),
				changes: PlayerChanges(flags: flags(home: [homeEdit], away: [])),
				venue: nil,
				binary: false
			)
		)
		// The result is a fixture not yet saved, with no edits or selection; the pencil has the
		// focus, and the work files are gone.
		check(model.isTemporary)
		checkEqual(model.source?.json, replacedJSON())
		checkEqual(model.source?.kind, .json)
		checkEqual(model.fixture?.teamName(side: 1), luton)
		checkEqual(model.fixture?.hasPlayerEdits, false)
		checkEqual(model.selection, [[], []])
		check(model.teamEditor == nil)
		checkEqual(model.squadFocus?.side, 1)
		checkEqual(model.squadFocus?.control, .pencil)
		checkEqual(model.activity, nil)
		checkEqual(workFiles(setup), [])
		checkEqual(try Data(contentsOf: url), plainSampleJSON())
		// An export not yet saved closes without asking, and opening another asks first.
		checkEqual(model.closeCheck(), .close)
		model.requestOpen(url)
		checkEqual(setup.prompts.questions.last?.message, ViewerModel.openDiscardMessage)
		// Typed text goes as it is.
		model.editTeam(side: 0)
		model.teamEditor?.type("Norwich City")
		model.confirmTeam()
		wait(model)
		checkEqual(
			setup.standIn.runs.last,
			["replace-home", folder + "/in-6.json", "Norwich City", folder + "/out-7.json"]
		)
		checkEqual(setup.standIn.runs.filter { $0.first == "list-teams" }.count, 1)
	}

	static func editsTeams() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		check(!model.canEditTeam(side: 0), "no fixture: no editor")
		model.editTeam(side: 0)
		check(model.teamEditor == nil)
		// A team list that fails to load is fetched again by the next edit; free text still works.
		setup.standIn.set(["STATUS": "1", "STDOUT": "error: FM24 is not running\n"])
		open(setup, setup.scratch.file("next.json", plainSampleJSON()))
		model.editTeam(side: 0)
		loadTeams(model)
		checkEqual(model.teams.state, .unloaded)
		checkEqual(model.teamEditor?.matches, [])
		check(model.log.text.contains("cannot list teams: error: FM24 is not running"), model.log.text)
		setup.standIn.set([:])
		model.editTeam(side: 0)
		loadTeams(model)
		checkEqual(model.teams.state, .loaded)
		checkEqual(setup.standIn.runs.count, 2)
		guard let editor = model.teamEditor else {
			check(false, "no team editor")
			return
		}
		// Text the game cannot take is refused before it is asked.
		let long = String(repeating: "é", count: 128)
		for text in ["", " \t", "1\nteam=2", "a\rb", long] {
			editor.type(text)
			editor.error = nil
			model.confirmTeam()
			checkEqual(editor.error, TeamEditor.problem)
			check(!model.isBusy)
		}
		checkEqual(setup.standIn.runs.count, 2)
		// The field holds 255 characters, as the Windows one.
		editor.type(String(repeating: "x", count: 300))
		checkEqual(editor.text.count, 255)
		check(editor.team != nil)
		// An invalid venue entry is refused first, on the Venue tab.
		let venue = setup.scratch.file("venue.json", sampleJSON(FIXTURE_SAMPLE_VENUE))
		open(setup, venue)
		check(model.teamEditor == nil, "a new fixture closes the editor")
		model.editTeam(side: 0)
		model.setVenueText("x", field: 4)
		model.confirmTeam()
		checkEqual(model.tab, .venue)
		checkEqual(setup.prompts.lastError, "Capacity: Enter a valid whole number.")
		check(!model.isBusy)
		checkEqual(setup.standIn.runs.count, 2)
		model.setVenueText("42777", field: 4)
		open(setup, setup.scratch.file("plain.json", plainSampleJSON()))
		model.editTeam(side: 0)

		// During a simulation the pencil and Confirm wait; the editor's Cancel does not.
		setup.standIn.set(["ROW_MS": "100"])
		model.setSeeds("1-5")
		model.simulate()
		check(model.simulation.running, "the simulation did not start")
		check(!model.canEditTeam(side: 1))
		check(model.canCancelTeamEdit)
		model.confirmTeam()
		checkEqual(model.replacement, nil)
		model.editTeam(side: 1)
		checkEqual(model.teamEditor?.side, 0)
		model.cancelTeamEdit()
		check(model.teamEditor == nil)
		check(waitUntil { !model.simulation.running }, "the simulation did not finish")

		// While a replacement runs, everything else waits, including the editor's Cancel.
		setup.standIn.set([
			"FIXTURE": setup.scratch.file("replaced.json", replacedJSON()).path, "DELAY_MS": "500",
		])
		model.editTeam(side: 1)
		model.teamEditor?.type("677")
		model.confirmTeam()
		checkEqual(model.busy, .replacement)
		check(!model.canCancelTeamEdit)
		model.cancelTeamEdit()
		check(model.teamEditor != nil)
		check(!model.canEditTeam(side: 0) && !model.canSwap(side: 0))
		check(!model.canEditPlayers(side: 0) && !model.venueEditable(4) && !model.canSimulate)
		checkEqual(
			model.closeCheck(),
			.blocked("A team replacement is running. Please wait before closing.")
		)
		let errors = setup.prompts.errors.count
		model.simulate()
		checkEqual(setup.prompts.lastError, ViewerModel.busyMessage)
		model.requestOpen(venue)
		checkEqual(setup.prompts.errors.count, errors + 2)
		wait(model)
		check(model.isTemporary)
		// An export waits for a replacement, with the Windows message.
		model.editTeam(side: 0)
		model.teamEditor?.type("12")
		model.confirmTeam()
		check(!model.canExport)
		model.export()
		checkEqual(
			setup.prompts.lastError,
			"Please wait for the team replacement to finish before exporting."
		)
		wait(model)
		check(model.canExport)
		checkEqual(setup.scratch.names("Exports"), [])
	}

	static func carriesVenue() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let json = sampleJSON(FIXTURE_SAMPLE_VENUE)
		open(setup, setup.scratch.file("venue.json", json))
		let capacity = 4
		model.setVenueText("50000", field: capacity)
		let input = setup.scratch.file("input.json")
		let output = setup.scratch.file("replaced.json", json)
		setup.standIn.set(["FIXTURE": output.path, "INPUT": input.path])
		model.editTeam(side: 0)
		model.teamEditor?.type("Norwich City")
		model.confirmTeam()
		wait(model)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		// The game is given the fixture without the venue edit, which goes into its result.
		checkEqual(try Data(contentsOf: input), json)
		guard let fixture = model.fixture else {
			check(false, "no fixture")
			return
		}
		checkEqual(fixture.venue[capacity].value, 50000)
		var values = try FixtureHandle(json: json).venue
		values[capacity].value = 50000
		checkEqual(
			model.source?.json,
			try FixtureHandle.write(json: json, changes: nil, venue: values, binary: false)
		)
		check(!model.venue.dirty)
		checkEqual(model.venue.texts[capacity], "50000")

		// A binary fixture's decoded JSON is what the game is given.
		let plain = plainSampleJSON()
		let bytes = try FixtureHandle.write(json: plain, changes: nil, venue: nil, binary: true)
		setup.standIn.set(["FIXTURE": setup.scratch.file("decoded.json", plain).path])
		open(setup, setup.scratch.file("next.bin", bytes))
		checkEqual(model.source?.kind, .binary)
		setup.standIn.set([
			"FIXTURE": setup.scratch.file("replaced.json", replacedJSON()).path, "INPUT": input.path,
		])
		model.editTeam(side: 1)
		model.teamEditor?.type("677")
		model.confirmTeam()
		wait(model)
		checkEqual(try Data(contentsOf: input), plain)
		check(model.isTemporary)
		checkEqual(model.source?.kind, .json)
		checkEqual(model.fixture?.teamName(side: 1), luton)
		setup.checkTemporaryOnly()
		checkEqual(workFiles(setup), [])
	}

	/// Opens the swap prompt for the selected away player.
	private static func openPrompt(_ setup: HarnessSetup) -> SwapPrompt? {
		setup.model.requestSwap(side: 1)
		guard let prompt = setup.model.swapPrompt else {
			check(false, "no swap prompt")
			return nil
		}
		prompt.delay = 0.01
		return prompt
	}

	static func swapsPlayers() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let url = openEdited(setup)
		let folder = model.work.folder.path
		// Swap player needs exactly one selected player.
		check(!model.canSwap(side: 1), "two players are selected")
		model.requestSwap(side: 1)
		check(model.swapPrompt == nil)
		model.select(side: 1, blocks: [2])
		model.select(side: 0, blocks: [])
		check(model.canSwap(side: 1) && !model.canSwap(side: 0))
		guard let prompt = openPrompt(setup) else {
			return
		}
		checkEqual(prompt.label, "Swap Injected player for...")
		checkEqual(prompt.side, 1)
		checkEqual(prompt.block, 2)
		check(!prompt.allowDuplicates)
		checkEqual(prompt.cue, "Unique ID (loading player names…)")
		check(waitUntil { model.players.state == .loaded }, "the player list did not load")
		checkEqual(prompt.cue, "Player name or unique ID")
		checkEqual(setup.standIn.runs, [["list-players", folder + "/list-1.txt"]])

		// Cancel, or what is not a player, leaves the fixture as it is.
		model.cancelSwap()
		check(model.swapPrompt == nil)
		for text in ["", "0", "4294967296", "Martin Odegaard", "0000000000000001"] {
			guard let prompt = openPrompt(setup) else {
				return
			}
			prompt.type(text)
			model.confirmSwap()
			check(model.swapPrompt === prompt, "the prompt stays open for \(text)")
			checkEqual(setup.prompts.errors.last?.title, "Swap player")
			checkEqual(setup.prompts.lastError, SwapPrompt.problem)
		}
		checkEqual(setup.standIn.runs.count, 1)

		// An unknown unique ID: the game's error, and the selection stays.
		let input = setup.scratch.file("input.json")
		setup.standIn.set([
			"STATUS": "1", "STDOUT": "error: no person has unique ID 2000040347\n", "INPUT": input.path,
		])
		model.swapPrompt?.type("2000040347")
		model.confirmSwap()
		check(model.swapPrompt == nil)
		checkEqual(model.busy, .replacement)
		checkEqual(model.activity, ViewerModel.swappingActivity)
		wait(model)
		checkEqual(setup.prompts.errors.last?.title, ViewerModel.errorTitle)
		checkEqual(setup.prompts.lastError, "error: no person has unique ID 2000040347")
		check(model.canSwap(side: 1))
		checkEqual(model.source?.url, url)
		checkEqual(
			setup.standIn.runs.last,
			[
				"inject-player", folder + "/in-2.json", "away.player[2]", "2000040347",
				folder + "/out-3.json",
			]
		)

		// A player already in the fixture is refused until "Allow duplicates?" is ticked; the
		// refusal points at the checkbox. The label picked gives the ID.
		setup.standIn.set([
			"DUPLICATE": "1", "INPUT": input.path,
			"FIXTURE": setup.scratch.file("swapped.json", replacedJSON()).path,
		])
		guard let duplicate = openPrompt(setup) else {
			return
		}
		duplicate.pick(ChoiceTests.odegaard)
		model.confirmSwap()
		wait(model)
		checkEqual(
			setup.prompts.lastError,
			"error: away.player[2].person_uid (Stand-in) is already this player; tick \"Allow "
				+ "duplicates?\" to swap them in anyway"
		)
		checkEqual(
			setup.standIn.runs.last,
			["inject-player", folder + "/in-4.json", "away.player[2]", "40347", folder + "/out-5.json"]
		)
		guard let allowed = openPrompt(setup) else {
			return
		}
		allowed.pick(ChoiceTests.odegaard)
		allowed.allowDuplicates = true
		let errors = setup.prompts.errors.count
		model.confirmSwap()
		wait(model)
		checkEqual(setup.prompts.errors.count, errors)
		checkEqual(
			setup.standIn.runs.last,
			[
				"inject-player", folder + "/in-6.json", "away.player[2]", "40347", folder + "/out-7.json",
				"--allow-duplicate",
			]
		)
		// The other edits are carried over; the swapped-out player's are dropped.
		checkEqual(
			try Data(contentsOf: input),
			try FixtureHandle.write(
				json: plainSampleJSON(),
				changes: PlayerChanges(flags: flags(home: [homeEdit], away: [3])),
				venue: nil,
				binary: false
			)
		)
		check(model.isTemporary)
		checkEqual(model.fixture?.hasPlayerEdits, false)
		checkEqual(model.selection, [[], []])
		checkEqual(model.squadFocus?.control, .list)
		checkEqual(model.squadFocus?.side, 1)
		checkEqual(setup.standIn.runs.filter { $0.first == "list-players" }.count, 1)
		setup.checkTemporaryOnly()
		checkEqual(workFiles(setup), [])
		checkEqual(try Data(contentsOf: url), plainSampleJSON())
	}

	static func searchesSwaps() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		open(setup, setup.scratch.file("next.json", plainSampleJSON()))
		model.select(side: 1, blocks: [0])
		// Text typed while the list loads is searched once it arrives.
		setup.standIn.set(["DELAY_MS": "300"])
		guard let prompt = openPrompt(setup) else {
			return
		}
		prompt.type("ode")
		check(prompt.filterPending)
		check(waitUntil { !prompt.filterPending }, "the search did not run")
		checkEqual(prompt.matches, [])
		checkEqual(prompt.cue, "Unique ID (loading player names…)")
		// The search runs again once the list has arrived.
		let found = [ChoiceTests.odegaard, "Jan Bodegaard (5005)"]
		check(waitUntil { prompt.matches == found }, "the list was not searched: \(prompt.matches)")
		checkEqual(model.players.state, .loaded)
		// Typing waits for a pause before the list follows it.
		prompt.delay = 0.3
		prompt.type("muller")
		check(prompt.filterPending)
		checkEqual(prompt.matches, found)
		prompt.type("mulle")
		check(waitUntil { !prompt.filterPending }, "the search did not run")
		checkEqual(prompt.matches, ["Thomas Müller – FC Bayern (2002)"])
		// A label picked leaves the list as it is; fewer than three letters or an ID lists nothing.
		prompt.pick("Thomas Müller – FC Bayern (2002)")
		check(!prompt.filterPending)
		checkEqual(prompt.person, "2002")
		prompt.delay = 0.01
		for text in ["mu", "40347"] {
			prompt.type(text)
			check(waitUntil { !prompt.filterPending }, "the search did not run")
			checkEqual(prompt.matches, [])
		}
		checkEqual(prompt.person, "40347")
		// The field holds 1023 characters, as the Windows one.
		prompt.type(String(repeating: "z", count: 1100))
		checkEqual(prompt.text.count, 1023)
		check(prompt.person == nil)
		// No other fixture opens while the prompt is open (QuestionTests); once it has closed, one
		// opens with no prompt.
		model.cancelSwap()
		guard let reopened = openPrompt(setup) else {
			return
		}
		let other = setup.scratch.file("other.json", plainSampleJSON())
		model.open(other)
		checkEqual(setup.prompts.lastError, ViewerModel.askingMessage)
		check(model.swapPrompt === reopened)
		model.cancelSwap()
		open(setup, other)
		checkEqual(model.source?.name, "other.json")
		check(model.swapPrompt == nil)
	}

	static func dropsLists() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		open(setup, setup.scratch.file("next.json", plainSampleJSON()))
		model.editTeam(side: 1)
		loadTeams(model)
		model.teamEditor?.type("u")
		checkEqual(model.teamEditor?.matches.count, 3)
		model.cancelTeamEdit()
		model.select(side: 1, blocks: [0])
		model.requestSwap(side: 1)
		check(waitUntil { model.players.state == .loaded }, "the player list did not load")
		guard let prompt = model.swapPrompt else {
			check(false, "no swap prompt")
			return
		}
		prompt.type("ode")
		prompt.filter()
		checkEqual(prompt.matches.count, 2)
		// The game quits: another save may have other teams and players.
		setup.monitor.gameQuit()
		checkEqual(model.teams.state, .unloaded)
		checkEqual(model.players.state, .unloaded)
		check(model.teams.choices.isEmpty && model.players.choices.isEmpty)
		check(waitUntil { prompt.matches.isEmpty }, "the prompt still lists players")
		checkEqual(prompt.cue, "Unique ID")
		model.cancelSwap()
		checkEqual(setup.standIn.runs.count, 2)
		model.editTeam(side: 0)
		loadTeams(model)
		checkEqual(setup.standIn.runs.count, 3)
		checkEqual(setup.standIn.runs.last?.first, "list-teams")
		// A list the quit overtook is set aside.
		setup.standIn.set(["DELAY_MS": "300"])
		model.cancelTeamEdit()
		model.requestSwap(side: 1)
		checkEqual(model.players.state, .loading)
		setup.monitor.gameQuit()
		checkEqual(model.players.state, .unloaded)
		check(waitUntil(seconds: 1) { model.players.state == .loaded } == false)
		check(model.players.choices.isEmpty)
	}

	static func warnsBinaryTeams() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let prompts = setup.prompts
		// The venue sample names the teams that replaced both sides.
		let json = sampleJSON(FIXTURE_SAMPLE_VENUE)
		open(setup, setup.scratch.file("replaced.json", json))
		let binary = setup.scratch.file("replaced.bin")
		prompts.destination = (binary, true)
		prompts.answers = [false]
		model.requestSave()
		checkEqual(prompts.questions.last?.title, "Save fixture as")
		checkEqual(prompts.questions.last?.message, ViewerModel.binaryTeamsMessage)
		checkEqual(prompts.questions.last?.action, "Save as Binary")
		wait(model)
		check(!FileManager.default.fileExists(atPath: binary.path))
		prompts.answers = [true]
		model.requestSave()
		wait(model)
		check(FileManager.default.fileExists(atPath: binary.path))
		checkEqual(prompts.questions.count, 2)
		// JSON keeps the teams, and a fixture without replaced teams loses nothing: no question.
		prompts.destination = (setup.scratch.file("again.json"), false)
		model.requestSave()
		wait(model)
		checkEqual(prompts.questions.count, 2)
		open(setup, setup.scratch.file("plain.json", plainSampleJSON()))
		prompts.destination = (setup.scratch.file("plain.bin"), true)
		model.requestSave()
		wait(model)
		checkEqual(prompts.questions.count, 2)
		check(FileManager.default.fileExists(atPath: setup.scratch.file("plain.bin").path))
		check(prompts.errors.isEmpty, "\(prompts.errors)")
	}

	/// Exchanges the lineup sample's home places `first` and `second` in the Edit line-up sheet.
	private static func exchange(_ model: ViewerModel, _ first: Int, _ second: Int) {
		model.editLineup(side: 0)
		check(model.lineupEditor != nil, "no Edit line-up sheet")
		model.lineupEditor?.exchange(first, second)
		model.confirmLineup()
	}

	static func swapsAfterLineup() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let json = lineupSampleJSON()
		open(setup, setup.scratch.file("lu.json", json))
		let folder = model.work.folder.path
		// Wirtz into the XI and Gravenberch to the bench, with both their happiness maximised.
		model.select(side: 0, blocks: [10, 19])
		model.maxPlayers(.happiness, side: 0)
		exchange(model, 10, 19)
		// Gravenberch, selected, is in place 19 of the input, which has the new order written in.
		model.select(side: 0, blocks: [10])
		let input = setup.scratch.file("input.json")
		setup.standIn.set([
			"INPUT": input.path, "FIXTURE": setup.scratch.file("swapped.json", json).path,
		])
		model.requestSwap(side: 0)
		guard let prompt = model.swapPrompt else {
			check(false, "no swap prompt")
			return
		}
		checkEqual(prompt.label, "Swap Ryan Gravenberch for...")
		check(waitUntil { model.players.state != .loading }, "the player list did not load")
		prompt.type("40347")
		model.confirmSwap()
		wait(model)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(
			setup.standIn.runs.last,
			["inject-player", folder + "/in-2.json", "home.player[19]", "40347", folder + "/out-3.json"]
		)
		check(model.log.text.contains("Swapping home.player[19] for player 40347"), model.log.text)
		// Wirtz's edit goes with the order; Gravenberch's, swapped out, does not.
		var flags = [UInt8](repeating: 0, count: 512)
		flags[19] = happiness
		let written = try Data(contentsOf: input)
		checkEqual(
			written,
			try FixtureHandle.write(
				json: json,
				changes: PlayerChanges(flags: flags, lineup: exchangedOrder(10, 19)),
				venue: nil,
				binary: false
			)
		)
		checkEqual(occurrences("\"home_lineup\": true", in: written), 1)
		checkEqual(try FixtureHandle(json: written).playerName(side: 0, block: 19), "Ryan Gravenberch")
		// The result has no edits pending.
		check(model.isTemporary)
		checkEqual(model.fixture?.hasPlayerEdits, false)
		setup.checkTemporaryOnly()
		checkEqual(workFiles(setup), [])
	}

	static func refusesSquadSwaps() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let json = lineupSampleJSON()
		let url = setup.scratch.file("lu.json", json)
		open(setup, url)
		let folder = model.work.folder.path
		exchange(model, 10, 19)
		model.select(side: 0, blocks: [5])
		model.requestSwap(side: 0)
		guard let prompt = model.swapPrompt else {
			check(false, "no swap prompt")
			return
		}
		check(waitUntil { model.players.state != .loading }, "the player list did not load")
		let runs = setup.standIn.runs.count
		// The home squad's unique IDs, at the places the pending order gives them: the prompt stays
		// open with the reason, and the game is not asked.
		let squad = [
			("91193048", "Florian Wirtz is already in Liverpool's squad (Slot 10, ST)."),
			("37071197", "Ryan Gravenberch is already in Liverpool's squad (Sub)."),
			("2000164850", "Ben Doak is already in Liverpool's squad (Reserve)."),
			("29056345", "Wataru Endo is already in Liverpool's squad (Slot 5, DMCR)."),
		]
		for (person, reason) in squad {
			prompt.type(person)
			model.confirmSwap()
			check(model.swapPrompt === prompt, "the prompt closed for \(person)")
			checkEqual(setup.prompts.errors.last?.title, "Swap player")
			checkEqual(
				setup.prompts.lastError,
				reason
					+ " Use Edit Line-up to move this player, or tick \"Allow duplicates?\" to put a copy "
					+ "here."
			)
			check(!model.isBusy)
		}
		checkEqual(setup.standIn.runs.count, runs)
		check(model.log.text.contains("Error: Florian Wirtz is already in Liverpool's squad"))
		// The away side's IDs are the game's to check, and so is a copy that Allow duplicates asks for.
		setup.standIn.set(["FIXTURE": url.path])
		prompt.type("29083750")
		model.confirmSwap()
		wait(model)
		checkEqual(
			setup.standIn.runs.last,
			[
				"inject-player", folder + "/in-2.json", "home.player[5]", "29083750",
				folder + "/out-3.json",
			]
		)
		open(setup, url)
		exchange(model, 10, 19)
		model.select(side: 0, blocks: [5])
		model.requestSwap(side: 0)
		guard let copy = model.swapPrompt else {
			check(false, "no swap prompt")
			return
		}
		copy.type("91193048")
		copy.allowDuplicates = true
		model.confirmSwap()
		wait(model)
		checkEqual(
			setup.standIn.runs.last,
			[
				"inject-player", folder + "/in-4.json", "home.player[5]", "91193048",
				folder + "/out-5.json", "--allow-duplicate",
			]
		)
		// FM24's AI picks the away XI, which Edit line-up cannot change: fm24ctl's refusal, as before.
		open(setup, url)
		model.select(side: 1, blocks: [2])
		setup.standIn.set(["DUPLICATE": "1"])
		model.requestSwap(side: 1)
		model.swapPrompt?.type("29083750")
		model.confirmSwap()
		wait(model)
		checkEqual(setup.standIn.runs.last?.first, "inject-player")
		checkEqual(setup.standIn.runs.last?[2], "away.player[2]")
		check(
			setup.prompts.lastError?.hasSuffix("tick \"Allow duplicates?\" to swap them in anyway")
				== true,
			"\(setup.prompts.lastError ?? "")"
		)
	}

	static func dropsReplacedLineup() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let json = lineupSampleJSON()
		let url = setup.scratch.file("lu.json", json)
		let input = setup.scratch.file("input.json")
		setup.standIn.set([
			"INPUT": input.path, "FIXTURE": setup.scratch.file("replaced.json", json).path,
		])
		var awayFlags = [UInt8](repeating: 0, count: 512)
		awayFlags[FixtureHandle.blockCount + 2] = happiness
		// A home order and an away edit; the away team's replacement keeps the home order.
		for side in 0..<2 {
			open(setup, url)
			exchange(model, 10, 19)
			model.select(side: 1, blocks: [2])
			model.maxPlayers(.happiness, side: 1)
			check(model.fixture?.lineupEdited(side: 0) == true)
			model.editTeam(side: side)
			loadTeams(model)
			model.teamEditor?.type("677")
			model.confirmTeam()
			wait(model)
			check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
			let carried =
				side == 0
				? PlayerChanges(flags: awayFlags)
				: PlayerChanges(lineup: exchangedOrder(10, 19))
			let written = try Data(contentsOf: input)
			checkEqual(
				written,
				try FixtureHandle.write(json: json, changes: carried, venue: nil, binary: false)
			)
			checkEqual(occurrences("\"home_lineup\": true", in: written), side == 0 ? 0 : 1)
			// The result is the game's build, with the home order pending again only if it was kept.
			check(model.isTemporary)
			checkEqual(model.fixture?.hasPlayerEdits, side == 1)
			checkEqual(model.fixture?.lineupEdited(side: 0), side == 1)
			checkEqual(model.fixture?.lineupEdited(side: 1), false)
		}
		checkEqual(
			setup.standIn.runs.map { $0.first ?? "" },
			["list-teams", "replace-home", "replace-away"]
		)
	}

	/// The away team's replacement through the stand-in, which writes the fixture its FIXTURE names.
	private static func replaceAway(_ setup: HarnessSetup) {
		replaceTeam(setup, side: 1)
	}

	/// The side's team replacement through the stand-in, which writes the fixture its FIXTURE names.
	private static func replaceTeam(_ setup: HarnessSetup, side: Int) {
		let model = setup.model
		model.editTeam(side: side)
		loadTeams(model)
		model.teamEditor?.type("677")
		model.confirmTeam()
		wait(model)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	/// The log's lines from `first` on, without their times.
	private static func logged(_ model: ViewerModel, from first: Int) -> [String] {
		model.log.lines.dropFirst(first).map { String($0.drop { $0 != " " }.dropFirst()) }
	}

	static func keepsLineup() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let json = lineupSampleJSON()
		let lu = setup.scratch.file("lu.json", json)
		// What lineup writes, and a save after Edit line-up: Wirtz and Gravenberch exchanged, marked.
		let exchanged = PlayerChanges(lineup: exchangedOrder(10, 19))
		let marked = try FixtureHandle.write(
			json: json,
			changes: exchanged,
			venue: nil,
			binary: false
		)
		let markedURL = setup.scratch.file("marked.json", marked)
		let built = setup.scratch.file("built.json", json)
		let replaced = ["Replaced the away team; the fixture is not saved yet"]
		let kept = replaced + ["Kept the home line-up from before the replacement"]
		// A pending order, or the order of a marked file, is pending again in the game's build.
		setup.standIn.set(["FIXTURE": built.path])
		for url in [lu, markedURL] {
			open(setup, url)
			if url == lu {
				exchange(model, 10, 19)
			}
			let lines = model.log.lines.count
			replaceAway(setup)
			checkEqual(Array(logged(model, from: lines).suffix(2)), kept)
			checkEqual(model.fixture?.lineupEdited(side: 0), true)
			checkEqual(model.fixture?.lineupMarked(side: 0), false)
			checkEqual(model.fixture.map { Array($0.order(side: 0).prefix(22)) }, exchangedPlaces(10, 19))
			checkEqual(model.fixture?.cell(side: 0, row: 10, column: 1), "Florian Wirtz")
			check(model.fixture?.hasPlayerEdits == true)
		}
		// A save writes it, marker and all.
		let saved = setup.scratch.file("kept.json")
		setup.prompts.destination = (saved, false)
		model.requestSave()
		wait(model)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
		checkEqual(try Data(contentsOf: saved), marked)
		checkEqual(model.fixture?.lineupEdited(side: 0), false)
		checkEqual(model.fixture?.lineupMarked(side: 0), true)
		// fm24ctl has kept it already: nothing is pending, and the result is marked.
		setup.standIn.set(["FIXTURE": markedURL.path])
		open(setup, lu)
		exchange(model, 10, 19)
		var lines = model.log.lines.count
		replaceAway(setup)
		checkEqual(Array(logged(model, from: lines).suffix(2)), kept)
		checkEqual(model.fixture?.hasPlayerEdits, false)
		checkEqual(model.fixture?.lineupMarked(side: 0), true)
		checkEqual(model.fixture?.cell(side: 0, row: 10, column: 1), "Florian Wirtz")
		// A line-up neither changed nor marked is the game's.
		open(setup, lu)
		lines = model.log.lines.count
		replaceAway(setup)
		checkEqual(Array(logged(model, from: lines).suffix(1)), replaced)
		checkEqual(model.fixture?.hasPlayerEdits, false)
		checkEqual(model.fixture?.cell(side: 0, row: 10, column: 1), "Florian Wirtz")
		// Gravenberch, in the fixture by an ID the rebuild wrote back (as after a swap), is missing:
		// the player no one names, Gravenberch under his own ID, takes his place.
		let swapped = replacing(
			replacing(json, "\"u32\": 37071197}", with: "\"u32\": 2000040347}"),
			"\"u32\": 37071197}",
			with: "\"u32\": 2000040347}"
		)
		setup.standIn.set(["FIXTURE": built.path])
		open(setup, setup.scratch.file("swapped.json", swapped))
		exchange(model, 10, 19)
		lines = model.log.lines.count
		replaceAway(setup)
		checkEqual(
			Array(logged(model, from: lines).suffix(3)),
			kept + [
				"Ryan Gravenberch (2000040347) is not in the rebuilt home squad; Ryan Gravenberch "
					+ "(37071197) takes that place (Sub)"
			]
		)
		checkEqual(model.fixture.map { Array($0.order(side: 0).prefix(22)) }, exchangedPlaces(10, 19))
		// A rebuilt side whose order cannot change keeps the game's, with the reason in the log.
		let uneven = setup.scratch.file(
			"uneven.json",
			replacing(
				json,
				"\"home.player_count\", \"u8\": 22",
				with: "\"home.player_count\", \"u8\": 21"
			)
		)
		setup.standIn.set(["FIXTURE": uneven.path])
		open(setup, lu)
		exchange(model, 10, 19)
		lines = model.log.lines.count
		replaceAway(setup)
		checkEqual(
			Array(logged(model, from: lines).suffix(2)),
			replaced + [
				"Error: Cannot keep the home line-up from before the replacement: the new fixture's home "
					+ "order cannot be changed."
			]
		)
		checkEqual(model.fixture?.hasPlayerEdits, false)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	/// Moves Newcastle's players in awayLineupSampleJSON in a cycle in the Edit line-up sheet.
	private static func exchangeAway(_ model: ViewerModel) {
		model.editLineup(side: 1)
		check(model.lineupEditor != nil, "no Edit line-up sheet")
		for (first, second) in awayCycle {
			model.lineupEditor?.exchange(first, second)
		}
		model.confirmLineup()
	}

	static func keepsAwayLineup() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let json = awayLineupSampleJSON()
		let url = setup.scratch.file("away.json", json)
		let input = setup.scratch.file("input.json")
		setup.standIn.set([
			"INPUT": input.path, "FIXTURE": setup.scratch.file("built.json", json).path,
		])
		// The home team's replacement is given the away order, marked, and the game's build has it
		// pending again.
		open(setup, url)
		exchangeAway(model)
		let lines = model.log.lines.count
		replaceTeam(setup, side: 0)
		let written = try Data(contentsOf: input)
		checkEqual(
			written,
			try FixtureHandle.write(
				json: json,
				changes: PlayerChanges(lineup: exchangedOrder(awayCycle, side: 1)),
				venue: nil,
				binary: false
			)
		)
		checkEqual(occurrences("\"away_lineup\": true", in: written), 1)
		checkEqual(
			Array(logged(model, from: lines).suffix(2)),
			[
				"Replaced the home team; the fixture is not saved yet",
				"Kept the away line-up from before the replacement",
			]
		)
		check(model.isTemporary)
		checkEqual(model.fixture?.lineupEdited(side: 1), true)
		checkEqual(model.fixture?.lineupEdited(side: 0), false)
		checkEqual(
			model.fixture.map { Array($0.order(side: 1).prefix(13)) },
			exchangedPlaces(awayCycle, count: 13)
		)
		checkEqual(model.fixture?.cell(side: 1, row: 2, column: 1), "Lewis Miley")
		// The away team's replacement drops it.
		open(setup, url)
		exchangeAway(model)
		replaceTeam(setup, side: 1)
		checkEqual(try Data(contentsOf: input), json)
		checkEqual(model.fixture?.hasPlayerEdits, false)
		checkEqual(model.fixture?.lineupEdited(side: 1), false)
		checkEqual(model.fixture?.cell(side: 1, row: 2, column: 1), "Sven Botman")
		checkEqual(
			setup.standIn.runs.map { $0.first ?? "" },
			["list-teams", "replace-home", "replace-away"]
		)
		check(setup.prompts.errors.isEmpty, "\(setup.prompts.errors)")
	}

	static func swapsAfterAwayLineup() throws {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let json = awayLineupSampleJSON()
		open(setup, setup.scratch.file("away.json", json))
		let folder = model.work.folder.path
		exchangeAway(model)
		// Miley, selected, is in place 2 of the input, which has the new order written in.
		model.select(side: 1, blocks: [12])
		let input = setup.scratch.file("input.json")
		setup.standIn.set([
			"INPUT": input.path, "FIXTURE": setup.scratch.file("swapped.json", json).path,
		])
		model.requestSwap(side: 1)
		guard let prompt = model.swapPrompt else {
			check(false, "no swap prompt")
			return
		}
		checkEqual(prompt.label, "Swap Lewis Miley for...")
		check(waitUntil { model.players.state != .loading }, "the player list did not load")
		// Newcastle's own players, at the places the pending order gives them, are refused without
		// asking the game.
		let runs = setup.standIn.runs.count
		let squad = [
			("28018613", "Kieran Trippier is already in Newcastle's squad (Sub)."),
			("37063649", "Sven Botman is already in Newcastle's squad (Slot 1, DR)."),
		]
		for (person, reason) in squad {
			prompt.type(person)
			model.confirmSwap()
			check(model.swapPrompt === prompt, "the prompt closed for \(person)")
			checkEqual(
				setup.prompts.lastError,
				reason
					+ " Use Edit Line-up to move this player, or tick \"Allow duplicates?\" to put a copy "
					+ "here."
			)
		}
		checkEqual(setup.standIn.runs.count, runs)
		prompt.type("40347")
		model.confirmSwap()
		wait(model)
		checkEqual(
			setup.standIn.runs.last,
			["inject-player", folder + "/in-2.json", "away.player[2]", "40347", folder + "/out-3.json"]
		)
		checkEqual(
			try Data(contentsOf: input),
			try FixtureHandle.write(
				json: json,
				changes: PlayerChanges(lineup: exchangedOrder(awayCycle, side: 1)),
				venue: nil,
				binary: false
			)
		)
		check(model.isTemporary)
		checkEqual(model.fixture?.hasPlayerEdits, false)
		setup.checkTemporaryOnly()
	}
}
