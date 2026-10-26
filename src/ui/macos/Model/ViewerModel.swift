// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Combine
import FixtureCore
import Foundation

/// The summary's first line: the file being viewed and any unsaved edits.
struct SummaryHeading: Equatable {
	static let prefix = "Viewing fixture: "

	var name: String
	var marker: String

	/// What the heading adds for unsaved edits.
	static func marker(venue: Bool, players: Bool) -> String {
		switch (venue, players) {
		case (true, true): return " (unsaved venue and player edits)"
		case (false, true): return " (unsaved player edits)"
		case (true, false): return " (unsaved venue edits)"
		case (false, false): return ""
		}
	}
}

/// The viewer window's state and rules, as the Windows viewer's WindowState keeps them: the fixture
/// on show and the file it came from, the selection, the player and venue edits not yet saved, the
/// team editor and swap prompt with the team and player lists they search, and the one operation
/// that may run: a file operation (which includes the harness jobs that export the next fixture
/// and decode binary ones), a team replacement or player swap, or a simulation, beside which only
/// an export may run. While a question, panel or the swap prompt waits for an answer, nothing
/// starts, as nothing can while a Windows dialog is open.
@MainActor
final class ViewerModel: ObservableObject {
	enum Busy: Equatable {
		case none
		/// An open, a save, an export, or the startup export.
		case file
		/// A team replacement or player swap.
		case replacement
		case simulation
	}

	/// What the start panel's Retry does: the startup export, or the open that failed.
	enum StartAction: Equatable {
		case export
		case open(URL)
	}

	enum Tab: Hashable {
		case squads
		case venue
		case simulation
	}

	/// What closing the window, or quitting, needs first.
	enum CloseCheck: Equatable {
		case close
		/// An operation is running; the message says to wait.
		case blocked(String)
		/// There are unsaved edits, to be discarded only if the user says so.
		case confirm
	}

	/// A request to put the keyboard focus in a venue field; `serial` tells requests apart.
	struct VenueFocus: Equatable {
		let field: Int
		let serial: Int
	}

	/// The same for a field of the Simulation tab.
	struct SimulationFocus: Equatable {
		let field: SimulationForm.Field
		let serial: Int
	}

	/// The same for a side's pencil (after its team was replaced) or squad list (after a swap).
	struct SquadFocus: Equatable {
		enum Control: Equatable {
			case pencil
			case list
		}

		let side: Int
		let control: Control
		let serial: Int
	}

	static let errorTitle = "FM24 Match Harness"
	static let busyMessage = "Please wait for the current fixture operation to finish."
	static let askingMessage = "Please finish with the open dialog first."
	static let closeBusyMessage = "A fixture file operation is running. Please wait before closing."
	static let closeReplacementMessage =
		"A team replacement is running. Please wait before closing."
	static let closeSimulationMessage =
		"A simulation is running. Please wait for it to finish before closing."
	static let openTitle = "Open fixture"
	static let openDiscardMessage =
		"The current fixture has unsaved changes. Discard them and open the selected fixture?"
	static let closeTitle = "Close fixture"
	static let closeDiscardMessage = "Fixture changes are not saved. Discard them and close?"
	static let discardAction = "Discard"
	static let unsavedPrefix = "This fixture object is not saved. "
	static let exportLink = "Click here"
	static let exportSuffix = " to export it"
	static let exportBusyMessage =
		"Please wait for the fixture file operation to finish before exporting."
	static let exportReplacementMessage =
		"Please wait for the team replacement to finish before exporting."
	static let exportNameTooLong = "Fixture export filename is too long."
	static let exportFailed =
		"Cannot export the next fixture; FM24 must be running and idle. See the log and harness.log."
	static let decodeFailed =
		"cannot decode binary fixture; FM24 must be running and idle. See the log and harness.log."
	static let noHarness =
		"FM24 Viewer's copy of fm24ctl cannot be found, so it cannot reach FM24. Build it again with "
		+ "scripts/build.sh."
	static let exportingActivity = "Exporting the next fixture from FM24…"
	static let exportCancelled = "Exporting the next fixture was cancelled."
	static let openCancelled = "Opening the fixture was cancelled."
	static let decodingActivity = "Decoding the binary fixture with FM24…"
	static let waitingActivity = "Waiting for another harness job…"
	static let startingGameActivity = "Starting FM24 with the fixture's save…"
	static let idleWaitActivity = "Waiting for FM24 to be idle with the fixture's save…"
	static let simulatingStatus = "Simulating fixture... Please wait before closing the window."
	static let simulationFailed = "Simulation failed. Check the log and harness.log for details."
	static let resultsBeside = "Results are saved beside the fixture, using its filename with .csv."
	static let resultsInExportFolder =
		"Results are saved in the export folder, using the fixture's export name with .csv."
	static let completeBeside =
		"Simulation complete. Results were saved beside the fixture; see the log for paths."
	static let completeInExportFolder =
		"Simulation complete. Results were saved in the export folder; see the log for paths."
	static let resultsMissing = "The last simulation's results cannot be found."
	static let replaceFailed =
		"Cannot replace team; FM24 must be running and idle. See the log and harness.log."
	static let swapFailed =
		"Cannot fetch the player; FM24 must be running and idle. See the log and harness.log."
	static let listTeamsFailed =
		"Cannot list teams; FM24 must be running and idle. See harness.log."
	static let listPlayersFailed =
		"Cannot list players; FM24 must be running and idle. See harness.log."
	static let swappingActivity = "Swapping the player with FM24…"
	static let saveTitle = "Save fixture as"
	static let binaryTeamsMessage =
		"Binary fixtures do not keep the replaced teams (home_team and away_team) that per-seed "
		+ "rebuilds need, so a binary copy plays only with Skip rebuild or the keep seed. Keep the "
		+ "JSON for rebuilds. Save as binary anyway?"
	static let binaryTeamsAction = "Save as Binary"
	static let runAnywayAction = "Run Anyway"
	/// What the question about another loaded save says in place of fm24ctl's --any-save.
	static let runAnywayHint = ", or choose Run Anyway to use the loaded save"
	static let launchFailed = "Cannot start FM24 with the harness. See the log."
	static let launchStopped =
		"Stopped waiting for FM24, which may still be starting, and may not load the save: check the "
		+ "game before launching it again."
	/// What the sheet says in place of fm24ctl's --user-data, which it cannot pass, when Steam's
	/// launch option sets the game's data folder in a way fm24ctl does not follow.
	static let userDataHint = "pass the folder with --user-data"
	static let launchOptionHint =
		"give it in Steam's launch option as --user_data_location=<absolute folder>, or launch FM24 "
		+ "at its start screen and load the save there"
	/// The log's note of a --rosetta that --launch-save cannot pass on.
	static let rosettaIgnored =
		"--rosetta is for Apple silicon: this Mac runs the game's x86_64 slice natively, so FM24 "
		+ "starts without it."
	static let installFailed = "Cannot install the harness. See the log."
	static let installUnread =
		"fm24ctl install did not say where it installed the harness. See the log."
	static let installingActivity = "Installing the harness…"
	/// The error boxes' second button for a job no harness served: Launch FM24… once Steam's launch
	/// option is set, else Install Harness…, with setOptionFirst, as Launch FM24… would then start
	/// the game directly, which has not been verified.
	static let launchAction = "Launch FM24…"
	static let installAction = "Install Harness…"
	static let setOptionFirst =
		"Steam's launch option for FM24 is not set, or Steam has not saved it yet: choose Install "
		+ "Harness… to set it, then start FM24 from Steam. Launch FM24… would open the game without "
		+ "the option, which has not been verified: the game crashed in its one attempt."

	static func replacingActivity(side: Int) -> String {
		"Replacing the \(side == 0 ? "home" : "away") team with FM24…"
	}

	/// fm24ctl's refusal of a job for another save than the one loaded, as the question about it
	/// puts it: with Run Anyway in place of --any-save.
	static func otherSaveQuestion(_ refusal: String) -> String {
		guard let hint = refusal.range(of: HarnessResult.anySaveHint) else {
			return refusal
		}
		return refusal.replacingCharacters(in: hint, with: runAnywayHint)
	}

