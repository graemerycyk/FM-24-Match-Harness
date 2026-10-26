// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import Combine

/// Tells a GameMonitor when FM24 starts or quits, from the system's notices of applications
/// starting and quitting.
@MainActor
final class GameWatch {
	private var watchers: Set<AnyCancellable> = []

	init(_ monitor: GameMonitor) {
		let center = NSWorkspace.shared.notificationCenter
		center.publisher(for: NSWorkspace.didLaunchApplicationNotification)
			.filter(GameWatch.isGame)
			.receive(on: DispatchQueue.main)
			.sink { [weak monitor] _ in monitor?.gameStarted() }
			.store(in: &watchers)
		center.publisher(for: NSWorkspace.didTerminateApplicationNotification)
			.filter(GameWatch.isGame)
			.receive(on: DispatchQueue.main)
			.sink { [weak monitor] _ in monitor?.gameQuit() }
			.store(in: &watchers)
	}

	/// Whether FM24 is running.
	nonisolated static func gameRunning() -> Bool {
		NSWorkspace.shared.runningApplications.contains {
			$0.bundleIdentifier == GameMonitor.bundleIdentifier
		}
	}

	private nonisolated static func isGame(_ notification: Notification) -> Bool {
		let application =
			notification.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
		return application?.bundleIdentifier == GameMonitor.bundleIdentifier
	}
}
