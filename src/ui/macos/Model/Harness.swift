// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// A line fm24ctl printed: on standard output (the harness's result lines) or on standard error
/// (why fm24ctl itself failed, and its notices).
enum HarnessLine: Equatable {
	case output(String)
	case error(String)
}

/// How an fm24ctl run ended: its exit status and what it printed, line by line.
struct HarnessResult: Equatable {
	/// What fm24ctl prints while another job keeps the harness busy.
	static let waitingNotice = "the harness is running another job; waiting for it to finish"
	/// The start of what fm24ctl prints when FM24 is not running and it starts the game to load the
	/// save of the fixture a job is for.
	static let startingGamePrefix = "Football Manager 2024 is not running; starting it to load "
	/// fm24ctl's exit status when the game is not in the state the job needs: it has another save
	/// loaded, is at its start screen, or stayed busy (commands.h).
	static let wrongState: Int32 = 3
	/// fm24ctl's exit status for arguments it refuses (commands.h), such as a seed pattern the
	/// harness would not take: the reason comes before the usage line.
	static let usage: Int32 = 2
	static let usagePrefix = "usage: "
	/// How fm24ctl's refusal of a job for another save than the one loaded ends: --any-save would
	/// use the loaded save.
	static let anySaveHint = ", or pass --any-save to use the loaded save"
	/// The start of the line with which fm24ctl reports the save loaded and idle.
	static let readyPrefix = "ready: "
	/// The start of the lines with which fm24ctl reports the game's state while it waits for it.
	static let statePrefix = "FM24 state: "
	/// The starts of fm24ctl's notices before a job, which say nothing about why the job failed:
	/// run's warning that the loaded save is at another date than the fixture's, and that it cannot
	/// read or check which save is loaded.
	static let noticePrefixes = [
		"warning: ", "the harness cannot read which save FM24 has loaded; ",
		"cannot check which save FM24 has loaded; ",
	]

	var status: Int32
	var output: [String] = []
	var errors: [String] = []
	/// False if fm24ctl could not be started at all; `errors` then says why.
	var started = true

	var succeeded: Bool {
		started && status == 0
	}

	/// The headline when no harness of this protocol serves the job socket.
	static var notServing: String {
		"FM24 isn't running with harness \(FixtureHandle.protocolVersion)."
	}

	/// fm24ctl refused the job because FM24 has another save loaded than the fixture's, which
	/// --any-save would use.
	var refusedOtherSave: Bool {
		started && status == HarnessResult.wrongState
			&& errors.contains { $0.hasSuffix(HarnessResult.anySaveHint + ".") }
	}

	/// Why the run failed, or nil if it succeeded (see diagnosis).
	func problem(fallback: String, serving: () -> Bool) -> String? {
		diagnosis(fallback: fallback, serving: serving)?.message
	}

	/// Why the run failed, or nil if it succeeded. In order of preference: the harness's last
	/// "error:" line (such as its refusal while the game loads or processes); for the usage status,
	/// fm24ctl's first line of errors that is not its usage line, which says what it refused; for
	/// the wrongState status, fm24ctl's last line of errors, which says what the game is doing
	/// instead; if no harness of this protocol serves the job socket (`serving`, asked only then),
	/// notServing with fm24ctl's first line of errors, which names the case (the game is not
	/// running, runs without the harness, or has another version), offering to launch the game;
	/// fm24ctl's last line of errors; or `fallback`. fm24ctl's notices (noticePrefixes) never
	/// explain a failure.
	func diagnosis(fallback: String, serving: () -> Bool) -> HarnessProblem? {
		guard !succeeded else {
			return nil
		}
		let explained = reasons
		guard started else {
			return HarnessProblem(message: explained.first ?? fallback)
		}
		if let line = output.last(where: { $0.hasPrefix("error:") }) {
			return HarnessProblem(message: line)
		}
		if status == HarnessResult.usage,
			let line = explained.first(where: { !$0.hasPrefix(HarnessResult.usagePrefix) })
		{
			return HarnessProblem(message: line)
		}
		if status == HarnessResult.wrongState, let line = explained.last {
			return HarnessProblem(message: line)
		}
		if !serving() {
			let message =
				explained.first.map { "\(HarnessResult.notServing)\n\($0)" } ?? HarnessResult.notServing
			return HarnessProblem(message: message, offersLaunch: true)
		}
		return HarnessProblem(message: explained.last ?? fallback)
	}