	@Published private(set) var source: FixtureSource?
	@Published private(set) var fixture: FixtureHandle?
	/// A file operation runs; an export may run beside a simulation.
	@Published private(set) var fileOperation = false
	@Published var tab = Tab.squads
	/// The selected player blocks of each side.
	@Published private(set) var selection: [Set<Int>] = [[], []]
	/// Why no fixture is open, for the start panel ("" before anything has failed).
	@Published private(set) var status = ""
	/// What the start panel's Retry does, after the startup export or the first open failed.
	@Published private(set) var retry: StartAction?
	/// What a running harness job is doing, while it runs.
	@Published private(set) var activity: String?
	/// fm24ctl's latest line of progress while it starts FM24 with the fixture's save for a job, or
	/// waits for the game to be idle with it.
	@Published private(set) var activityDetail: String?
	/// The Venue tab's entries.
	@Published private(set) var venue = VenueForm()
	/// A venue field that should take the keyboard focus, until it has.
	@Published private(set) var venueFocus: VenueFocus?
	/// Counts changes to the fixture on show (a new fixture, player edits, a save), which the C core
	/// holds out of SwiftUI's sight: views of its cells refresh when it changes.
	@Published private(set) var revision = 0
	/// The Simulation tab's entries, kept when another fixture opens.
	@Published private(set) var simulationForm = SimulationForm()
	/// The running simulation's progress, or how the last one ended.
	@Published private(set) var simulation = SimulationState()
	/// A Simulation tab field that should take the keyboard focus, until it has.
	@Published private(set) var simulationFocus: SimulationFocus?
	/// The team replacement or player swap running.
	@Published private(set) var replacement: ReplacementJob?
	/// The open team editor: one side at a time.
	@Published private(set) var teamEditor: TeamEditor?
	/// The open swap prompt.
	@Published private(set) var swapPrompt: SwapPrompt?
	/// A pencil or squad list that should take the keyboard focus, until it has.
	@Published private(set) var squadFocus: SquadFocus?
	/// The open Launch FM24 sheet.
	@Published private(set) var launchPrompt: LaunchPrompt?
	/// `fm24ctl launch` runs, also once Cancel has asked it to end, until it has.
	@Published private(set) var launchRunning = false
	/// The open Install Harness sheet.
	@Published private(set) var installState: InstallState?
	/// Its Copy has put Steam's launch option on the clipboard.
	@Published private(set) var installCopied = false
	/// The game's teams and players, for the team editors and the swap prompt.
	let teams = ChoiceList(.teams)
	let players = ChoiceList(.players)

	let log: ViewerLog
	weak var prompts: ViewerPrompts?
	/// fm24ctl, through which the viewer reaches the game; nil if it cannot be found.
	let harness: HarnessService?
	let work: WorkFiles
	let settings: ViewerSettings
	/// Follows the game for the start panel; nil without one (tests and snapshots).
	let monitor: GameMonitor?
	/// Told of each file opened or saved, for Open Recent.
	var noteFile: (URL) -> Void = { _ in }
	/// The clock that names exports.
	var now: () -> Date = Date.init
	/// Keeps the Mac awake and the viewer out of App Nap while a simulation runs, which the game
	/// plays in the background; returns what ends it.
	var beginActivity: (String) -> () -> Void = { reason in
		let activity = ProcessInfo.processInfo.beginActivity(
			options: [.userInitiated, .idleSystemSleepDisabled],
			reason: reason
		)
		return { ProcessInfo.processInfo.endActivity(activity) }
	}
	/// Shows files in Finder.
	var reveal: ([URL]) -> Void = { _ in }
	/// How often a simulation's results are read for its progress.
	var progressInterval: TimeInterval = 0.2
	/// The home folder, whose FM24 save folders the Launch FM24 sheet lists, as fm24ctl finds them.
	var home = ViewerModel.homeFolder()
	/// Steam's launch option for FM24 runs the harness's launcher in `home`, so that `fm24ctl
	/// launch` starts the game through Steam (the tested route) and not directly.
	var launchOptionSet: Bool {
		LaunchSaves.launchOptionSet(home: home)
	}
	/// Apple silicon, where the game's x86_64 slice runs under Rosetta.
	var appleSilicon = ViewerModel.onAppleSilicon()
	/// Puts text on the clipboard (the Install Harness sheet's Copy).
	var copyText: (String) -> Void = { _ in }
	/// `fm24ctl launch`, while it runs, and a count of launches, so that one that was cancelled
	/// leaves the sheet alone when it ends.
	private var launchTask: Task<Void, Never>?
	private var launchSerial = 0
	/// fm24ctl is starting FM24 with the fixture's save for the job that runs, or waiting for the
	/// game to be idle with it, after which the job goes on with `resumedActivity`.
	private var waitingForGame = false
	private var resumedActivity: String?
	private var focusSerial = 0
	private var watchers: Set<AnyCancellable> = []
	/// Questions and panels put to the user and not yet answered.
	private var questions = 0
	/// The startup export, or an open while no fixture is on show, with what Retry does after it:
	/// Cancel, closing and quitting stop it, as Ctrl+C stops fm24ctl's export on Windows.
	private var starting: (task: Task<Void, Never>, action: StartAction)?
	/// Counts those jobs, so that one that was cancelled leaves the window alone when it ends.
	private var startSerial = 0

	init(
		log: ViewerLog,
		prompts: ViewerPrompts? = nil,
		harness: HarnessService? = nil,
		work: WorkFiles = WorkFiles(),
		settings: ViewerSettings = ViewerSettings(),
		monitor: GameMonitor? = nil
	) {
		self.log = log
		self.prompts = prompts
		self.harness = harness
		self.work = work
		self.settings = settings
		self.monitor = monitor
		monitor?.quit
			.sink { [weak self] in self?.dropLists() }
			.store(in: &watchers)
	}

	var title: String {
		"FM24 Match Harness (protocol \(FixtureHandle.protocolVersion))"
	}

	/// The operation running, a file operation first, as the Windows close checks take them.
	var busy: Busy {
		if fileOperation {
			return .file
		}
		return replacement != nil ? .replacement : simulation.running ? .simulation : .none
	}

	var isBusy: Bool {
		busy != .none
	}

	/// A question, panel, the swap prompt or the Launch FM24 or Install Harness sheet waits for the
	/// user.
	var isAsking: Bool {
		questions > 0 || swapPrompt != nil || launchPrompt != nil || installState != nil
	}

	/// Venue or player edits that no save has written.
	var hasUnsavedEdits: Bool {
		venue.dirty || fixture?.hasPlayerEdits == true
	}

	/// An export not yet saved is on show: the summary offers to export it.
	var isTemporary: Bool {
		fixture != nil && source?.temporary == true
	}

	/// The summary's first line for a saved fixture (nil for one not yet saved).
	var heading: SummaryHeading? {
		guard let source, let fixture, !source.temporary else {
			return nil
		}
		return SummaryHeading(
			name: source.name,
			marker: SummaryHeading.marker(venue: venue.dirty, players: fixture.hasPlayerEdits)
		)
	}

	// MARK: Opening

	/// Asks for a fixture with the Open panel and opens it.
	func requestOpen() {
		guard !refuseWhileBusy() else {
			return
		}
		ask { prompts, answered in
			prompts.chooseFixture { [weak self] url in
				answered()
				if let url {
					self?.requestOpen(url)
				}
			}
		}
	}

	/// Opens a fixture the user chose (the Open panel, Open Recent, a drop or Open With), first asking
	/// whether to discard the fixture on show if it has unsaved changes. The next fixture's export
	/// (WorkFiles.temporaryFixture, which `fm24ctl` with no arguments opens) opens as not yet saved.
	func requestOpen(_ url: URL) {
		guard !refuseWhileBusy() else {
			return
		}
		guard FixtureFiles.acceptable(url) else {
			report(FixtureFiles.pathProblem)
			return
		}
		let temporary = work.isTemporaryFixture(url)
		guard source?.temporary == true || hasUnsavedEdits else {
			open(url, temporary: temporary)
			return
		}
		ask { prompts, answered in
			prompts.confirm(
				ViewerModel.openDiscardMessage,
				title: ViewerModel.openTitle,
				action: ViewerModel.discardAction
			) { [weak self] discard in
				answered()
				if discard {
					self?.open(url, temporary: temporary)
				}
			}
		}
	}

