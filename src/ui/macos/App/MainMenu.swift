// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit

/// The menu bar, built in code. Viewer commands go to the app delegate (`target` nil: the
/// responder chain), and the standard ones (editing, windows) to the first responder.
@MainActor
enum MainMenu {
	static func make(recent: NSMenuDelegate) -> NSMenu {
		let menu = NSMenu()
		menu.addItem(submenu(application()))
		menu.addItem(submenu(file(recent: recent)))
		menu.addItem(submenu(edit()))
		menu.addItem(submenu(view()))
		menu.addItem(submenu(players()))
		menu.addItem(submenu(fixture()))
		let window = windows()
		menu.addItem(submenu(window))
		NSApp.windowsMenu = window
		return menu
	}

	private static func application() -> NSMenu {
		let name = "FM24 Viewer"
		let menu = NSMenu(title: name)
		menu.addItem(
			item(
				"About \(name)",
				#selector(NSApplication.orderFrontStandardAboutPanel(_:)),
				target: NSApp
			)
		)
		menu.addItem(.separator())
		let services = NSMenu(title: "Services")
		let servicesItem = item("Services", nil)
		servicesItem.submenu = services
		NSApp.servicesMenu = services
		menu.addItem(servicesItem)
		menu.addItem(.separator())
		menu.addItem(item("Hide \(name)", #selector(NSApplication.hide(_:)), "h"))
		menu.addItem(
			item(
				"Hide Others",
				#selector(NSApplication.hideOtherApplications(_:)),
				"h",
				[.command, .option]
			)
		)
		menu.addItem(item("Show All", #selector(NSApplication.unhideAllApplications(_:))))
		menu.addItem(.separator())
		menu.addItem(item("Quit \(name)", #selector(NSApplication.terminate(_:)), "q"))
		return menu
	}

	private static func file(recent: NSMenuDelegate) -> NSMenu {
		let menu = NSMenu(title: "File")
		menu.addItem(item("Open…", #selector(AppDelegate.openFixture(_:)), "o"))
		let recentMenu = NSMenu(title: "Open Recent")
		recentMenu.delegate = recent
		let recentItem = item("Open Recent", nil)
		recentItem.submenu = recentMenu
		menu.addItem(recentItem)
		menu.addItem(.separator())
		menu.addItem(item("Close", #selector(NSWindow.performClose(_:)), "w"))
		// As in the Windows viewer, saving always asks where: both open the Save panel.
		menu.addItem(item("Save…", #selector(AppDelegate.saveFixture(_:)), "s"))
		let saveAs = item("Save As…", #selector(AppDelegate.saveFixture(_:)), "s", [.command, .shift])
		menu.addItem(saveAs)
		menu.addItem(.separator())
		menu.addItem(item("Export", #selector(AppDelegate.exportFixture(_:)), "e"))
		menu.addItem(item("Export Folder…", #selector(AppDelegate.chooseExportFolder(_:))))
		menu.addItem(.separator())
		menu.addItem(item("Launch FM24…", #selector(AppDelegate.launchGame(_:))))
		menu.addItem(item("Install Harness…", #selector(AppDelegate.installHarness(_:))))
		return menu
	}

	private static func edit() -> NSMenu {
		let menu = NSMenu(title: "Edit")
		menu.addItem(item("Undo", Selector(("undo:")), "z"))
		menu.addItem(item("Redo", Selector(("redo:")), "z", [.command, .shift]))
		menu.addItem(.separator())
		menu.addItem(item("Cut", #selector(NSText.cut(_:)), "x"))
		menu.addItem(item("Copy", #selector(NSText.copy(_:)), "c"))
		menu.addItem(item("Paste", #selector(NSText.paste(_:)), "v"))
		menu.addItem(item("Select All", #selector(NSText.selectAll(_:)), "a"))
		return menu
	}

	private static func view() -> NSMenu {
		let menu = NSMenu(title: "View")
		let squads = item("Squads", #selector(AppDelegate.showTab(_:)), "1")
		squads.tag = 0
		menu.addItem(squads)
		let venue = item("Venue", #selector(AppDelegate.showTab(_:)), "2")
		venue.tag = 1
		menu.addItem(venue)
		let simulation = item("Simulation", #selector(AppDelegate.showTab(_:)), "3")
		simulation.tag = 2
		menu.addItem(simulation)
		menu.addItem(.separator())
		menu.addItem(item("Show Log", #selector(AppDelegate.showLog(_:)), "l", [.command, .option]))
		menu.addItem(.separator())
		menu.addItem(
			item(
				"Enter Full Screen",
				#selector(NSWindow.toggleFullScreen(_:)),
				"f",
				[.command, .control]
			)
		)
		return menu
	}

	/// For each side: the Max buttons' edits and Swap player for its selected players, and its
	/// pencil. A Max item's tag is `side * PlayerEdit.allCases.count + edit`, the others' the side.
	private static func players() -> NSMenu {
		let menu = NSMenu(title: "Players")
		for (side, name) in ["Home Team", "Away Team"].enumerated() {
			if side > 0 {
				menu.addItem(.separator())
			}
			let header = item(name, nil)
			header.isEnabled = false
			menu.addItem(header)
			for edit in PlayerEdit.allCases {
				let max = item(edit.menuTitle, #selector(AppDelegate.maxPlayers(_:)))
				max.tag = side * PlayerEdit.allCases.count + edit.rawValue
				max.indentationLevel = 1
				menu.addItem(max)
			}
			let swap = item("Swap Player…", #selector(AppDelegate.swapPlayer(_:)))
			swap.tag = side
			swap.indentationLevel = 1
			menu.addItem(swap)
			let replace = item("Replace \(name)…", #selector(AppDelegate.replaceTeam(_:)))
			replace.tag = side
			replace.indentationLevel = 1
			menu.addItem(replace)
		}
		return menu
	}

	/// The Simulation tab's commands.
	private static func fixture() -> NSMenu {
		let menu = NSMenu(title: "Fixture")
		menu.addItem(item("Simulate", #selector(AppDelegate.simulate(_:)), "r"))
		menu.addItem(item("Show Results in Finder", #selector(AppDelegate.revealResults(_:))))
		return menu
	}

	private static func windows() -> NSMenu {
		let menu = NSMenu(title: "Window")
		menu.addItem(item("Minimise", #selector(NSWindow.performMiniaturize(_:)), "m"))
		menu.addItem(item("Zoom", #selector(NSWindow.performZoom(_:))))
		menu.addItem(.separator())
		menu.addItem(
			item("Bring All to Front", #selector(NSApplication.arrangeInFront(_:)), target: NSApp)
		)
		return menu
	}

	private static func item(
		_ title: String,
		_ action: Selector?,
		_ key: String = "",
		_ modifiers: NSEvent.ModifierFlags = .command,
		target: AnyObject? = nil
	) -> NSMenuItem {
		let item = NSMenuItem(title: title, action: action, keyEquivalent: key)
		item.keyEquivalentModifierMask = modifiers
		item.target = target
		return item
	}

	private static func submenu(_ menu: NSMenu) -> NSMenuItem {
		let item = NSMenuItem(title: menu.title, action: nil, keyEquivalent: "")
		item.submenu = menu
		return item
	}
}
