// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureSamples
import Foundation

/// The viewer tests' harness, reporting as tests/test.c does: "[PASS] name" or "[FAIL] name", then
/// a count. Failed checks print where they are and carry on.
@MainActor
enum Tests {
	private static var run = 0
	private static var failed = 0
	private static var failing = false

	static func test(_ name: String, _ body: () throws -> Void) {
		run += 1
		failing = false
		do {
			try body()
		} catch {
			fail("threw \(error)", file: #fileID, line: #line)
		}
		if failing {
			failed += 1
		}
		print("[\(failing ? "FAIL" : "PASS")] \(name)")
	}

	static func fail(_ message: String, file: StaticString, line: UInt) {
		failing = true
		FileHandle.standardError.write(Data("\(file):\(line): \(message)\n".utf8))
	}

	/// The exit status: failure if any test failed, or none ran.
	static func finish() -> Int32 {
		print("\(run) tests, \(failed) failed")
		return run == 0 || failed > 0 ? 1 : 0
	}
}

@MainActor
func check(
	_ condition: Bool,
	_ message: @autoclosure () -> String = "check failed",
	file: StaticString = #fileID,
	line: UInt = #line
) {
	if !condition {
		Tests.fail(message(), file: file, line: line)
	}
}

@MainActor
func checkEqual<Value: Equatable>(
	_ actual: Value,
	_ expected: Value,
	file: StaticString = #fileID,
	line: UInt = #line
) {
	if actual != expected {
		Tests.fail("expected \(expected), got \(actual)", file: file, line: line)
	}
}

/// Runs the main run loop, which finishes the model's background work, until `condition` holds;
/// false if it did not within `seconds`.
@MainActor
func waitUntil(seconds: TimeInterval = 10, _ condition: () -> Bool) -> Bool {
	let deadline = Date().addingTimeInterval(seconds)
	while !condition() {
		if Date() > deadline {
			return false
		}
		RunLoop.main.run(until: Date().addingTimeInterval(0.01))
	}
	return true
}

/// A sample fixture's JSON (tests/fixture_samples.c).
func sampleJSON(_ sample: FixtureSample) -> Data {
	var length = 0
	guard let text = fixtureSamples_json(Int32(sample.rawValue), &length) else {
		fatalError("cannot make sample \(sample.rawValue)")
	}
	defer { free(text) }
	return Data(bytes: text, count: length)
}

/// A sample with the "save" and "game_date" that exports of the macOS harness record
/// (fixtureSamples_savedJson).
func savedSampleJSON(_ sample: FixtureSample) -> Data {
	var length = 0
	guard let text = fixtureSamples_savedJson(Int32(sample.rawValue), &length) else {
		fatalError("cannot make saved sample \(sample.rawValue)")
	}
	defer { free(text) }
	return Data(bytes: text, count: length)
}

/// The labelled sample without its pitch length, which the venue checks refuse: it stores no
/// venue value, so it is written as it is. With `saved`, it has a save and in-game date.
func plainSampleJSON(saved: Bool = false) -> Data {
	replacing(
		saved ? savedSampleJSON(FIXTURE_SAMPLE_LABELLED) : sampleJSON(FIXTURE_SAMPLE_LABELLED),
		"{\"name\":\"venue\",\"u32\":0},",
		with: "{\"u32\":0},"
	)
}

/// A fixture's "save" and "game_date", by which fm24ctl checks that the game has the fixture's
/// save loaded before a job; nil for a key it lacks.
struct SaveOrigin: Equatable {
	/// The saved samples'.
	static let sample = SaveOrigin(save: FIXTURE_SAMPLE_SAVE, date: FIXTURE_SAMPLE_GAME_DATE)

	var save: String?
	var date: String?

	init(save: String?, date: String?) {
		self.save = save
		self.date = date
	}

	/// The keys of the JSON `json`, if there is any.
	init(_ json: Data?) {
		let object = json.flatMap { try? JSONSerialization.jsonObject(with: $0) } as? [String: Any]
		save = object?["save"] as? String
		date = object?["game_date"] as? String
	}
}

/// `data` with the first `from` replaced by `to`.
func replacing(_ data: Data, _ from: String, with to: String) -> Data {
	let text = String(decoding: data, as: UTF8.self)
	guard let range = text.range(of: from) else {
		fatalError("\(from) is not in the sample")
	}
	return Data(text.replacingCharacters(in: range, with: to).utf8)
}

/// A folder of its own for a test's files, removed when the tests end.
final class Scratch {
	let url: URL