	/// Opens a fixture in the background: JSON is read here, a binary fixture decoded by the game.
	/// The fixture on show stays if it fails.
	func open(_ url: URL, temporary: Bool = false) {
		guard !refuseWhileBusy() else {
			return
		}
		guard FixtureFiles.acceptable(url) else {
			report(FixtureFiles.pathProblem)
			return
		}
		fileOperation = true
		log.add("Opening \(url.path)")
		let first = fixture == nil
		startSerial += 1
		let serial = startSerial
		let task = Task {
			let result = await load(url, temporary: temporary)
			guard serial == startSerial else {
				return
			}
			starting = nil
			finishOpening(url, result, retry: .open(url))
		}
		starting = first ? (task, .open(url)) : nil
	}

	/// Exports the next fixture from the game to temp-fixture.json in the user's temporary
	/// directory and shows it as not yet saved, as the Windows viewer starts. If that fails, the
	/// start panel says why and offers Retry.
	func exportNextFixture() {
		guard !refuseWhileBusy() else {
			return
		}
		retry = .export
		guard let harness else {
			status = ViewerModel.noHarness
			log.error(status)
			return
		}
		let target = work.temporaryFixture
		fileOperation = true
		status = ""
		activity = ViewerModel.exportingActivity
		log.add("Exporting the next fixture to \(target.path)")
		startSerial += 1
		let serial = startSerial
		let task = Task {
			let result = await harness.exportNextFixture(to: target) { line in
				self.harnessLine(line)
			}
			guard serial == startSerial else {
				return
			}
			activity = nil
			if let problem = problem(result, fallback: ViewerModel.exportFailed, harness) {
				starting = nil
				fileOperation = false
				status = problem.message
				log.error(problem.message)
				return
			}
			let loaded = await load(target, temporary: true)
			guard serial == startSerial else {
				return
			}
			starting = nil
			finishOpening(target, loaded, retry: .export)
		}
		starting = (task, .export)
	}

	/// The start panel's Cancel, also shown as the startup export or first open runs.
	var canCancelStart: Bool {
		starting != nil
	}

	/// Stops the startup export or first open: its fm24ctl ends, and the start panel offers Retry.
	func cancelStart() {
		guard let start = starting else {
			return
		}
		starting = nil
		startSerial += 1
		start.task.cancel()
		fileOperation = false
		activity = nil
		retry = start.action
		status = start.action == .export ? ViewerModel.exportCancelled : ViewerModel.openCancelled
		log.add(status)
	}

	/// Tries the startup export, or the open that failed, again.
	func retryStart() {
		switch retry {
		case .export:
			exportNextFixture()
		case .open(let url):
			open(url, temporary: work.isTemporaryFixture(url))
		case nil:
			break
		}
	}

	/// Shows an opened fixture, starting afresh: no selection, edits, team editor or swap prompt.
	func show(_ source: FixtureSource, _ handle: FixtureHandle) {
		display(source, handle)
		status = ""
		retry = nil
		simulation = SimulationState()
		simulationFocus = nil
	}

	/// Shows a fixture in place of the one on show, with no selection, edits, team editor or swap
	/// prompt; a replacement keeps the Simulation tab's summary, as on Windows.
	private func display(_ source: FixtureSource, _ handle: FixtureHandle) {
		self.source = source
		fixture = handle
		selection = [[], []]
		venue = VenueForm(handle.venue)
		venueFocus = nil
		teamEditor = nil
		swapPrompt = nil
		squadFocus = nil
		revision += 1
	}

	private func load(
		_ url: URL,
		temporary: Bool
	) async -> Result<(FixtureSource, FixtureHandle), Error> {
		if FixtureSource.kind(of: url) == .binary {
			return await decode(url)
		}
		return await Task.detached(priority: .userInitiated) {
			Result { try FixtureFiles.load(url, temporary: temporary) }
		}.value
	}

	/// Opens a binary fixture: the game decodes a copy in the viewer's temporary folder, and both the
	/// fixture's bytes and its JSON are kept, so that saving it needs no second decode.
	private func decode(_ url: URL) async -> Result<(FixtureSource, FixtureHandle), Error> {
		guard let harness else {
			return .failure(ViewerError(message: ViewerModel.noHarness))
		}
		let input: URL
		let output: URL
		do {
			input = try work.file("in", extension: "bin")
			output = try work.file("decoded", extension: "json")
		} catch {
			return .failure(error)
		}
		defer {
			work.remove(input, output)
		}
		let copied = await Task.detached(priority: .userInitiated) {
			Result { try FixtureFiles.copyBinary(url, to: input) }
		}.value
		let bytes: Data
		switch copied {
		case .success(let read):
			bytes = read
		case .failure(let error):
			return .failure(error)
		}
		activity = ViewerModel.decodingActivity
		let result = await harness.decode(input, to: output) { [weak self] line in
			self?.harnessLine(line)
		}
		// Cancelled: the window has moved on.
		guard !Task.isCancelled else {
			return .failure(ViewerError(message: ViewerModel.openCancelled))
		}
		activity = nil
		if let problem = problem(result, fallback: ViewerModel.decodeFailed, harness) {
			return .failure(ViewerError(message: problem.message, offersLaunch: problem.offersLaunch))
		}
		return await Task.detached(priority: .userInitiated) {
			Result { try FixtureFiles.loadDecoded(url, bytes: bytes, decoded: output) }
		}.value
	}

	private func finishOpening(
		_ url: URL,
		_ result: Result<(FixtureSource, FixtureHandle), Error>,
		retry action: StartAction
	) {
		fileOperation = false
		activity = nil
		switch result {
		case .success(let (source, handle)):
			show(source, handle)
			if !source.temporary {
				noteFile(url)
			}
			log.add("Opened \(source.temporary ? "the next fixture" : source.name)")
		case .failure(let failure):
			let text = message(failure)
			// The start panel offers Launch FM24… itself.
			let offersLaunch = fixture != nil && (failure as? ViewerError)?.offersLaunch == true
			if fixture == nil {
				status = text
				retry = action
			}
			report(text, offeringLaunch: offersLaunch)
		}
	}

	// MARK: Saving

	/// Asks for a destination with the Save panel and saves the fixture there, once the venue
	/// entries are valid.
	func requestSave() {
		guard !refuseWhileBusy(), let source, let fixture, readVenue() != nil else {
			return
		}
		// A fixture not yet saved is offered under its export's name, in the export folder.
		var name = source.name
		var directory: URL? = source.url.deletingLastPathComponent()
		if source.temporary {
			name = exportName(fixture, extension: source.kind == .binary ? ".bin" : ".json") ?? name
			let folder = settings.exportFolder
			directory = FileManager.default.fileExists(atPath: folder.path) ? folder : nil
		}
		ask { prompts, answered in
			prompts.chooseDestination(
				name: name,
				directory: directory,
				binary: source.kind == .binary
			) { [weak self] url, binary in
				answered()
				guard let self, let url else {
					return
				}
				// Binary data drops the teams a replacement put in (docs/CLI.md): say so first.
				guard binary, let shown = self.source, FixtureHandle.replacesTeams(shown.json) else {
					self.save(to: url, binary: binary)
					return
				}
				self.ask { prompts, answered in
					prompts.confirm(
						ViewerModel.binaryTeamsMessage,
						title: ViewerModel.saveTitle,
						action: ViewerModel.binaryTeamsAction
					) { [weak self] save in
						answered()
						if save {
							self?.save(to: url, binary: true)
						}
					}
				}
			}
		}
	}

	/// Saves the fixture with its pending player and venue edits, replacing the destination
	/// atomically; the saved file then becomes the one on show, with no edits pending.
	func save(to url: URL, binary: Bool) {
		guard !refuseWhileBusy(), let source, let fixture else {
			return
		}
		guard FixtureFiles.acceptable(url), FixtureSource.kind(of: url) == (binary ? .binary : .json)
		else {
			report(FixtureFiles.pathProblem)
			return
		}
		guard let venue = readVenue() else {
			return
		}
		let edits = fixture.hasPlayerEdits ? fixture.playerEdits : nil
		fileOperation = true
		log.add("Saving \(url.path)")
		Task {
			let result = await Task.detached(priority: .userInitiated) {
				Result {
					try FixtureFiles.save(
						source,
						playerEdits: edits,
						venue: venue,
						as: binary ? .binary : .json,
						to: url
					)
				}
			}.value
			fileOperation = false
			switch result {
			case .success(let saved):
				written(saved, venue: venue)
				log.add("Saved \(saved.name)")
			case .failure(let failure):
				report(message(failure))
			}
		}
	}

