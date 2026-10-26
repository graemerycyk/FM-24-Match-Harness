// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit

/// FM24 Viewer: the macOS fixture viewer (the Windows viewer is src/ui/fixture_window.c).
///   fm24viewer [<fixture.json|.bin>]
/// opens the fixture, or exports the next one from the game as `fm24ctl` with no arguments does on
/// Windows. FM24CTL names an fm24ctl to run in place of the bundle's own. See Snapshot.swift for
/// the developer snapshot argument.
@main
enum ViewerApp {
	@MainActor
	static func main() {
		let arguments = CommandLine.arguments
		if arguments.count > 1, arguments[1] == Snapshot.argument {
			exit(Snapshot.run(Array(arguments.dropFirst(2))))
		}
		let app = NSApplication.shared
		let delegate = AppDelegate()
		app.delegate = delegate
		withExtendedLifetime(delegate) {
			app.run()
		}
	}
}