	init() {
		url = FileManager.default.temporaryDirectory
			.appendingPathComponent("fm24-viewer-tests-\(getpid())-\(UUID().uuidString)")
		try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
	}

	deinit {
		try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: url.path)
		try? FileManager.default.removeItem(at: url)
	}

	func file(_ name: String, _ contents: Data? = nil) -> URL {
		let file = url.appendingPathComponent(name)
		if let contents {
			try? contents.write(to: file)
		}
		return file
	}

	/// The files in a folder of the scratch folder (or in it), by name.
	func names(_ folder: String = "") -> [String] {
		let path = folder.isEmpty ? url.path : url.appendingPathComponent(folder).path
		return ((try? FileManager.default.contentsOfDirectory(atPath: path)) ?? []).sorted()
	}
}

/// Settings kept in memory, so that tests never write the user's preferences.
final class MemorySettings: SettingsStore {
	var values: [String: Any] = [:]

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

/// fm24ctl's stand-in (tests/viewer_stand_in_fm24ctl.c), which never contacts the game, from
/// FM24CTL as ctest sets it, with a log of its runs. Nil, failing the test, if FM24CTL names
/// anything else: the real fm24ctl must never run here.
@MainActor
final class StandIn {
	static let name = "fm24_stand_in_fm24ctl"

	let harness: Fm24ctl
	/// The home folder the stand-in is given (HOME), whose paths install and launch print.
	var home: String?
	private let log: URL

	init?(_ scratch: Scratch, serving: @escaping () -> Bool = { true }) {
		let path = ProcessInfo.processInfo.environment["FM24CTL"] ?? ""
		guard URL(fileURLWithPath: path).lastPathComponent == StandIn.name else {
			check(false, "FM24CTL must name \(StandIn.name), not \(path.isEmpty ? "nothing" : path)")
			return nil
		}
		log = scratch.file("stand-in.log")
		harness = Fm24ctl(executable: URL(fileURLWithPath: path), serving: serving)
		set([:])
	}

	/// What the stand-in does next (FM24_STAND_IN_ without the prefix: STATUS, STDOUT and so on).
	func set(_ settings: [String: String]) {
		var environment = ProcessInfo.processInfo.environment.filter {
			!$0.key.hasPrefix("FM24_STAND_IN_")
		}
		environment["FM24_STAND_IN_LOG"] = log.path
		if let home {
			environment["HOME"] = home
		}
		for (key, value) in settings {
			environment["FM24_STAND_IN_\(key)"] = value
		}
		harness.environment = environment
	}

	/// Each run's arguments, in turn.
	var runs: [[String]] {
		let text = (try? String(contentsOf: log, encoding: .utf8)) ?? ""
		return text.split(separator: "\n", omittingEmptySubsequences: false).dropLast().map {
			$0.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
		}
	}
}

/// A model whose harness is the stand-in, with its own temporary directory (T in the scratch
/// folder), settings, export folder (Exports) and game.
@MainActor
struct HarnessSetup {
	let scratch: Scratch
	let model: ViewerModel
	let prompts: FakePrompts
	let standIn: StandIn
	let monitor: GameMonitor
	let settings: ViewerSettings
	let activities = Activities()
	var temporary: URL {
		scratch.url.appendingPathComponent("T")
	}
	/// The home folder whose save folders the Launch FM24 sheet lists.
	var home: URL {
		scratch.url.appendingPathComponent("Home")
	}
	var exports: URL {
		scratch.url.appendingPathComponent("Exports")
	}

	init?(serving: Bool = true) {
		let scratch = Scratch()
		guard let standIn = StandIn(scratch, serving: { serving }) else {
			return nil
		}
		self.scratch = scratch
		self.standIn = standIn
		prompts = FakePrompts()
		let homePath = scratch.url.appendingPathComponent("Home").path
		monitor = GameMonitor(
			isRunning: { false },
			isServing: { serving },
			launchOptionSet: { LaunchSaves.launchOptionSet(home: homePath) }
		)
		let temporary = scratch.url.appendingPathComponent("T")
		try? FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
		let settings = ViewerSettings(defaults: MemorySettings(), documents: scratch.url)
		settings.exportFolder = scratch.url.appendingPathComponent("Exports")
		self.settings = settings
		let home = scratch.url.appendingPathComponent("Home")
		try? FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
		standIn.home = home.path
		standIn.set([:])
		model = ViewerModel(
			log: ViewerLog(echo: false),
			prompts: prompts,
			harness: standIn.harness,
			work: WorkFiles(directory: temporary),
			settings: settings,
			monitor: monitor
		)
		model.now = { Date(timeIntervalSince1970: 1_700_000_000) }
		model.progressInterval = 0.02
		model.home = home.path
		model.appleSilicon = true
		let activities = self.activities
		model.beginActivity = { reason in
			activities.begun.append(reason)
			return { activities.ended += 1 }
		}
	}

