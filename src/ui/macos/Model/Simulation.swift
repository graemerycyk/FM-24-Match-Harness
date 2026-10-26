// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import Foundation

/// What `fm24ctl run` is given for a simulation, read from the Simulation tab's entries.
struct SimulationOptions: Equatable {
	var threads: Int
	/// The seed pattern, "keep" (the fixture's own seed) for a blank one.
	var seeds: String
	/// --detailed: the per-match averages beside the results.
	var detailed: Bool
	/// --keep-build: plays the fixture as it is, with only its seed changed.
	var keepBuild: Bool
	/// How many matches the pattern plays, or nil if the harness would refuse it.
	var total: Int?
}

/// The Simulation tab's entries, as the Windows form holds them: the thread count, the seed
/// pattern and the two options. They stay as they are when another fixture opens.
struct SimulationForm: Equatable {
	enum Field: Equatable {
		case threads
		case seeds
	}

	/// Why the entries cannot be simulated, and the field at fault.
	struct Problem: Error, Equatable {
		let field: Field
		let message: String
	}

	/// The most characters the fields take, as on Windows.
	static let threadsLimit = 15
	static let seedsLimit = 255
	/// The most threads, which is also the default: one per logical processor less one for the
	/// game, from 1 to 16 (commands_defaultThreads).
	static let threadLimit = Int(fixtureView_threadLimit())
	static let seedsPlaceholder = "e.g. 1,4,7-70"
	static var threadsLabel: String {
		"Number of threads (1-\(threadLimit)):"
	}
	static var threadsProblem: String {
		"Number of threads must be between 1 and \(threadLimit)."
	}
	static let seedsProblem =
		"Enter a seed pattern such as 1, 42, 1-1000, 1,3,5 or 1,4,7-70; or leave blank to use the "
		+ "fixture's seed."

	private(set) var threads = String(SimulationForm.threadLimit)
	private(set) var seeds = ""
	var detailed = false
	/// "Skip rebuild", on by default: it plays any fixture, even one the game has moved on from.
	var keepBuild = true

	mutating func setThreads(_ text: String) {
		threads = VenueForm.limited(text, to: SimulationForm.threadsLimit)
	}

	mutating func setSeeds(_ text: String) {
		seeds = VenueForm.limited(text, to: SimulationForm.seedsLimit)
	}

	/// The thread count's stepper: the count in the field, kept within 1 and the limit (the limit if
	/// the field holds no number), moved by `delta`.
	mutating func stepThreads(by delta: Int) {
		let limit = SimulationForm.threadLimit
		let current = Int(threads).map { min(max($0, 1), limit) } ?? limit
		threads = String(min(max(current + delta, 1), limit))
	}

	/// The options the entries give, or why they cannot be simulated (the Windows checks, in order).
	func options() -> Result<SimulationOptions, Problem> {
		var count: UInt = 0
		guard fixtureView_threadCount(SimulationForm.cText(threads), &count) else {
			return .failure(Problem(field: .threads, message: SimulationForm.threadsProblem))
		}
		let pattern = SimulationForm.cText(seeds)
		guard fixtureView_seedPattern(pattern) else {
			return .failure(Problem(field: .seeds, message: SimulationForm.seedsProblem))
		}
		var total = 0
		let counted = fixtureView_seedCount(pattern, &total)
		return .success(
			SimulationOptions(
				threads: Int(count),
				seeds: seeds.isEmpty ? "keep" : seeds,
				detailed: detailed,
				keepBuild: keepBuild,
				total: counted ? total : nil
			)
		)
	}

	/// Text for C, which ends at a NUL: one that holds a NUL is refused as text no check accepts.
	private static func cText(_ text: String) -> String {
		text.utf8.contains(0) ? "\u{1}" : text
	}
}

/// A run's progress, read from the results CSV the game writes as it plays. It writes the rows in
/// seed order as they complete, so the count can trail the run by up to threads − 1 matches.
final class RunProgress {
	/// Text longer than this without a line break is not a results row.
	private static let pendingLimit = 65_536

	let url: URL
	let total: Int?
	private var counts = FixtureViewProgress()
	private var file: FileHandle?
	private var pending = Data()

	init(url: URL, total: Int?) {
		self.url = url
		self.total = total
	}

	deinit {
		try? file?.close()
	}

	/// Matches read so far.
	var completed: Int {
		Int(counts.completed)
	}

	/// The header has been read: the game has started writing results.
	var started: Bool {
		counts.header
	}

	/// A line was not a results row, so the count may be short.
	var unreadable: Bool {
		counts.failed
	}

	/// "n = <completed>: home <wins> (<percent>%) / draw … / away …".
	var text: String {
		var counts = self.counts
		return FixtureHandle.text { fixtureView_progressText(&counts, $0, $1) }
	}

	/// Reads the lines the game has added since the last read; false if it has added none.
	@discardableResult
	func read() -> Bool {
		if file == nil {
			file = try? FileHandle(forReadingFrom: url)
		}
		guard let file, let data = try? file.readToEnd(), !data.isEmpty else {
			return false
		}
		pending.append(data)
		var counts = self.counts
		let used = pending.withUnsafeBytes { raw in
			fixtureView_readProgress(&counts, raw.bindMemory(to: CChar.self).baseAddress, raw.count)
		}
		pending.removeFirst(used)
		if pending.count > RunProgress.pendingLimit {
			counts.failed = true
			pending.removeAll()
		}
		self.counts = counts
		return true
	}
}