	/// After a save or an export: the file written is the one on show, with no edits pending; a
	/// binary file names no save.
	private func written(_ saved: FixtureSource, venue: [FixtureViewNumber]) {
		fixture?.clearPlayerEdits()
		fixture?.setVenue(venue)
		if saved.kind == .binary {
			fixture?.forgetSave()
		}
		self.venue = VenueForm(venue)
		source = saved
		revision += 1
		noteFile(saved.url)
	}

	// MARK: Exporting

	/// The export link and File > Export: only for a fixture not yet saved, and not during a file
	/// operation; a simulation may run.
	var canExport: Bool {
		isTemporary && !fileOperation && replacement == nil
	}

	/// Exports the fixture not yet saved, with its pending edits, to a new file in the export
	/// folder named after its teams and the time, never replacing a file; it then counts as saved.
	func export() {
		guard isTemporary, let source, let fixture else {
			return
		}
		if fileOperation {
			report(ViewerModel.exportBusyMessage)
			return
		}
		if replacement != nil {
			report(ViewerModel.exportReplacementMessage)
			return
		}
		if isAsking {
			report(ViewerModel.askingMessage)
			return
		}
		guard let name = exportName(fixture, extension: ".json") else {
			report(ViewerModel.exportNameTooLong)
			return
		}
		guard let venue = readVenue() else {
			return
		}
		let destination = settings.exportFolder.appendingPathComponent(name)
		let edits = fixture.hasPlayerEdits ? fixture.playerEdits : nil
		fileOperation = true
		log.add("Exporting \(destination.path)")
		Task {
			let result = await Task.detached(priority: .userInitiated) {
				Result {
					try FixtureFiles.export(source, playerEdits: edits, venue: venue, to: destination)
				}
			}.value
			fileOperation = false
			switch result {
			case .success(let exported):
				written(exported, venue: venue)
				log.add("Exported \(exported.name)")
			case .failure(let failure):
				report(message(failure))
			}
		}
	}

	/// Asks for the export folder with a folder panel.
	func requestExportFolder() {
		guard !isAsking else {
			return
		}
		ask { prompts, answered in
			prompts.chooseExportFolder(current: settings.exportFolder) { [weak self] url in
				answered()
				if let url, let self {
					self.settings.exportFolder = url
					self.log.add("Exports go to \(url.path)")
				}
			}
		}
	}

	/// "<home>-v-<away>-<time><extension>" for the fixture, at the time now.
	private func exportName(_ fixture: FixtureHandle, extension fileExtension: String) -> String? {
		let seconds = now().timeIntervalSince1970
		guard seconds >= 0 else {
			return nil
		}
		return fixture.exportName(time: UInt64(seconds), extension: fileExtension)
	}

	// MARK: The squad lists and player edits

	/// Selects the side's players in these blocks; only players that can be selected, and not while
	/// an operation runs.
	func select(side: Int, blocks: Set<Int>) {
		guard !isBusy, let fixture, selection.indices.contains(side) else {
			return
		}
		let selectable = Set(
			(0..<fixture.rowCount(side: side)).compactMap {
				fixture.rowBlock(side: side, row: $0)
			}
		)
		selection[side] = blocks.intersection(selectable)
	}

	/// The Max buttons need a selected player and no operation running.
	func canEditPlayers(side: Int) -> Bool {
		!isBusy && fixture != nil && selection.indices.contains(side) && !selection[side].isEmpty
	}

	/// Applies a Max button's edit to the side's selected players, or none of them if any lacks the
	/// values it changes. The lists and summary show it at once; the next save writes it.
	func maxPlayers(_ edit: PlayerEdit, side: Int) {
		guard canEditPlayers(side: side), !isAsking, let fixture else {
			return
		}
		let blocks = selection[side]
		if let problem = fixture.editProblem(edit, side: side, blocks: blocks) {
			report(problem)
			return
		}
		if fixture.apply(edit, side: side, blocks: blocks) {
			revision += 1
			let players = blocks.count == 1 ? "player" : "players"
			log.add("\(edit.title): \(blocks.count) \(side == 0 ? "home" : "away") \(players)")
		}
	}

	// MARK: The venue

	/// A venue field can be changed when the fixture stores it (the pitch's width and length only
	/// together) and no operation runs.
	func venueEditable(_ field: Int) -> Bool {
		!isBusy && fixture?.venueEditable(field) == true
	}

	func setVenueText(_ text: String, field: Int) {
		if venueEditable(field) {
			venue.setText(text, field: field)
		}
	}

	func setVenueSurface(_ choice: Int?) {
		if venueEditable(FixtureHandle.surfaceField) {
			venue.setSurface(choice)
		}
	}

	/// The venue values a save writes, from the Venue tab's entries. Nil after showing why they
	/// cannot be saved, with the Venue tab on show and the keyboard focus on the field at fault.
	func readVenue() -> [FixtureViewNumber]? {
		guard let fixture else {
			return nil
		}
		switch venue.values(over: fixture.venue) {
		case .success(let values):
			return values
		case .failure(let problem):
			tab = .venue
			report(problem.message)
			if let field = problem.field {
				focusSerial += 1
				venueFocus = VenueFocus(field: field, serial: focusSerial)
			}
			return nil
		}
	}

	/// The Venue tab has put the focus where `request` asked.
	func focusedVenue(_ request: VenueFocus) {
		if venueFocus == request {
			venueFocus = nil
		}
	}

	// MARK: Replacing teams and players

	/// The pencil, which opens a side's team editor, and the editor's Confirm: a fixture, and no
	/// file operation, replacement or simulation running.
	func canEditTeam(side: Int) -> Bool {
		fixture != nil && (0..<FixtureHandle.sideCount).contains(side) && !isBusy
	}

	/// The team editor's picker and Cancel, which a simulation leaves alone.
	var canCancelTeamEdit: Bool {
		!fileOperation && replacement == nil
	}

	/// Opens the side's team editor with its team's name, closing the other side's, and starts
	/// loading the team list if it has not loaded.
	func editTeam(side: Int) {
		guard canEditTeam(side: side), !isAsking, let fixture else {
			return
		}
		tab = .squads
		load(teams)
		teamEditor = TeamEditor(side: side, team: fixture.teamName(side: side), teams: teams)
	}

	/// Closes the team editor, unless a replacement or file operation needs it to wait.
	func cancelTeamEdit() {
		if canCancelTeamEdit {
			teamEditor = nil
		}
	}

	/// The team editor's Confirm: checks the team and the venue entries, then has the game rebuild
	/// the side in a copy (with the other side's edits) and shows it as a fixture not yet saved.
	func confirmTeam() {
		guard let editor = teamEditor, canEditTeam(side: editor.side) else {
			return
		}
		if isAsking {
			report(ViewerModel.askingMessage)
			return
		}
		guard let team = editor.team else {
			log.error(TeamEditor.problem)
			editor.error = TeamEditor.problem
			editor.requestFocus()
			return
		}
		startReplacement(.team(side: editor.side, team: team))
	}

	/// Swap player: needs exactly one selected player and no operation running.
	func canSwap(side: Int) -> Bool {
		!isBusy && fixture != nil && selection.indices.contains(side) && selection[side].count == 1
	}

	/// Asks for the player to put in the place of the side's selected player, starting to load the
	/// player list if it has not loaded.
	func requestSwap(side: Int) {
		guard canSwap(side: side), !isAsking, let fixture, let block = selection[side].first else {
			return
		}
		load(players)
		swapPrompt = SwapPrompt(
			side: side,
			block: block,
			name: fixture.playerName(side: side, block: block),
			players: players
		)
	}

	func cancelSwap() {
		swapPrompt = nil
	}

