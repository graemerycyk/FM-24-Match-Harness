// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import Foundation

/// HarnessService through fm24ctl, run as a subprocess: the copy inside the app bundle, which comes
/// from the same build and so speaks the same harness protocol, or the one FM24CTL names. Its
/// standard input is /dev/null. Its output, which it writes a line at a time, is read as it comes,
/// so that a full pipe never holds it up, and again once it has exited, without waiting for the end of the pipes, which a child it
/// started (the game, after launch) may keep open. Cancelling the task that awaits a job ends its
/// fm24ctl (SIGTERM): a job still waiting for the harness never reaches the game.
@MainActor
final class Fm24ctl: HarnessService {
	let executable: URL
	/// Replaces the viewer's own environment for fm24ctl, if set (tests use it).
	var environment: [String: String]?
	/// Told of each run's arguments, for the log.
	var onRun: ([String]) -> Void = { _ in }
	private let serving: () -> Bool

	/// `serving` is the job socket's check.
	init(
		executable: URL,
		environment: [String: String]? = nil,
		serving: @escaping () -> Bool = Fm24ctl.channelServed
	) {
		self.executable = executable
		self.environment = environment
		self.serving = serving
	}

	/// The viewer's fm24ctl: the one FM24CTL names, else Contents/MacOS/fm24ctl in the app bundle.
	/// Nil outside an app bundle, so that a build folder's fm24ctl is never run by mistake.
	nonisolated static func locate(
		environment: [String: String] = ProcessInfo.processInfo.environment,
		bundle: Bundle = .main
	) -> URL? {
		if let path = environment["FM24CTL"], !path.isEmpty {
			return URL(fileURLWithPath: path)
		}
		guard bundle.bundleURL.pathExtension == "app" else {
			return nil
		}
		return bundle.url(forAuxiliaryExecutable: "fm24ctl")
	}

	/// True if a harness of this protocol holds its job socket's lock (channel_served).
	nonisolated static func channelServed() -> Bool {
		var path = [CChar](repeating: 0, count: 1100)
		let size = path.count
		return channel_socketPath(&path, size) && channel_served(path)
	}

	func exportNextFixture(
		to output: URL,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		await run(["get-next-fixture", output.path], onLine: onLine)
	}

	func decode(
		_ input: URL,
		to output: URL,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		await run(["decode", input.path, output.path], onLine: onLine)
	}

	func simulate(
		_ input: URL,
		options: SimulationOptions,
		output: URL,
		anySave: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		var arguments = [
			"run", input.path, options.seeds, output.path, "--threads", String(options.threads),
		]
		if options.detailed {
			arguments.append("--detailed")
		}
		if options.keepBuild {
			arguments.append("--keep-build")
		}
		return await run(arguments + Fm24ctl.anySave(anySave), onLine: onLine)
	}

	func replaceTeam(
		side: Int,
		input: URL,
		team: String,
		output: URL,
		anySave: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		let command = side == 0 ? "replace-home" : "replace-away"
		let arguments = [command, input.path, team, output.path] + Fm24ctl.anySave(anySave)
		return await run(arguments, onLine: onLine)
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
		let arguments = ["inject-player", input.path, block, person, output.path]
		let options = (allowDuplicate ? ["--allow-duplicate"] : []) + Fm24ctl.anySave(anySave)
		return await run(arguments + options, onLine: onLine)
	}

	func listTeams(to output: URL, onLine: @escaping (HarnessLine) -> Void) async -> HarnessResult {
		await run(["list-teams", output.path], onLine: onLine)
	}

	func listPlayers(to output: URL, onLine: @escaping (HarnessLine) -> Void) async -> HarnessResult {
		await run(["list-players", output.path], onLine: onLine)
	}

	func launch(
		save: String?,
		rosetta: Bool,
		onLine: @escaping (HarnessLine) -> Void
	) async -> HarnessResult {
		let arguments =
			["launch"] + (save.map { ["--save", $0] } ?? []) + (rosetta ? ["--rosetta"] : [])
		return await run(arguments, onLine: onLine)
	}

	func install(onLine: @escaping (HarnessLine) -> Void) async -> HarnessResult {
		await run(["install"], onLine: onLine)
	}

	private static func anySave(_ anySave: Bool) -> [String] {
		anySave ? ["--any-save"] : []
	}

	func isServing() -> Bool {
		serving()
	}

	/// Runs fm24ctl with `arguments` and returns how it ended.
	func run(_ arguments: [String], onLine: @escaping (HarnessLine) -> Void) async -> HarnessResult {
		onRun(arguments)
		let process = Process()
		process.executableURL = executable
		process.arguments = arguments
		if let environment {
			process.environment = environment
		}
		process.standardInput = FileHandle.nullDevice
		let output = ProcessOutput(onLine: onLine)
		return await withTaskCancellationHandler {
			await withCheckedContinuation { continuation in
				output.run(process) { result in
					continuation.resume(returning: result)
				}
			}
		} onCancel: {
			output.cancel()
		}
	}
}

