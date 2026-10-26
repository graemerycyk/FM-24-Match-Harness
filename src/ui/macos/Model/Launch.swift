// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Combine
import FixtureCore
import Foundation

/// A save the Launch FM24 sheet offers: a .fm file in one of FM24's save folders.
struct LaunchSave: Equatable {
	/// Its name, as fm24ctl reports it: the file name without .fm.
	let name: String
	let url: URL
	/// "cloud/games" or "games".
	let folder: String
	let modified: Date
}

/// The saves `fm24ctl launch --save` finds, newest first: those in the cloud/games and games
/// folders of FM24's data folder, the usual one or the one Steam's launch option gives the game
/// (src/ui/save_list.h, which follows fm24ctl's rules). Only file names and dates are read.
struct LaunchSaves: Equatable {
	enum Data: Equatable {
		case usual
		/// The folder Steam's launch option gives the game.
		case steam
		/// Steam's launch option sets the folder in a way fm24ctl does not follow.
		case unclear
		/// The home folder is not known.
		case noHome
	}

	static let folderNames = ["cloud/games", "games"]

	let data: Data
	/// The data folder, unless `data` is unclear or there is no home folder.
	let folder: URL?
	let saves: [LaunchSave]
	/// The home folder they were read for, which paths show as ~.
	var home = ""
	/// Steam's launch option for FM24 runs the harness's launcher, as Steam has saved it, so that
	/// `fm24ctl launch` starts the game through Steam; otherwise it opens fm.app directly.
	var optionSet = false

	/// What the sheet says before Launch FM24 while Steam's launch option is not set: the direct
	/// start it would make has not been verified. Its Install Harness… button closes it to set the
	/// option, as File > Install Harness… waits until it is closed.
	static let directStartWarning =
		"Steam's launch option for FM24 is not set, or Steam has not saved it yet, so Launch FM24 "
		+ "would open the game without it, which has not been verified: the game crashed in its one "
		+ "attempt. Set the option with Install Harness… and start FM24 from Steam instead."

	/// Whether Steam's launch option for FM24 runs the harness's launcher in `home`
	/// (saveList_launchOptionSet).
	static func launchOptionSet(home: String) -> Bool {
		saveList_launchOptionSet(home)
	}

	/// The saves of the user whose home folder is `home`.
	static func read(home: String) -> LaunchSaves {
		let optionSet = launchOptionSet(home: home)
		guard let list = saveList_read(home) else {
			return LaunchSaves(data: .noHome, folder: nil, saves: [], home: home, optionSet: optionSet)
		}
		defer { saveList_free(list) }
		let data: Data
		switch saveList_data(list) {
		case SAVE_LIST_USUAL: data = .usual
		case SAVE_LIST_STEAM: data = .steam
		case SAVE_LIST_UNCLEAR: data = .unclear
		default: data = .noHome
		}
		let folder = FixtureHandle.text { saveList_dataFolder(list, $0, $1) }
		let saves = (0..<saveList_count(list)).map { index in
			LaunchSave(
				name: FixtureHandle.text { saveList_name(list, index, $0, $1) },
				url: URL(fileURLWithPath: FixtureHandle.text { saveList_path(list, index, $0, $1) }),
				folder: folderNames[Int(min(saveList_folder(list, index), 1))],
				modified: Date(timeIntervalSince1970: TimeInterval(saveList_modified(list, index)))
			)
		}
		return LaunchSaves(
			data: data,
			folder: folder.isEmpty ? nil : URL(fileURLWithPath: folder, isDirectory: true),
			saves: saves,
			home: home,
			optionSet: optionSet
		)
	}

	/// Whether fm24ctl can load a save from the sheet: not while Steam's launch option sets the data
	/// folder in a way it does not follow, as it then refuses every save (macos_game.c's
	/// dataFolder), and the sheet cannot pass --user-data.
	var loadsSaves: Bool {
		data != .unclear
	}

	/// What the sheet says about where the saves come from.
	var note: String {
		switch data {
		case .unclear:
			return "Steam's launch option for FM24 sets its data folder (user_data_location) in a way "
				+ "fm24ctl does not follow, so FM24 Viewer cannot load a save: launch FM24 at its start "
				+ "screen and load the save there, or give the folder in the option as "
				+ "--user_data_location=<absolute folder>."
		case .noHome:
			return
				"Your home folder cannot be found, so no saves are listed. Choose Other… to pick a save."
		case .usual, .steam:
			let place = folder.map { abbreviated($0.path) } ?? ""
			let given = data == .steam ? ", the data folder Steam's launch option gives FM24" : ""
			return saves.isEmpty
				? "No saves in cloud/games or games of \(place)\(given). Choose Other… to pick a save."
				: "Saves in \(place)\(given), newest first."
		}
	}

	/// A path in the home folder as ~/….
	func abbreviated(_ path: String) -> String {
		LaunchSaves.abbreviated(path, home: home)
	}

	/// A path in `home` as ~/….
	static func abbreviated(_ path: String, home: String) -> String {
		let folder = home.hasSuffix("/") ? String(home.dropLast()) : home
		guard folder.hasPrefix("/"), path.hasPrefix(folder + "/") else {
			return path
		}
		return "~" + path.dropFirst(folder.count)
	}
}