	/// The swap prompt's Confirm: once it names a player, the game puts that player in the block of
	/// a copy (with the other edits) and it is shown as a fixture not yet saved.
	func confirmSwap() {
		guard let prompt = swapPrompt else {
			return
		}
		guard let person = prompt.person else {
			log.error(SwapPrompt.problem)
			prompts?.showError(SwapPrompt.problem, title: SwapPrompt.title)
			prompt.requestFocus()
			return
		}
		swapPrompt = nil
		guard fixture != nil else {
			return
		}
		if isBusy {
			report(ViewerModel.busyMessage)
			return
		}
		startReplacement(
			.swap(
				side: prompt.side,
				block: prompt.block,
				person: person,
				allowDuplicate: prompt.allowDuplicates
			)
		)
	}

	/// A pencil or list has taken the focus `request` asked for.
	func focusedSquad(_ request: SquadFocus) {
		if squadFocus == request {
			squadFocus = nil
		}
	}

	/// Checks the venue entries, then runs the job; with `anySave` (Run Anyway), in another save
	/// than the fixture's if FM24 has one loaded.
	private func startReplacement(_ job: ReplacementJob, anySave: Bool = false) {
		guard let source, let fixture else {
			return
		}
		guard let harness else {
			replacementFailed(job, ViewerModel.noHarness)
			return
		}
		guard let venue = readVenue() else {
			return
		}
		let edits = job.carriedEdits(fixture.playerEdits)
		replacement = job
		teamEditor?.error = nil
		switch job {
		case .team(let side, let team):
			activity = ViewerModel.replacingActivity(side: side)
			log.add("Replacing the \(side == 0 ? "home" : "away") team with \(team)")
		case .swap(let side, let block, let person, _):
			activity = ViewerModel.swappingActivity
			log.add("Swapping \(ReplacementJob.block(side: side, block: block)) for player \(person)")
		}
		Task {
			await replace(job, source: source, edits: edits, venue: venue, anySave: anySave, harness)
		}
	}

	private func replace(
		_ job: ReplacementJob,
		source: FixtureSource,
		edits: [UInt8],
		venue: [FixtureViewNumber],
		anySave: Bool,
		_ harness: HarnessService
	) async {
		let input: URL
		let output: URL
		do {
			input = try work.file("in", extension: "json")
			output = try work.file("out", extension: "json")
		} catch {
			finishReplacement(job, .failure(ViewerError(message: message(error))))
			return
		}
		let written = await Task.detached(priority: .userInitiated) {
			Result { try ReplacementFiles.writeInput(source, carrying: edits, to: input) }
		}.value
		if case .failure(let error) = written {
			work.remove(input)
			finishReplacement(job, .failure(ViewerError(message: message(error))))
			return
		}
		let onLine: (HarnessLine) -> Void = { [weak self] line in self?.harnessLine(line) }
		let result: HarnessResult
		switch job {
		case .team(let side, let team):
			result = await harness.replaceTeam(
				side: side,
				input: input,
				team: team,
				output: output,
				anySave: anySave,
				onLine: onLine
			)
		case .swap(let side, let block, let person, let allowDuplicate):
			result = await harness.injectPlayer(
				input: input,
				block: ReplacementJob.block(side: side, block: block),
				person: person,
				output: output,
				allowDuplicate: allowDuplicate,
				anySave: anySave,
				onLine: onLine
			)
		}
		if result.refusedOtherSave, !anySave {
			work.remove(input, output)
			replacement = nil
			activity = nil
			endGameWait()
			askRunAnyway(result) { [weak self] in
				guard let self, !self.refuseWhileBusy() else {
					return
				}
				self.startReplacement(job, anySave: true)
			}
			return
		}
		let fallback = job.isSwap ? ViewerModel.swapFailed : ViewerModel.replaceFailed
		let loaded: Result<(json: Data, handle: FixtureHandle), ViewerError>
		if let problem = problem(result, fallback: fallback, harness) {
			loaded = .failure(ViewerError(message: problem.message, offersLaunch: problem.offersLaunch))
		} else {
			loaded = await Task.detached(priority: .userInitiated) {
				Result { try ReplacementFiles.loadResult(output, venue: venue) }
					.mapError { ($0 as? ViewerError) ?? ViewerError(message: $0.localizedDescription) }
			}.value
		}
		work.remove(input, output)
		finishReplacement(job, loaded.map { (json: $0.json, handle: $0.handle, url: output) })
	}

	/// Shows the result as a fixture not yet saved, whose edits are in it now, or says why there is
	/// none: a team's in red in its editor, a swap's in an error box.
	private func finishReplacement(
		_ job: ReplacementJob,
		_ result: Result<(json: Data, handle: FixtureHandle, url: URL), ViewerError>
	) {
		replacement = nil
		activity = nil
		endGameWait()
		switch result {
		case .success(let (json, handle, url)):
			let side = job.side
			display(
				FixtureSource(url: url, kind: .json, json: json, original: nil, temporary: true),
				handle
			)
			log.add(
				job.isSwap
					? "Swapped the player; the fixture is not saved yet"
					: "Replaced the \(side == 0 ? "home" : "away") team; the fixture is not saved yet"
			)
			if tab == .squads {
				focusSerial += 1
				squadFocus = SquadFocus(
					side: side,
					control: job.isSwap ? .list : .pencil,
					serial: focusSerial
				)
			}
		case .failure(let failure):
			replacementFailed(job, failure.message, offeringLaunch: failure.offersLaunch)
		}
	}

	private func replacementFailed(
		_ job: ReplacementJob,
		_ problem: String,
		offeringLaunch: Bool = false
	) {
		guard !job.isSwap, let editor = teamEditor, editor.side == job.side else {
			let message = job.isSwap ? ReplacementJob.swapProblem(problem) : problem
			report(message, offeringLaunch: offeringLaunch)
			return
		}
		log.error(problem)
		editor.error = problem
		if tab == .squads {
			editor.requestFocus()
		}
	}

	/// Starts loading a list from the game, unless it has loaded or is loading; a failure is
	/// logged, and the next picker to need the list tries again.
	private func load(_ list: ChoiceList) {
		guard let harness else {
			return
		}
		list.load { [weak self] in
			guard let self else {
				return .failure(ViewerError(message: "the viewer has closed"))
			}
			return await self.fetch(list.kind, harness)
		}
	}

	private func fetch(
		_ kind: ChoiceKind,
		_ harness: HarnessService
	) async -> Result<[Choice], ViewerError> {
		let file: URL
		do {
			file = try work.file("list", extension: "txt")
		} catch {
			log.error("cannot list \(kind.name)s: \(message(error))")
			return .failure(ViewerError(message: message(error)))
		}
		let onLine: (HarnessLine) -> Void = { [weak self] line in self?.logLine(line) }
		let result =
			kind == .teams
			? await harness.listTeams(to: file, onLine: onLine)
			: await harness.listPlayers(to: file, onLine: onLine)
		let fallback = kind == .teams ? ViewerModel.listTeamsFailed : ViewerModel.listPlayersFailed
		let parsed: Result<[Choice], ViewerError>
		if let problem = problem(result, fallback: fallback, harness) {
			parsed = .failure(ViewerError(message: problem.message))
		} else {
			parsed = await Task.detached(priority: .userInitiated) {
				ViewerModel.readList(file, kind)
			}.value
		}
		work.remove(file)
		switch parsed {
		case .success(let choices):
			log.add("Listed \(choices.count) \(kind.name)s")
		case .failure(let failure):
			log.error("cannot list \(kind.name)s: \(failure.message)")
		}
		return parsed
	}

	/// The choices in a list file the game wrote.
	private nonisolated static func readList(
		_ file: URL,
		_ kind: ChoiceKind
	) -> Result<[Choice], ViewerError> {
		guard let data = try? Data(contentsOf: file) else {
			return .failure(ViewerError(message: "cannot read the \(kind.name) list"))
		}
		return ChoiceSearch.parse(data, kind)
	}

	/// The game has quit: its lists may not be the next save's.
	private func dropLists() {
		teams.drop()
		players.drop()
	}

	// MARK: Simulating

	/// The Simulation tab's entries can be changed while no simulation runs.
	var simulationEditable: Bool {
		!simulation.running
	}