/// One fm24ctl run's standard output and error, read without blocking on a queue of their own,
/// which also handles the exit, so that the last read comes after every earlier one.
private final class ProcessOutput: @unchecked Sendable {
	/// Each stream keeps its first line and up to this many of its last.
	private static let lineLimit = 10_000

	private let queue = DispatchQueue(
		label: "io.github.graemerycyk.fm24-match-harness.viewer.fm24ctl"
	)
	private let onLine: (HarnessLine) -> Void
	private let pipes = [Pipe(), Pipe()]
	// Used on `queue` only, once the run has started. A stream's descriptor is -1 once it has ended.
	private var descriptors: [Int32]
	private var sources: [DispatchSourceRead?] = [nil, nil]
	private var pending = [Data(), Data()]
	private var lines: [[String]] = [[], []]
	// Used under `lock`: the process once it has started, and whether the run was cancelled.
	private let lock = NSLock()
	private var started: Process?
	private var cancelled = false

	init(onLine: @escaping (HarnessLine) -> Void) {
		self.onLine = onLine
		descriptors = pipes.map { $0.fileHandleForReading.fileDescriptor }
	}

	/// Starts `process` with its output read here; `completion` runs on the main thread.
	func run(_ process: Process, completion: @escaping (HarnessResult) -> Void) {
		process.standardOutput = pipes[0]
		process.standardError = pipes[1]
		process.terminationHandler = { [self] ended in
			let status =
				ended.terminationReason == .exit ? ended.terminationStatus : 128 + ended.terminationStatus
			queue.async { self.finish(status, completion) }
		}
		queue.sync {
			for stream in 0..<2 {
				let descriptor = descriptors[stream]
				_ = fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK)
				let source = DispatchSource.makeReadSource(fileDescriptor: descriptor, queue: queue)
				source.setEventHandler { [self] in read(stream) }
				// The pipe, which closes the descriptor, outlives the source.
				source.setCancelHandler { [pipe = pipes[stream]] in withExtendedLifetime(pipe) {} }
				source.resume()
				sources[stream] = source
			}
		}
		do {
			try process.run()
			lock.lock()
			started = process
			let stop = cancelled
			lock.unlock()
			if stop {
				process.terminate()
			}
		} catch {
			queue.async {
				self.stop()
				self.deliver(
					HarnessResult(
						status: -1,
						errors: ["cannot run fm24ctl (\(error.localizedDescription))"],
						started: false
					),
					completion
				)
			}
		}
	}

	/// Ends the process, now or as soon as it has started.
	func cancel() {
		lock.lock()
		defer { lock.unlock() }
		cancelled = true
		if let started, started.isRunning {
			started.terminate()
		}
	}

	/// Reads what `stream` has, without waiting.
	private func read(_ stream: Int) {
		let descriptor = descriptors[stream]
		guard descriptor >= 0 else {
			return
		}
		var buffer = [UInt8](repeating: 0, count: 65_536)
		while true {
			let count = buffer.withUnsafeMutableBytes {
				Darwin.read(descriptor, $0.baseAddress, $0.count)
			}
			if count > 0 {
				append(Data(buffer[0..<count]), stream)
			} else if count == 0 {
				end(stream)
				return
			} else if errno != EINTR {
				return
			}
		}
	}

	private func append(_ data: Data, _ stream: Int) {
		pending[stream].append(data)
		while let newline = pending[stream].firstIndex(of: 0x0A) {
			var line = pending[stream][pending[stream].startIndex..<newline]
			if line.last == 0x0D {
				line = line.dropLast()
			}
			pending[stream] = pending[stream][(newline + 1)...]
			add(String(decoding: line, as: UTF8.self), stream)
		}
	}

	private func add(_ text: String, _ stream: Int) {
		lines[stream].append(text)
		if lines[stream].count >= 2 * ProcessOutput.lineLimit {
			lines[stream].removeSubrange(1...ProcessOutput.lineLimit)
		}
		let line = stream == 0 ? HarnessLine.output(text) : .error(text)
		let onLine = self.onLine
		DispatchQueue.main.async { onLine(line) }
	}

	/// The end of a stream: what is left is its last line.
	private func end(_ stream: Int) {
		if !pending[stream].isEmpty {
			let rest = pending[stream]
			pending[stream] = Data()
			add(String(decoding: rest, as: UTF8.self), stream)
		}
		sources[stream]?.cancel()
		sources[stream] = nil
		descriptors[stream] = -1
	}

	private func finish(_ status: Int32, _ completion: @escaping (HarnessResult) -> Void) {
		stop()
		deliver(HarnessResult(status: status, output: lines[0], errors: lines[1]), completion)
	}

	/// Reads what has been written, then stops reading.
	private func stop() {
		for stream in 0..<2 where descriptors[stream] >= 0 {
			read(stream)
			if descriptors[stream] >= 0 {
				end(stream)
			}
		}
	}

	private func deliver(_ result: HarnessResult, _ completion: @escaping (HarnessResult) -> Void) {
		DispatchQueue.main.async { completion(result) }
	}
}