	/// Each path the stand-in was given is in the temporary directory.
	func checkTemporaryOnly() {
		let inside = temporary.resolvingSymlinksInPath().path + "/"
		for run in standIn.runs {
			for argument in run.dropFirst() where argument.hasPrefix("/") {
				check(
					URL(fileURLWithPath: argument).resolvingSymlinksInPath().path.hasPrefix(inside),
					"\(argument) is outside the temporary directory"
				)
			}
		}
	}
}

/// The activities that keep the Mac awake during a simulation, as the model began and ended them.
@MainActor
final class Activities {
	var begun: [String] = []
	var ended = 0
}

/// Scripted answers to the viewer's prompts, and a record of what it asked.
@MainActor
final class FakePrompts: ViewerPrompts {
	var errors: [(title: String, message: String)] = []
	/// The answers to questions, in turn: true for their action; Cancel once they run out.
	var answers: [Bool] = []
	var questions: [(title: String, message: String, action: String)] = []
	var fixtureToOpen: URL?
	var destination: (url: URL, binary: Bool)?
	var offered: (name: String, directory: URL?, binary: Bool)?
	var folderToChoose: URL?
	var offeredFolder: URL?
	/// The .fm file the save panel chooses, and the folder it was offered.
	var saveToChoose: URL?
	var offeredSaveFolder: URL?
	/// The error boxes that offered Launch FM24… (or Install Harness…), and the answers to them,
	/// in turn: true for that action; OK once they run out.
	var launchOffers: [(title: String, message: String, action: String)] = []
	var launchAnswers: [Bool] = []
	/// Questions and the Open panel wait, as sheets do, for `answer` and `choose`.
	var deferring = false
	private(set) var waitingAnswers: [(Bool) -> Void] = []
	private(set) var waitingFixtures: [(URL?) -> Void] = []

	func showError(_ message: String, title: String) {
		errors.append((title, message))
	}

	func offerLaunch(
		_ message: String,
		title: String,
		action: String,
		completion: @escaping (Bool) -> Void
	) {
		launchOffers.append((title, message, action))
		completion(launchAnswers.isEmpty ? false : launchAnswers.removeFirst())
	}

	func chooseSave(directory: URL?, completion: @escaping (URL?) -> Void) {
		offeredSaveFolder = directory
		completion(saveToChoose)
	}

	/// Answers the first question waiting.
	func answer(_ action: Bool, file: StaticString = #fileID, line: UInt = #line) {
		guard !waitingAnswers.isEmpty else {
			Tests.fail("no question is waiting", file: file, line: line)
			return
		}
		waitingAnswers.removeFirst()(action)
	}

	/// Closes the first Open panel waiting, with `url` chosen.
	func choose(_ url: URL?, file: StaticString = #fileID, line: UInt = #line) {
		guard !waitingFixtures.isEmpty else {
			Tests.fail("no Open panel is waiting", file: file, line: line)
			return
		}
		waitingFixtures.removeFirst()(url)
	}

	func confirm(
		_ message: String,
		title: String,
		action: String,
		completion: @escaping (Bool) -> Void
	) {
		questions.append((title, message, action))
		if deferring {
			waitingAnswers.append(completion)
		} else {
			completion(answers.isEmpty ? false : answers.removeFirst())
		}
	}

	func chooseFixture(completion: @escaping (URL?) -> Void) {
		if deferring {
			waitingFixtures.append(completion)
		} else {
			completion(fixtureToOpen)
		}
	}

	func chooseDestination(
		name: String,
		directory: URL?,
		binary: Bool,
		completion: @escaping (URL?, Bool) -> Void
	) {
		offered = (name, directory, binary)
		completion(destination?.url, destination?.binary ?? false)
	}

	func chooseExportFolder(current: URL, completion: @escaping (URL?) -> Void) {
		offeredFolder = current
		completion(folderToChoose)
	}

	var lastError: String? {
		errors.last?.message
	}
}