	/// Why `launch` or `install` failed, or nil if it succeeded: every line of errors fm24ctl
	/// printed (which is all it prints about the failure), else the harness's last "error:" line,
	/// else `fallback`.
	func explanation(fallback: String) -> String? {
		guard !succeeded else {
			return nil
		}
		let explained = explanations
		if !explained.isEmpty {
			return explained.joined(separator: "\n")
		}
		return (started ? output.last(where: { $0.hasPrefix("error:") }) : nil) ?? fallback
	}

	/// fm24ctl's lines of errors, trimmed, without blank ones or its notice of waiting.
	private var explanations: [String] {
		errors.map { $0.trimmingCharacters(in: .whitespaces) }
			.filter { !$0.isEmpty && $0 != HarnessResult.waitingNotice }
	}

	/// Those lines without fm24ctl's notices before a job.
	private var reasons: [String] {
		explanations.filter { line in !HarnessResult.noticePrefixes.contains { line.hasPrefix($0) } }
	}
}

/// Why a harness job failed, and whether launching the game would help: no harness of this
/// protocol served the job.
struct HarnessProblem: Equatable {
	var message: String
	var offersLaunch = false
}

/// The harness jobs the viewer has fm24ctl run (Fm24ctl), or tests fake. Paths are absolute, and
/// every one the game reads or writes is in the user's temporary directory (WorkFiles). `onLine`
/// gets each line as it is printed, on the main thread, before the result comes back. Cancelling
/// the task that awaits a job stops it where it can.
@MainActor
protocol HarnessService: AnyObject {
	/// get-next-fixture: the game exports the next fixture to `output`.
	func exportNextFixture(
		to output: URL,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult

	/// decode, without --full: the game decodes a binary fixture into JSON, as the Windows viewer's
	/// inspect job does.
	func decode(
		_ input: URL,
		to output: URL,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult

	/// run: the game plays `input` with the options' seeds and threads, writing the results to
	/// `output` row by row as it goes (and, with --detailed, its reports beside them). With
	/// `anySave` (--any-save), a fixture for another save than the one loaded plays in that one.
	func simulate(
		_ input: URL,
		options: SimulationOptions,
		output: URL,
		anySave: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult

	/// replace-home or replace-away (`side` 1): the game rebuilds a side of `input` with `team`
	/// (an ID or a club's short name), writing the fixture to `output`; `anySave` as for simulate.
	func replaceTeam(
		side: Int,
		input: URL,
		team: String,
		output: URL,
		anySave: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult

	/// inject-player: the game's values for the player `person` (a unique ID) go into `block`
	/// ("home.player[N]") of `input`, written to `output`; with `allowDuplicate`, even if the player
	/// is in the fixture already; `anySave` as for simulate.
	func injectPlayer(
		input: URL,
		block: String,
		person: String,
		output: URL,
		allowDuplicate: Bool,
		anySave: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult

	/// list-teams: the game lists its teams in `output`.
	func listTeams(to output: URL, onLine: @escaping (HarnessLine) -> Void) async -> HarnessResult

	/// list-players: the game lists its players in `output`.
	func listPlayers(to output: URL, onLine: @escaping (HarnessLine) -> Void) async -> HarnessResult

	/// launch: starts the game with the harness (with `rosetta`, its x86_64 slice under Rosetta),
	/// with `save` (a save's name or its .fm file) loaded, and waits until it is loaded and idle.
	func launch(
		save: String?,
		rosetta: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult

	/// install: writes the harness and its launcher for the game to load when it next starts, and
	/// says what Steam's launch option for them is.
	func install(onLine: @escaping (HarnessLine) -> Void) async -> HarnessResult

	/// Whether a harness of this protocol serves the job socket: a check of its lock file, which
	/// sends the game nothing.
	func isServing() -> Bool
}
