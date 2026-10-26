// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Combine
import Foundation

/// Whether FM24 runs, and with this protocol's harness.
enum GameState: Equatable {
	/// Not checked yet.
	case unknown
	case notRunning
	/// Running without the harness, or with another version of it.
	case runningWithoutHarness
	case ready

	init(running: Bool, serving: Bool) {
		self = serving ? .ready : running ? .runningWithoutHarness : .notRunning
	}

	/// The start panel's line about the game; nil before it has been checked. While Steam's launch
	/// option is not set (`optionSet`), it leads with setting it, as Launch FM24… would then start
	/// the game directly, which has not been verified.
	func description(optionSet: Bool) -> String? {
		let harness = "harness \(FixtureHandle.protocolVersion)"
		switch self {
		case .unknown: return nil
		case .notRunning:
			return optionSet
				? "FM24 is not running. Choose Launch FM24… to start it with the harness and a save, or "
					+ "start it from Steam with the harness and load a save, then choose Retry."
				: "FM24 is not running. Set Steam's launch option for it once (File > Install Harness…), "
					+ "then start it from Steam, load a save and choose Retry. \(GameState.directStart)"
		case .runningWithoutHarness:
			return optionSet
				? "FM24 is running without \(harness): quit it, then choose Launch FM24… to start it "
					+ "with the harness."
				: "FM24 is running without \(harness): quit it, set Steam's launch option for it once "
					+ "(File > Install Harness…), then start it from Steam. \(GameState.directStart)"
		case .ready: return "FM24 is running with \(harness)."
		}
	}

	/// What Launch FM24… does while Steam's launch option is not set.
	static let directStart =
		"Launch FM24… would open it without the option, which has not been verified: the game "
		+ "crashed in its one attempt."
}

/// Follows the game: the app tells it when FM24 starts or quits (App/GameWatch.swift), and it
/// checks the job socket's lock (channel_served, which sends the game nothing) only then, when a
/// harness job fails, and for a while after the game starts, until its harness serves.
@MainActor
final class GameMonitor: ObservableObject {
	nonisolated static let bundleIdentifier = "com.sigames.eur.FootballManager2024"

	@Published private(set) var state = GameState.unknown
	/// Steam's launch option for FM24 runs the harness's launcher, as Steam has saved it; read with
	/// the game's state.
	@Published private(set) var optionSet = true
	/// Told when the game quits.
	var onQuit: () -> Void = {}
	/// Sends when the game quits, to every subscriber.
	let quit = PassthroughSubject<Void, Never>()

	private let isRunning: () -> Bool
	private let isServing: () -> Bool
	private let launchOptionSet: () -> Bool
	private let interval: TimeInterval
	private let checks: Int
	private var watch: Task<Void, Never>?

	/// After the game starts, its harness is looked for every `interval` seconds, `checks` times.
	/// `launchOptionSet` reads Steam's launch option (LaunchSaves.launchOptionSet).
	init(
		isRunning: @escaping () -> Bool,
		isServing: @escaping () -> Bool,
		launchOptionSet: @escaping () -> Bool,
		interval: TimeInterval = 2,
		checks: Int = 90
	) {
		self.isRunning = isRunning
		self.isServing = isServing
		self.launchOptionSet = launchOptionSet
		self.interval = interval
		self.checks = checks
	}

	/// The start panel's line about the game (GameState.description).
	var description: String? {
		state.description(optionSet: optionSet)
	}

	/// Checks the game and Steam's launch option now; `serving` is a check just made of the job
	/// socket.
	func refresh(serving: Bool? = nil) {
		state = GameState(running: isRunning(), serving: serving ?? isServing())
		optionSet = launchOptionSet()
	}

	/// FM24 has started: its harness, if it has one, serves once the game has loaded.
	func gameStarted() {
		refresh()
		watch?.cancel()
		guard state != .ready else {
			return
		}
		let interval = self.interval
		let checks = self.checks
		watch = Task { [weak self] in
			for _ in 0..<checks {
				try? await Task.sleep(nanoseconds: UInt64(interval * 1_000_000_000))
				guard let self, !Task.isCancelled, self.state != .notRunning else {
					return
				}
				self.refresh()
				if self.state == .ready {
					return
				}
			}
		}
	}

	func gameQuit() {
		watch?.cancel()
		watch = nil
		refresh()
		onQuit()
		quit.send()
	}
}
