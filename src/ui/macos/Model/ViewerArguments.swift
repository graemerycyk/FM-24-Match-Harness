// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import Foundation

/// The viewer's command line (`open -a "FM24 Viewer" --args …` passes it):
///   fm24viewer [<fixture.json|.bin>] [--launch-save <name|/path.fm> [--rosetta]]
/// --launch-save starts FM24 with the harness and that save loaded, as File > Launch FM24… does,
/// then exports the next fixture and shows it, unless a fixture is shown by then. A .fm file's
/// path must be absolute: `open` starts the app in "/", not the shell's folder. Other arguments
/// starting with "-" (such as AppKit's -NS… settings) and their values are left alone, as are any
/// that are not fixtures.
struct ViewerArguments: Equatable {
	static let launchSaveOption = "--launch-save"
	static let rosettaOption = "--rosetta"
	static let missingSave = "--launch-save needs a save's name or its .fm file."
	static let rosettaAlone = "--rosetta only applies with --launch-save."

	/// Why --launch-save cannot pass `save` on: a path that is not absolute.
	static func relativeSave(_ save: String) -> String {
		"--launch-save takes a save's name or the absolute path of its .fm file, not \(save)."
	}

	/// The first .json or .bin argument.
	var fixture: URL?
	var launchSave: String?
	var rosetta = false
	/// Why --launch-save was given but launches nothing, for the start panel.
	var launchProblem: String?
	/// Why other arguments were ignored, for the log.
	var problems: [String] = []

	/// Reads the arguments after the executable's path.
	static func parse(_ arguments: [String]) -> ViewerArguments {
		var parsed = ViewerArguments()
		var index = arguments.startIndex
		while index < arguments.endIndex {
			let argument = arguments[index]
			index += 1
			if argument == launchSaveOption {
				let save = index < arguments.endIndex ? arguments[index] : ""
				if save.isEmpty {
					parsed.launchProblem = missingSave
				} else if !saveList_isName(save), !save.hasPrefix("/") {
					parsed.launchProblem = relativeSave(save)
				} else {
					parsed.launchSave = save
				}
				if index < arguments.endIndex {
					index += 1
				}
			} else if argument == rosettaOption {
				parsed.rosetta = true
			} else if !argument.hasPrefix("-"), parsed.fixture == nil,
				FixtureSource.kind(of: URL(fileURLWithPath: argument)) != nil
			{
				parsed.fixture = URL(fileURLWithPath: argument)
			}
		}
		if parsed.rosetta, parsed.launchSave == nil {
			parsed.rosetta = false
			if parsed.launchProblem == nil {
				parsed.problems.append(rosettaAlone)
			}
		}
		return parsed
	}
}