	/// Simulate fixture: needs a fixture and no operation running.
	var canSimulate: Bool {
		fixture != nil && !isBusy
	}

	/// The Simulation tab's status line: the run, how the last one ended, or where results go.
	var simulationStatus: String {
		if simulation.running {
			return ViewerModel.simulatingStatus
		}
		switch simulation.outcome {
		case .complete:
			return simulation.inExportFolder
				? ViewerModel.completeInExportFolder : ViewerModel.completeBeside
		case .failed:
			return ViewerModel.simulationFailed
		case nil:
			return isTemporary ? ViewerModel.resultsInExportFolder : ViewerModel.resultsBeside
		}
	}

	func setThreads(_ text: String) {
		if simulationEditable {
			simulationForm.setThreads(text)
		}
	}

	func stepThreads(by delta: Int) {
		if simulationEditable {
			simulationForm.stepThreads(by: delta)
		}
	}

	func setSeeds(_ text: String) {
		if simulationEditable {
			simulationForm.setSeeds(text)
		}
	}

	func setDetailed(_ detailed: Bool) {
		if simulationEditable {
			simulationForm.detailed = detailed
		}
	}

	func setKeepBuild(_ keepBuild: Bool) {
		if simulationEditable {
			simulationForm.keepBuild = keepBuild
		}
	}

	/// The Simulation tab has put the focus where `request` asked.
	func focusedSimulation(_ request: SimulationFocus) {
		if simulationFocus == request {
			simulationFocus = nil
		}
	}

	/// Simulate fixture: checks the entries and the venue, as on Windows, then has the game play the
	/// fixture in the viewer's temporary folder (a copy with its pending edits if it has any or
	/// stores a venue value), shows the progress as the results arrive, and moves them beside the
	/// fixture as <name>.csv, or into the export folder under the export's name for a fixture not
	/// yet saved. The fixture's own file is never written. With `anySave` (Run Anyway), a fixture
	/// for another save than the one FM24 has loaded plays in that one.
	func simulate(anySave: Bool = false) {
		guard !refuseWhileBusy(), let source, let fixture else {
			return
		}
		guard let harness else {
			report(ViewerModel.noHarness)
			return
		}
		let options: SimulationOptions
		switch simulationForm.options() {
		case .success(let read):
			options = read
		case .failure(let problem):
			tab = .simulation
			report(problem.message)
			focusSerial += 1
			simulationFocus = SimulationFocus(field: problem.field, serial: focusSerial)
			return
		}
		guard let venue = readVenue() else {
			return
		}
		var exportedName: String?
		if source.temporary {
			guard let name = exportName(fixture, extension: ".csv") else {
				report(ViewerModel.exportNameTooLong)
				return
			}
			exportedName = name
		}
		let run = SimulationRun(
			source: source,
			fixture: fixture,
			options: options,
			venue: venue,
			edits: fixture.hasPlayerEdits ? fixture.playerEdits : nil,
			exportedName: exportedName,
			anySave: anySave
		)
		var progress = FixtureViewProgress()
		simulation = SimulationState(
			summary: FixtureHandle.text { fixtureView_progressText(&progress, $0, $1) },
			running: true,
			total: options.total
		)
		activity = nil
		let seeds = options.total.map { "\(options.seeds) (\($0) matches)" } ?? options.seeds
		log.add(
			"Simulating \(source.temporary ? "the fixture not yet saved" : source.name): seeds \(seeds), "
				+ "\(options.threads) threads"
		)
		let endActivity = beginActivity("Simulating a fixture with FM24")
		Task {
			await simulate(run, harness)
			endActivity()
		}
	}

	/// Shows the last simulation's results in Finder.
	func revealResults() {
		let files = simulation.results.filter { FileManager.default.fileExists(atPath: $0.path) }
		if files.isEmpty {
			report(ViewerModel.resultsMissing)
		} else {
			reveal(files)
		}
	}

	/// What a simulation plays, and where its results go.
	private struct SimulationRun {
		let source: FixtureSource
		let fixture: FixtureHandle
		let options: SimulationOptions
		let venue: [FixtureViewNumber]
		let edits: [UInt8]?
		/// The export's name with .csv, for a fixture not yet saved.
		let exportedName: String?
		/// --any-save.
		let anySave: Bool
	}

	private func simulate(_ run: SimulationRun, _ harness: HarnessService) async {
		let source = run.source
		let input: URL
		let output: URL
		do {
			let inputExtension = SimulationFiles.inputExtension(
				source,
				playerEdits: run.edits,
				venue: run.venue
			)
			input = try work.file("in", extension: inputExtension)
			output = try work.file("run", extension: "csv")
		} catch {
			finishSimulation(failure: message(error))
			return
		}
		let written = await Task.detached(priority: .userInitiated) {
			Result {
				try SimulationFiles.writeInput(source, playerEdits: run.edits, venue: run.venue, to: input)
			}
		}.value
		if case .failure(let error) = written {
			work.remove(input)
			finishSimulation(failure: message(error))
			return
		}
		let progress = RunProgress(url: output, total: run.options.total)
		let interval = UInt64(max(progressInterval, 0.01) * 1_000_000_000)
		let polling = Task { [weak self] in
			while true {
				try? await Task.sleep(nanoseconds: interval)
				guard !Task.isCancelled else {
					return
				}
				self?.update(progress)
			}
		}
		let result = await harness.simulate(
			input,
			options: run.options,
			output: output,
			anySave: run.anySave
		) { [weak self] line in
			self?.harnessLine(line)
		}
		polling.cancel()
		update(progress)
		// Another save loaded: nothing has played, and Run Anyway plays it in that save.
		if result.refusedOtherSave, !run.anySave, progress.completed == 0 {
			work.remove(input)
			for file in [output] + SimulationFiles.reports(output) {
				work.remove(file)
			}
			activity = nil
			endGameWait()
			simulation = SimulationState()
			askRunAnyway(result) { [weak self] in
				self?.simulate(anySave: true)
			}
			return
		}
		let diagnosis = problem(result, fallback: ViewerModel.simulationFailed, harness)
		let problem = diagnosis?.message
		// A fixture not yet saved may have been exported during the run: its results go beside it.
		let shown = fixture === run.fixture ? (self.source ?? source) : source
		let folder = shown.temporary ? settings.exportFolder : nil
		let destination =
			folder.flatMap { folder in run.exportedName.map { folder.appendingPathComponent($0) } }
			?? SimulationFiles.results(beside: shown.url)
		// A failed run's results, as far as it went, as the game leaves them on Windows.
		var moves: [(URL, URL)] = []
		if problem == nil || progress.completed > 0 {
			moves.append((output, destination))
		}
		if problem == nil, run.options.detailed {
			moves += zip(SimulationFiles.reports(output), SimulationFiles.reports(destination))
				.filter { FileManager.default.fileExists(atPath: $0.0.path) }
		}
		let saved = await Task.detached(priority: .userInitiated) {
			ViewerModel.saveResults(moves, creating: folder)
		}.value
		work.remove(input)
		for file in [output] + SimulationFiles.reports(output) where !saved.kept.contains(file) {
			work.remove(file)
		}
		simulation.inExportFolder = folder != nil
		for file in saved.files {
			log.add("Results: \(file.path)")
		}
		let failures = [problem, saved.failure].compactMap { $0 }
		finishSimulation(
			failure: failures.isEmpty ? nil : failures.joined(separator: "\n"),
			results: saved.files,
			offeringLaunch: diagnosis?.offersLaunch == true
		)
	}

	/// Moves a run's files into place, first creating `folder` if one is given: where each file is
	/// now, the work files that could not be moved (kept until the viewer quits), and the first
	/// failure.
	private nonisolated static func saveResults(
		_ moves: [(URL, URL)],
		creating folder: URL?
	) -> (files: [URL], kept: [URL], failure: String?) {
		var files: [URL] = []
		var kept: [URL] = []
		var failure: String?
		if let folder, !moves.isEmpty {
			try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
		}
		for (file, destination) in moves {
			do {
				try SimulationFiles.move(file, to: destination)
				files.append(destination)
			} catch {
				files.append(file)
				kept.append(file)
				let reason = (error as? ViewerError)?.message ?? error.localizedDescription
				failure = failure ?? "\(reason) It is in \(file.path) until FM24 Viewer quits."
			}
		}
		return (files, kept, failure)
	}