/// The Launch FM24 sheet: what to load as the game starts with the harness (a listed save, a .fm
/// file chosen with Other…, or none: the start screen), whether to run its x86_64 slice under
/// Rosetta, and how the launch is going.
@MainActor
final class LaunchPrompt: ObservableObject {
	enum Choice: Hashable {
		/// A save's .fm file, listed or chosen with Other….
		case save(URL)
		/// A save's name or .fm file as given to --launch-save.
		case named(String)
		case startScreen
	}

	enum Phase: Equatable {
		case choosing
		/// fm24ctl runs.
		case launching
		/// Cancel ended fm24ctl.
		case stopped
		/// fm24ctl's reason.
		case failed(String)
		/// fm24ctl's last line, with a fixture already on show.
		case succeeded(String)
	}

	static let title = "Launch FM24"
	static let startScreenLabel = "Start screen (no save)"
	static let rosettaLabel = "Run under Rosetta (x86_64)"
	/// The lines of fm24ctl's progress the sheet keeps.
	static let lineLimit = 200

	let saves: LaunchSaves
	/// On Apple silicon: the game's x86_64 slice can run under Rosetta.
	let offersRosetta: Bool
	@Published private(set) var choice: Choice
	/// A .fm file chosen with Other…, listed after the saves.
	@Published private(set) var other: URL?
	@Published private(set) var rosetta: Bool
	@Published private(set) var phase = Phase.choosing
	/// What fm24ctl has printed on standard output as it starts the game: its progress.
	@Published private(set) var lines: [String] = []

	/// Starts with `remembered`, the last choice (a .fm file's path, or "" for the start screen),
	/// if it can still be made, else the newest save, else the start screen, which is the only
	/// choice if fm24ctl cannot load a save.
	init(saves: LaunchSaves, offersRosetta: Bool, remembered: String?, rosetta: Bool) {
		self.saves = saves
		self.offersRosetta = offersRosetta
		self.rosetta = offersRosetta && rosetta
		let newest = saves.saves.first.map { Choice.save($0.url) } ?? .startScreen
		guard saves.loadsSaves else {
			choice = .startScreen
			return
		}
		switch remembered {
		case .some(""):
			choice = .startScreen
		case .some(let path) where path.hasPrefix("/"):
			let url = URL(fileURLWithPath: path)
			if saves.saves.contains(where: { $0.url == url }) {
				choice = .save(url)
			} else if FileManager.default.fileExists(atPath: path) {
				other = url
				choice = .save(url)
			} else {
				choice = newest
			}
		default:
			choice = newest
		}
	}

	/// Starts with the save --launch-save names, launching; with `rosetta` only if it is offered.
	init(saves: LaunchSaves, offersRosetta: Bool, named save: String, rosetta: Bool) {
		self.saves = saves
		self.offersRosetta = offersRosetta
		self.rosetta = offersRosetta && rosetta
		choice = .named(save)
	}

	/// What `fm24ctl launch --save` is given: a .fm file's path or a save's name; nil for the start
	/// screen.
	var save: String? {
		switch choice {
		case .save(let url): return url.path
		case .named(let save): return save
		case .startScreen: return nil
		}
	}

	/// The choice as the log and the settings keep it: a .fm file's path, or "" for the start
	/// screen; nil for a save named on the command line.
	var remembered: String? {
		switch choice {
		case .save(let url): return url.path
		case .named: return nil
		case .startScreen: return ""
		}
	}

	/// What is chosen, in words.
	var label: String {
		switch choice {
		case .save(let url):
			return saves.saves.first { $0.url == url }?.name ?? url.lastPathComponent
		case .named(let save): return save
		case .startScreen: return LaunchPrompt.startScreenLabel
		}
	}

	/// Where the game starts, for the log: "with <save>" or "at its start screen".
	var destination: String {
		choice == .startScreen ? "at its start screen" : "with \(label)"
	}

	/// What the sheet says while fm24ctl starts the game.
	var launchingText: String {
		"Starting FM24 \(destination); this can take several minutes."
	}

	var isLaunching: Bool {
		phase == .launching
	}

	/// Other… can choose a .fm file, unless fm24ctl cannot load one, or a launch runs.
	var offersOther: Bool {
		saves.loadsSaves && !isLaunching
	}

	/// Beside Cancel, Install Harness…, while Steam has not saved the launch option, as the sheet's
	/// warning says to set it first.
	var offersInstall: Bool {
		!saves.optionSet
	}

	/// A .fm file chosen with Other…, which becomes the choice.
	func chooseOther(_ url: URL) {
		guard offersOther else {
			return
		}
		other = url
		choice = .save(url)
	}

	/// The choice can be changed, and the launch started, unless it runs; a .fm file only if
	/// fm24ctl can load one.
	func choose(_ choice: Choice) {
		if case .save = choice, !saves.loadsSaves {
			return
		}
		if !isLaunching {
			self.choice = choice
		}
	}

	func setRosetta(_ rosetta: Bool) {
		if !isLaunching && offersRosetta {
			self.rosetta = rosetta
		}
	}

	func begin() {
		lines = []
		phase = .launching
	}

	/// A line of fm24ctl's progress.
	func add(_ line: String) {
		lines.append(line)
		if lines.count > LaunchPrompt.lineLimit {
			lines.removeFirst(lines.count - LaunchPrompt.lineLimit)
		}
	}

	func end(_ phase: Phase) {
		self.phase = phase
	}
}
