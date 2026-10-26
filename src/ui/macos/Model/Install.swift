// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// What `fm24ctl install` reported (macos_game.c's macosGame_install): the harness it wrote and
/// where, the launcher Steam's launch option runs, the game it found, whether that option is
/// already set, the account's option if it has another, and its advice: where it looked for a game
/// it did not find, and a data folder the option sets in a way fm24ctl does not follow.
struct InstallReport: Equatable {
	/// The harness's protocol version.
	var version: String
	/// The folder the harness and its launcher are in.
	var folder: String
	var launcher: String
	/// FM24's fm.app, or nil if Steam's libraries have none.
	var game: String?
	/// Steam's launch option for FM24 already runs the launcher.
	var optionSet: Bool
	/// The launch option to set: /bin/sh "<launcher>" %command%.
	var launchOption: String
	/// The account's launch option for FM24 if it has another one.
	var currentOption: String? = nil
	/// install's advice, in its words.
	var notes: [String] = []

	private static let installedPrefix = "installed harness "
	private static let launcherPrefix = "launcher: "
	private static let gamePrefix = "Football Manager 2024: "
	private static let optionSetPrefix =
		"Steam's launch option for Football Manager 2024 already uses this launcher"
	private static let currentOptionPrefix =
		"Steam's launch option for Football Manager 2024 is now: "
	/// The starts of install's advice: no Steam, no game in its libraries, and a data folder the
	/// option sets in a way fm24ctl does not follow.
	private static let notePrefixes = [
		"Steam is not in ", "Install Football Manager 2024 ", "It sets FM24's data folder",
	]

	/// The launch option that runs `launcher`, as fm24ctl prints it.
	static func launchOption(_ launcher: String) -> String {
		"/bin/sh \"\(launcher)\" %command%"
	}

	/// Reads install's standard output; nil if it does not say where it put the harness.
	static func parse(_ output: [String]) -> InstallReport? {
		guard let installed = output.first(where: { $0.hasPrefix(installedPrefix) }),
			let launcherLine = output.first(where: { $0.hasPrefix(launcherPrefix) })
		else {
			return nil
		}
		// "installed harness <version>[ (…)]: <folder>/<dylib>"
		let rest = installed.dropFirst(installedPrefix.count)
		let version = rest.prefix { $0 != " " && $0 != ":" }
		let launcher = String(launcherLine.dropFirst(launcherPrefix.count))
		guard !version.isEmpty, launcher.hasPrefix("/") else {
			return nil
		}
		let game = output.first { $0.hasPrefix(gamePrefix) }.map {
			String($0.dropFirst(gamePrefix.count))
		}
		let option = launchOption(launcher)
		let printed = output.map { $0.trimmingCharacters(in: .whitespaces) }.first { $0 == option }
		let current = output.first { $0.hasPrefix(currentOptionPrefix) }.map {
			String($0.dropFirst(currentOptionPrefix.count))
		}
		return InstallReport(
			version: String(version),
			folder: (launcher as NSString).deletingLastPathComponent,
			launcher: launcher,
			game: game,
			optionSet: output.contains { $0.hasPrefix(optionSetPrefix) },
			launchOption: printed ?? option,
			currentOption: current,
			notes: output.filter { line in notePrefixes.contains { line.hasPrefix($0) } }
		)
	}
}

/// The Install Harness sheet: fm24ctl installing, what it reported, or why it failed.
enum InstallState: Equatable {
	case installing
	case installed(InstallReport)
	case failed(String)
}