	/// Reads the results the game has added, for the summary and progress bar.
	private func update(_ progress: RunProgress) {
		let wasUnreadable = progress.unreadable
		guard progress.read() else {
			return
		}
		if progress.unreadable, !wasUnreadable {
			log.add(
				"\(progress.url.path) has a line that is not a result; the progress bar no longer shows "
					+ "how far the run is."
			)
		}
		if progress.started,
			activity == ViewerModel.waitingActivity || activity == ViewerModel.startingGameActivity
				|| activity == ViewerModel.idleWaitActivity
		{
			activity = nil
			endGameWait()
		}
		simulation.summary = progress.text
		simulation.completed = progress.completed
		simulation.started = progress.started
		simulation.unreadable = progress.unreadable
	}

	private func finishSimulation(
		failure: String?,
		results: [URL] = [],
		offeringLaunch: Bool = false
	) {
		activity = nil
		endGameWait()
		simulation.running = false
		simulation.results = results
		simulation.outcome = failure == nil ? .complete : .failed
		if let failure {
			report(failure, offeringLaunch: offeringLaunch)
		}
	}

	// MARK: Launching FM24 and installing the harness

	/// The start panel's status once Launch FM24 has started the game without a save, or found it
	/// running: it may have none loaded yet, so nothing is exported.
	static var launchedWithoutSave: String {
		"FM24 is running with harness \(FixtureHandle.protocolVersion). Once it has a save loaded and "
			+ "idle, choose Retry to export the next fixture."
	}

	/// File > Launch FM24…, the start panel's button and the error boxes' Launch FM24…: the sheet
	/// that starts FM24 with the harness, offering the saves fm24ctl finds and the last choice.
	func requestLaunch() {
		guard !refuseWhileBusy() else {
			return
		}
		guard harness != nil else {
			report(ViewerModel.noHarness)
			return
		}
		launchPrompt = LaunchPrompt(
			saves: LaunchSaves.read(home: home),
			offersRosetta: appleSilicon,
			remembered: settings.launchSave,
			rosetta: settings.launchRosetta
		)
	}

	/// --launch-save: starts FM24 with `save` (a save's name or its .fm file) at once, showing its
	/// progress in the sheet; `rosetta` only on Apple silicon, as fm24ctl refuses it elsewhere.
	func launch(save: String, rosetta: Bool) {
		guard let harness else {
			status = ViewerModel.noHarness
			log.error(status)
			return
		}
		guard !isAsking, !launchRunning else {
			report(ViewerModel.askingMessage)
			return
		}
		if rosetta, !appleSilicon {
			log.error(ViewerModel.rosettaIgnored)
		}
		let prompt = LaunchPrompt(
			saves: LaunchSaves.read(home: home),
			offersRosetta: appleSilicon,
			named: save,
			rosetta: rosetta
		)
		launchPrompt = prompt
		startLaunch(prompt, harness)
	}

	func chooseLaunch(_ choice: LaunchPrompt.Choice) {
		launchPrompt?.choose(choice)
	}

	func setLaunchRosetta(_ rosetta: Bool) {
		launchPrompt?.setRosetta(rosetta)
	}

	/// The sheet's Other…: a .fm file anywhere, in an Open panel.
	func chooseOtherSave() {
		guard let prompt = launchPrompt, prompt.offersOther else {
			return
		}
		let directory =
			prompt.other?.deletingLastPathComponent()
			?? prompt.saves.folder?.appendingPathComponent("cloud/games", isDirectory: true)
		ask { prompts, answered in
			prompts.chooseSave(directory: directory) { [weak prompt] url in
				answered()
				if let url {
					prompt?.chooseOther(url)
				}
			}
		}
	}

	/// The sheet's Launch FM24: runs `fm24ctl launch` for the choice, which the sheet remembers.
	func confirmLaunch() {
		guard let prompt = launchPrompt, !prompt.isLaunching, !launchRunning, let harness else {
			return
		}
		if let remembered = prompt.remembered, prompt.saves.loadsSaves {
			settings.launchSave = remembered
		}
		if prompt.offersRosetta {
			settings.launchRosetta = prompt.rosetta
		}
		startLaunch(prompt, harness)
	}

	/// The sheet's Cancel, Close or Done: ends a launch that runs, saying that the game may still
	/// be starting, and otherwise closes the sheet.
	func cancelLaunch() {
		guard let prompt = launchPrompt else {
			return
		}
		guard prompt.isLaunching else {
			launchPrompt = nil
			return
		}
		stopLaunch()
		prompt.end(.stopped)
		log.add(ViewerModel.launchStopped)
		keepOnStartPanel(ViewerModel.launchStopped)
	}

	/// The sheet's Install Harness…, while Steam's launch option is not set: closes the sheet, which
	/// keeps File > Install Harness… waiting, and installs the harness instead, as its warning says.
	func installInsteadOfLaunch() {
		guard let prompt = launchPrompt, prompt.offersInstall, !prompt.isLaunching else {
			return
		}
		launchPrompt = nil
		requestInstall()
	}

	private func startLaunch(_ prompt: LaunchPrompt, _ harness: HarnessService) {
		prompt.begin()
		launchRunning = true
		launchSerial += 1
		let serial = launchSerial
		log.add("Launching FM24 \(prompt.destination)\(prompt.rosetta ? ", under Rosetta" : "")")
		launchTask = Task {
			let result = await harness.launch(save: prompt.save, rosetta: prompt.rosetta) { line in
				self.logLine(line)
				if case .output(let text) = line, self.launchSerial == serial {
					prompt.add(text)
				}
			}
			launchRunning = false
			launchTask = nil
			guard serial == launchSerial else {
				log.add("fm24ctl launch ended (status \(result.status))")
				return
			}
			finishLaunch(prompt, result)
		}
	}

	/// With no fixture on show and nothing else under way, the start panel keeps why the launch
	/// ended, with the game's state and Retry: the startup export, unless it retries something else.
	private func keepOnStartPanel(_ reason: String) {
		guard fixture == nil, !isBusy else {
			return
		}
		status = reason
		retry = retry ?? .export
	}

	/// The start panel's reason when the viewer's arguments asked for a launch it cannot make, with
	/// Retry for the startup export, which runs only when asked.
	func showStartProblem(_ problem: String) {
		log.error(problem)
		status = problem
		retry = .export
	}

	/// Ends `fm24ctl launch`, which stops waiting; the game it started goes on.
	private func stopLaunch() {
		launchSerial += 1
		launchTask?.cancel()
		monitor?.refresh()
	}

	/// With no fixture on show, a launch that loaded a save goes on to the startup export; with
	/// one, the sheet says the game is ready. A failure shows fm24ctl's reason in the sheet.
	private func finishLaunch(_ prompt: LaunchPrompt, _ result: HarnessResult) {
		monitor?.refresh()
		guard result.succeeded else {
			let explained =
				result.explanation(fallback: ViewerModel.launchFailed) ?? ViewerModel.launchFailed
			let problem = explained.replacingOccurrences(
				of: ViewerModel.userDataHint,
				with: ViewerModel.launchOptionHint
			)
			log.error(problem)
			prompt.end(.failed(problem))
			keepOnStartPanel(problem)
			return
		}
		let ready = result.output.last { !$0.isEmpty } ?? ""
		log.add("FM24 is ready with the harness")
		guard fixture == nil, !isBusy, launchPrompt === prompt else {
			prompt.end(.succeeded(ready))
			return
		}
		launchPrompt = nil
		if prompt.save != nil {
			exportNextFixture()
		} else {
			status = ViewerModel.launchedWithoutSave
			retry = .export
		}
	}

	/// File > Install Harness…: runs `fm24ctl install` and shows what it reported in a sheet.
	func requestInstall() {
		guard !refuseWhileBusy() else {
			return
		}
		guard let harness else {
			report(ViewerModel.noHarness)
			return
		}
		installState = .installing
		installCopied = false
		log.add("Installing the harness")
		Task {
			let result = await harness.install { line in self.logLine(line) }
			if result.succeeded, let report = InstallReport.parse(result.output) {
				installState = .installed(report)
				log.add("Installed harness \(report.version) in \(report.folder)")
				monitor?.refresh()
				return
			}
			let problem =
				result.succeeded
				? ViewerModel.installUnread
				: result.explanation(fallback: ViewerModel.installFailed) ?? ViewerModel.installFailed
			log.error(problem)
			installState = .failed(problem)
		}
	}