/// The Simulation tab's state besides its entries: the run's summary and progress, how the last
/// run ended and where its results are.
struct SimulationState: Equatable {
	enum Outcome: Equatable {
		case complete
		case failed
	}

	/// "n = …: home … / draw … / away …", from the results read so far; "" before a run.
	var summary = ""
	var running = false
	var outcome: Outcome?
	/// Matches read from the results, and how many the run plays (nil if not known).
	var completed = 0
	var total: Int?
	/// Results have begun to arrive.
	var started = false
	/// A line of the results was not a result.
	var unreadable = false
	/// Where the last run's results are, the results CSV first.
	var results: [URL] = []
	/// The last run's results went to the export folder: the fixture was not saved.
	var inExportFolder = false

	/// How far the run is, from 0 to 1; nil until results arrive, without a total, or after a line
	/// that is not a result, when the progress bar cannot say.
	var fraction: Double? {
		guard running, started, !unreadable, let total, total > 0 else {
			return nil
		}
		return min(1, Double(completed) / Double(total))
	}

	/// The Dock icon's badge while a run is running: the percentage played, or "…" while that is
	/// not known.
	var badge: String? {
		guard running else {
			return nil
		}
		return fraction.map { "\(Int(($0 * 100).rounded(.down)))%" } ?? "…"
	}
}

/// The files of a simulation: the fixture the game plays, and its results.
enum SimulationFiles {
	/// The detailed reports, written beside the results as "<results without .csv><suffix>"
	/// (match_stats.h).
	static let reportSuffixes = ["-players.csv", "-teams.csv"]

	/// Whether the game plays an edited copy, as on Windows when the fixture has player edits or
	/// stores a venue value (whose derived values are then recomputed); otherwise it plays the
	/// fixture's own bytes.
	static func playsEdits(_ edits: [UInt8]?, venue: [FixtureViewNumber]) -> Bool {
		edits?.contains { $0 != 0 } == true || FixtureHandle.hasVenue(venue)
	}

	/// The extension of the copy the game plays: JSON for an edited copy, else the fixture's own.
	static func inputExtension(
		_ source: FixtureSource,
		playerEdits edits: [UInt8]?,
		venue: [FixtureViewNumber]
	) -> String {
		playsEdits(edits, venue: venue) || source.kind == .json ? "json" : "bin"
	}

	/// Writes the fixture the game plays to `url`, in the viewer's temporary folder, never touching
	/// the fixture's own file.
	static func writeInput(
		_ source: FixtureSource,
		playerEdits edits: [UInt8]?,
		venue: [FixtureViewNumber],
		to url: URL
	) throws {
		let bytes: Data
		if playsEdits(edits, venue: venue) {
			let changed = edits?.contains { $0 != 0 } == true ? edits : nil
			bytes = try FixtureHandle.write(
				json: source.json,
				playerEdits: changed,
				venue: venue,
				binary: false
			)
		} else {
			bytes = source.kind == .binary ? source.original ?? source.json : source.json
		}
		do {
			try bytes.write(to: url, options: .withoutOverwriting)
		} catch {
			throw ViewerError(
				message: "Cannot write the fixture to simulate in the viewer's temporary folder."
			)
		}
	}

	/// The results' file for a fixture: its own name with .csv, beside it.
	static func results(beside fixture: URL) -> URL {
		fixture.deletingPathExtension().appendingPathExtension("csv")
	}

	/// The detailed reports beside a results file.
	static func reports(_ results: URL) -> [URL] {
		let stem = results.deletingPathExtension().lastPathComponent
		let folder = results.deletingLastPathComponent()
		return reportSuffixes.map { folder.appendingPathComponent(stem + $0) }
	}

	/// Moves a file the game wrote to `destination`, replacing a file there, through a copy beside
	/// it when the two are on different volumes.
	static func move(_ file: URL, to destination: URL) throws {
		func failure(_ reason: String) -> ViewerError {
			ViewerError(message: "Cannot save the results as \(destination.path) (\(reason)).")
		}
		func failure(_ error: Int32) -> ViewerError {
			failure(String(cString: strerror(error)))
		}
		if rename(file.path, destination.path) == 0 {
			return
		}
		guard errno == EXDEV else {
			throw failure(errno)
		}
		let copy = destination.deletingLastPathComponent()
			.appendingPathComponent(".\(destination.lastPathComponent).\(UUID().uuidString).tmp")
		do {
			try FileManager.default.copyItem(at: file, to: copy)
		} catch {
			try? FileManager.default.removeItem(at: copy)
			throw failure(error.localizedDescription)
		}
		if rename(copy.path, destination.path) != 0 {
			let error = errno
			try? FileManager.default.removeItem(at: copy)
			throw failure(error)
		}
		try? FileManager.default.removeItem(at: file)
	}
}