	/// The Install Harness sheet's Copy: Steam's launch option, onto the clipboard.
	func copyLaunchOption() {
		guard case .installed(let report) = installState else {
			return
		}
		copyText(report.launchOption)
		installCopied = true
		log.add("Copied Steam's launch option: \(report.launchOption)")
	}

	/// The Install Harness sheet's Done, once install has finished.
	func closeInstall() {
		if installState != .installing {
			installState = nil
		}
	}

	/// Quitting: the startup export, the first open and `fm24ctl launch` end with the viewer.
	func stopJobs() {
		cancelStart()
		if launchRunning {
			stopLaunch()
		}
	}

	nonisolated static func homeFolder() -> String {
		ProcessInfo.processInfo.environment["HOME"].flatMap { $0.isEmpty ? nil : $0 }
			?? NSHomeDirectory()
	}

	/// Apple silicon, as fm24ctl tells it (saveList_appleSilicon).
	nonisolated static func onAppleSilicon() -> Bool {
		saveList_appleSilicon()
	}

	// MARK: Closing

	/// What closing the window or quitting needs: an unexported temporary fixture closes without
	/// asking, as on Windows, and so does the start panel, stopping the startup export or first open.
	func closeCheck() -> CloseCheck {
		switch busy {
		case .file: return starting != nil ? .close : .blocked(ViewerModel.closeBusyMessage)
		case .replacement: return .blocked(ViewerModel.closeReplacementMessage)
		case .simulation: return .blocked(ViewerModel.closeSimulationMessage)
		case .none: return hasUnsavedEdits ? .confirm : .close
		}
	}

	/// Settles a close or quit: says why it must wait, or asks whether to discard unsaved edits.
	/// `completion` gets true if the window may close.
	func confirmClose(completion: @escaping (Bool) -> Void) {
		switch closeCheck() {
		case .close:
			completion(true)
		case .blocked(let message):
			report(message)
			completion(false)
		case .confirm:
			guard prompts != nil else {
				completion(false)
				return
			}
			ask { prompts, answered in
				prompts.confirm(
					ViewerModel.closeDiscardMessage,
					title: ViewerModel.closeTitle,
					action: ViewerModel.discardAction
				) { [weak self] discard in
					answered()
					guard discard, let self else {
						completion(false)
						return
					}
					// Whatever started while the question waited must finish first.
					if case .blocked(let message) = self.closeCheck() {
						self.report(message)
						completion(false)
						return
					}
					completion(true)
				}
			}
		}
	}

	// MARK: Harness jobs

	/// A line fm24ctl printed, for the log; its notices of waiting for another job, of starting FM24
	/// with the fixture's save and of waiting for the game to be idle with it show as the activity,
	/// for a simulation until its results arrive. While FM24 starts or stays busy, which can take
	/// minutes, each line of fm24ctl's progress shows under it, until the save is ready and the job
	/// goes on.
	private func harnessLine(_ line: HarnessLine) {
		let simulating = simulation.running && !simulation.started
		switch line {
		case .output(let text):
			log.add("fm24ctl: \(text)")
			let shows = activity != nil || simulating
			if text.hasPrefix(HarnessResult.startingGamePrefix), shows {
				beginGameWait(ViewerModel.startingGameActivity, simulating: simulating)
			} else if !waitingForGame, text.hasPrefix(HarnessResult.statePrefix), shows {
				beginGameWait(ViewerModel.idleWaitActivity, simulating: simulating)
				activityDetail = text
			} else if waitingForGame, text.hasPrefix(HarnessResult.readyPrefix) {
				activity = resumedActivity
				endGameWait()
			} else if waitingForGame, !text.isEmpty {
				activityDetail = text
			}
		case .error(let text):
			log.add("fm24ctl: \(text)")
			if text == HarnessResult.waitingNotice, activity != nil || simulating {
				activity = ViewerModel.waitingActivity
			}
		}
	}

	/// fm24ctl waits for the game before the job: `activity` shows meanwhile.
	private func beginGameWait(_ activity: String, simulating: Bool) {
		if !waitingForGame {
			resumedActivity = simulating ? nil : self.activity
		}
		waitingForGame = true
		self.activity = activity
		activityDetail = nil
	}

	/// The game is ready for the job, or the job has ended.
	private func endGameWait() {
		waitingForGame = false
		resumedActivity = nil
		activityDetail = nil
	}

	/// A line of a job that runs beside the others (a list's), for the log only.
	private func logLine(_ line: HarnessLine) {
		switch line {
		case .output(let text), .error(let text):
			log.add("fm24ctl: \(text)")
		}
	}

	/// Why a harness job failed (HarnessResult.diagnosis), or nil; a failure also updates what the
	/// start panel says about the game.
	private func problem(
		_ result: HarnessResult,
		fallback: String,
		_ harness: HarnessService
	) -> HarnessProblem? {
		var serving: Bool?
		let problem = result.diagnosis(fallback: fallback) {
			let check = harness.isServing()
			serving = check
			return check
		}
		if problem != nil {
			monitor?.refresh(serving: serving)
		}
		return problem
	}

	/// fm24ctl refused the job because FM24 has another save loaded than the fixture's: asks, in
	/// fm24ctl's words, whether to run it in that save anyway, which `again` does.
	private func askRunAnyway(_ result: HarnessResult, again: @escaping () -> Void) {
		let refusal =
			result.errors.last { $0.hasSuffix(HarnessResult.anySaveHint + ".") }?
			.trimmingCharacters(in: .whitespaces) ?? ""
		log.error(refusal)
		ask { prompts, answered in
			prompts.confirm(
				ViewerModel.otherSaveQuestion(refusal),
				title: ViewerModel.errorTitle,
				action: ViewerModel.runAnywayAction
			) { [weak self] run in
				answered()
				if run {
					self?.log.add("Running it anyway in the save FM24 has loaded (--any-save)")
					again()
				}
			}
		}
	}

	// MARK: Errors

	/// Shows an error in an error box and logs it.
	func report(_ message: String) {
		log.error(message)
		prompts?.showError(message, title: ViewerModel.errorTitle)
	}

	/// The same, offering Launch FM24… beside OK if `offeringLaunch`: no harness of this protocol
	/// served the job. While Steam's launch option is not set, Install Harness… instead, after
	/// setOptionFirst, as Launch FM24… would start the game directly.
	func report(_ message: String, offeringLaunch: Bool) {
		guard offeringLaunch, prompts != nil else {
			report(message)
			return
		}
		log.error(message)
		let optionSet = launchOptionSet
		let text = optionSet ? message : "\(message)\n\n\(ViewerModel.setOptionFirst)"
		let action = optionSet ? ViewerModel.launchAction : ViewerModel.installAction
		ask { prompts, answered in
			prompts.offerLaunch(text, title: ViewerModel.errorTitle, action: action) {
				[weak self] accepted in
				answered()
				guard accepted, let self else {
					return
				}
				if optionSet {
					self.requestLaunch()
				} else {
					self.requestInstall()
				}
			}
		}
	}

	/// Says why nothing can start, if something runs or waits for the user's answer.
	private func refuseWhileBusy() -> Bool {
		if isBusy {
			report(ViewerModel.busyMessage)
		} else if isAsking {
			report(ViewerModel.askingMessage)
		}
		return isBusy || isAsking
	}

	/// Puts a question or panel to the user, which counts as waiting for an answer (isAsking) until
	/// its completion calls `answered`.
	private func ask(_ put: (ViewerPrompts, @escaping () -> Void) -> Void) {
		guard let prompts else {
			return
		}
		questions += 1
		var open = true
		put(prompts) { [weak self] in
			if open {
				open = false
				self?.questions -= 1
			}
		}
	}

	private func message(_ error: Error) -> String {
		(error as? ViewerError)?.message ?? error.localizedDescription
	}
}
