// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import Combine
import SwiftUI

/// The application: one viewer window, as on Windows, into which every fixture opens (Open With,
/// the Dock, the command line, `fm24ctl`, the Open panel and drops), a Log window, and the close
/// checks, which quitting also runs. Started with no fixture, it exports the next one from the
/// game, as `fm24ctl` with no arguments does on Windows; started with --launch-save, it first
/// starts the game with that save (ViewerArguments).
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate,
	NSMenuItemValidation
{
	private let prompts = SystemPrompts()
	private let log = ViewerLog()
	private let work = WorkFiles()
	private let monitor = GameMonitor(
		isRunning: GameWatch.gameRunning,
		isServing: Fm24ctl.channelServed,
		launchOptionSet: { LaunchSaves.launchOptionSet(home: ViewerModel.homeFolder()) }
	)
	private lazy var gameWatch = GameWatch(monitor)
	private lazy var dockProgress = DockProgress(model)
	private lazy var harness: Fm24ctl? = Fm24ctl.locate().map { Fm24ctl(executable: $0) }
	private lazy var model = ViewerModel(
		log: log,
		prompts: prompts,
		harness: harness,
		work: work,
		monitor: monitor
	)
	private var window: NSWindow?
	private var logWindow: NSWindow?
	/// The sheet of the viewer's own that is open: the swap prompt, or the Launch FM24 or Install
	/// Harness sheet (one at a time, as each opens only while no other waits for the user).
	private var sheet: (kind: SheetKind, window: NSWindow)?
	private var watchers: Set<AnyCancellable> = []
	/// A file came with the launch, so the command line's is not opened again.
	private var openedAtLaunch = false
	/// The command line's fixture, in case AppKit also hands it over as a file to open.
	private var argument: URL?
	/// The user has chosen to discard unsaved edits, so closing and quitting no longer ask.
	private var discarded = false

	func applicationWillFinishLaunching(_ notification: Notification) {
		NSApp.mainMenu = MainMenu.make(recent: self)
		window = makeWindow()
		model.noteFile = { url in
			NSDocumentController.shared.noteNewRecentDocumentURL(url)
		}
		model.reveal = { urls in
			NSWorkspace.shared.activateFileViewerSelecting(urls)
		}
		model.copyText = { text in
			NSPasteboard.general.clearContents()
			NSPasteboard.general.setString(text, forType: .string)
		}
		harness?.onRun = { [weak self] arguments in
			self?.log.add("Running fm24ctl \(arguments.joined(separator: " "))")
		}
		_ = gameWatch
		_ = dockProgress
		let directory = work.directory
		DispatchQueue.global(qos: .utility).async {
			WorkFiles.sweep(directory)
		}
	}

	/// Open With, a drop on the Dock icon, `open -a`, and Open Recent.
	func application(_ application: NSApplication, open urls: [URL]) {
		guard let url = urls.first else {
			return
		}
		openedAtLaunch = true
		if let argument, argument.standardizedFileURL == url.standardizedFileURL {
			self.argument = nil
			return
		}
		if urls.count > 1 {
			log.add("Opening \(url.lastPathComponent), the first of \(urls.count) files")
		}
		model.requestOpen(url)
	}

	func applicationDidFinishLaunching(_ notification: Notification) {
		let arguments = ViewerArguments.parse(Array(CommandLine.arguments.dropFirst()))
		for problem in arguments.problems {
			log.error(problem)
		}
		// A --launch-save that cannot launch: the start panel says why, if it shows.
		if !openedAtLaunch {
			if let url = arguments.fixture {
				argument = url
				model.open(url, temporary: work.isTemporaryFixture(url))
			} else if let problem = arguments.launchProblem {
				model.showStartProblem(problem)
			} else if arguments.launchSave == nil {
				model.exportNextFixture()
			}
		}
		if let problem = arguments.launchProblem, openedAtLaunch || arguments.fixture != nil {
			log.error(problem)
		}
		window?.makeKeyAndOrderFront(nil)
		if let save = arguments.launchSave {
			model.launch(save: save, rosetta: arguments.rosetta)
		}
	}

	func applicationWillTerminate(_ notification: Notification) {
		model.stopJobs()
		work.removeFolder()
	}

	func applicationShouldOpenUntitledFile(_ sender: NSApplication) -> Bool {
		false
	}

	/// Quitting runs the window's close checks. The question about unsaved edits is a sheet, so
	/// the answer comes later.
	func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
		guard !discarded else {
			return .terminateNow
		}
		switch model.closeCheck() {
		case .close:
			return .terminateNow
		case .blocked(let message):
			model.report(message)
			return .terminateCancel
		case .confirm:
			// Asked once AppKit is waiting for the reply.
			DispatchQueue.main.async { [weak self] in
				self?.model.confirmClose { quit in
					self?.discarded = quit
					NSApp.reply(toApplicationShouldTerminate: quit)
				}
			}
			return .terminateLater
		}
	}

	func windowShouldClose(_ sender: NSWindow) -> Bool {
		guard sender === window, !discarded, model.closeCheck() != .close else {
			return true
		}
		model.confirmClose { [weak self, weak sender] close in
			if close {
				self?.discarded = true
				sender?.close()
			}
		}
		return false
	}

	/// Closing the viewer window ends the app, as on Windows, even with the Log window open.
	func windowWillClose(_ notification: Notification) {
		if notification.object as? NSWindow === window {
			NSApp.terminate(nil)
		}
	}

	// MARK: Menu commands

	@objc func openFixture(_ sender: Any?) {
		model.requestOpen()
	}

	@objc func saveFixture(_ sender: Any?) {
		model.requestSave()
	}

	@objc func exportFixture(_ sender: Any?) {
		model.export()
	}

	@objc func chooseExportFolder(_ sender: Any?) {
		model.requestExportFolder()
	}

	@objc func launchGame(_ sender: Any?) {
		model.requestLaunch()
	}

	@objc func installHarness(_ sender: Any?) {
		model.requestInstall()
	}

	private static let tabs: [ViewerModel.Tab] = [.squads, .venue, .simulation]

	@objc func showTab(_ sender: NSMenuItem) {
		if AppDelegate.tabs.indices.contains(sender.tag) {
			model.tab = AppDelegate.tabs[sender.tag]
		}
	}

	@objc func simulate(_ sender: Any?) {
		model.simulate()
	}

	@objc func revealResults(_ sender: Any?) {
		model.revealResults()
	}

	@objc func maxPlayers(_ sender: NSMenuItem) {
		let count = PlayerEdit.allCases.count
		if let edit = PlayerEdit(rawValue: sender.tag % count) {
			model.maxPlayers(edit, side: sender.tag / count)
		}
	}

	@objc func swapPlayer(_ sender: NSMenuItem) {
		model.requestSwap(side: sender.tag)
	}

	@objc func replaceTeam(_ sender: NSMenuItem) {
		model.editTeam(side: sender.tag)
	}

	@objc func showLog(_ sender: Any?) {
		let shown = logWindow ?? makeLogWindow()
		logWindow = shown
		shown.makeKeyAndOrderFront(nil)
	}

	@objc func openRecent(_ sender: NSMenuItem) {
		if let url = sender.representedObject as? URL {
			model.requestOpen(url)
		}
	}

	@objc func clearRecent(_ sender: Any?) {
		NSDocumentController.shared.clearRecentDocuments(sender)
	}

	/// The commands that start or change something, which wait while a question, panel or the swap
	/// prompt is open, as Windows' modal dialogs make them.
	private static let starters: Set<Selector> = [
		#selector(openFixture(_:)), #selector(openRecent(_:)), #selector(saveFixture(_:)),
		#selector(exportFixture(_:)), #selector(chooseExportFolder(_:)), #selector(simulate(_:)),
		#selector(maxPlayers(_:)), #selector(swapPlayer(_:)), #selector(replaceTeam(_:)),
		#selector(launchGame(_:)), #selector(installHarness(_:)),
	]

	func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
		if let action = menuItem.action, AppDelegate.starters.contains(action), model.isAsking {
			return false
		}
		switch menuItem.action {
		case #selector(openFixture(_:)), #selector(openRecent(_:)), #selector(launchGame(_:)),
			#selector(installHarness(_:)):
			return !model.isBusy
		case #selector(saveFixture(_:)):
			return model.fixture != nil && !model.isBusy
		case #selector(exportFixture(_:)):
			return model.canExport
		case #selector(maxPlayers(_:)):
			return model.canEditPlayers(side: menuItem.tag / PlayerEdit.allCases.count)
		case #selector(swapPlayer(_:)):
			return model.canSwap(side: menuItem.tag)
		case #selector(replaceTeam(_:)):
			return model.canEditTeam(side: menuItem.tag)
		case #selector(showTab(_:)):
			let shown =
				AppDelegate.tabs.indices.contains(menuItem.tag)
				&& AppDelegate.tabs[menuItem.tag] == model.tab
			menuItem.state = model.fixture != nil && shown ? .on : .off
			return model.fixture != nil
		case #selector(simulate(_:)):
			return model.canSimulate
		case #selector(revealResults(_:)):
			return !model.simulation.running && !model.simulation.results.isEmpty
		case #selector(clearRecent(_:)):
			return !NSDocumentController.shared.recentDocumentURLs.isEmpty
		default:
			return true
		}
	}

	/// Open Recent, filled when it opens.
	func menuNeedsUpdate(_ menu: NSMenu) {
		menu.removeAllItems()
		let urls = NSDocumentController.shared.recentDocumentURLs
		for url in urls {
			let item = NSMenuItem(
				title: url.lastPathComponent,
				action: #selector(openRecent(_:)),
				keyEquivalent: ""
			)
			item.representedObject = url
			item.toolTip = url.path
			let icon = NSWorkspace.shared.icon(forFile: url.path)
			icon.size = NSSize(width: 16, height: 16)
			item.image = icon
			menu.addItem(item)
		}
		if !urls.isEmpty {
			menu.addItem(.separator())
		}
		menu.addItem(
			NSMenuItem(title: "Clear Menu", action: #selector(clearRecent(_:)), keyEquivalent: "")
		)
	}

	// MARK: Windows

	/// The viewer window: 1480 by 980 points where the screen has room, at least 900 by 600. It
	/// shows the file on show as its subtitle and proxy icon, and unsaved edits in its close button.
	private func makeWindow() -> NSWindow {
		let room = NSScreen.main?.visibleFrame.size ?? NSSize(width: 1480, height: 980)
		let size = NSSize(width: min(1480, room.width), height: min(980, room.height))
		let window = NSWindow(
			contentRect: NSRect(origin: .zero, size: size),
			styleMask: [.titled, .closable, .miniaturizable, .resizable],
			backing: .buffered,
			defer: false
		)
		window.title = model.title
		window.contentMinSize = NSSize(width: 900, height: 600)
		window.isRestorable = false
		window.isReleasedWhenClosed = false
		window.tabbingMode = .disallowed
		window.delegate = self
		let host = NSHostingView(rootView: ViewerView(model: model))
		host.sizingOptions = [.minSize]
		window.contentView = host
		window.setContentSize(size)
		window.center()
		prompts.window = window
		model.$source
			.sink { [weak window] source in
				window?.subtitle = source.map { $0.temporary ? "Not saved" : $0.name } ?? ""
				window?.representedURL = source?.temporary == false ? source?.url : nil
			}
			.store(in: &watchers)
		// After each change (@Published sends before it), the sheet the model asks for.
		Publishers.Merge3(
			model.$swapPrompt.map { _ in () },
			model.$launchPrompt.map { _ in () },
			model.$installState.map { _ in () }
		)
		.receive(on: DispatchQueue.main)
		.sink { [weak self] in self?.updateSheet() }
		.store(in: &watchers)
		// A sheet asked for while the window was in the Dock opens once it is back.
		NotificationCenter.default.publisher(for: NSWindow.didDeminiaturizeNotification, object: window)
			.sink { [weak self] _ in self?.updateSheet() }
			.store(in: &watchers)
		// objectWillChange comes before the change, so the state is read after it.
		model.objectWillChange
			.receive(on: DispatchQueue.main)
			.sink { [weak window, weak model] in
				window?.isDocumentEdited = model?.hasUnsavedEdits == true
			}
			.store(in: &watchers)
		return window
	}

	/// The viewer's own sheets.
	private enum SheetKind: Equatable {
		case swap(ObjectIdentifier)
		case launch(ObjectIdentifier)
		case install
	}

	/// Opens the sheet the model asks for on the viewer window (the swap prompt, the Launch FM24
	/// sheet or the Install Harness sheet), closing the one open if it is another. A window in the
	/// Dock comes back first, and the sheet opens once it has; a hidden one is shown.
	private func updateSheet() {
		let wanted: SheetKind? =
			model.swapPrompt.map { .swap(ObjectIdentifier($0)) }
			?? model.launchPrompt.map { .launch(ObjectIdentifier($0)) }
			?? (model.installState != nil ? .install : nil)
		guard sheet?.kind != wanted else {
			return
		}
		if let (_, open) = sheet {
			sheet = nil
			prompts.sheet = nil
			window?.endSheet(open)
		}
		guard let wanted, let window else {
			return
		}
		if window.isMiniaturized {
			window.deminiaturize(nil)
			return
		}
		if !window.isVisible {
			window.makeKeyAndOrderFront(nil)
		}
		let controller: NSViewController
		let title: String
		switch wanted {
		case .swap:
			guard let prompt = model.swapPrompt else {
				return
			}
			controller = AppDelegate.sized(
				SwapSheet(model: model, prompt: prompt, players: model.players)
			)
			title = SwapPrompt.title
		case .launch:
			guard let prompt = model.launchPrompt else {
				return
			}
			controller = AppDelegate.sized(LaunchSheet(model: model, prompt: prompt))
			title = LaunchPrompt.title
		case .install:
			controller = AppDelegate.sized(InstallSheet(model: model))
			title = InstallSheet.title
		}
		let opened = NSWindow(contentViewController: controller)
		opened.title = title
		opened.isRestorable = false
		sheet = (wanted, opened)
		prompts.sheet = opened
		window.beginSheet(opened) { [weak self] _ in
			// Closed some other way than by the model (the window closing).
			guard let self, self.sheet?.window === opened else {
				return
			}
			self.sheet = nil
			self.prompts.sheet = nil
			switch wanted {
			case .swap: self.model.cancelSwap()
			case .launch: self.model.cancelLaunch()
			case .install: self.model.closeInstall()
			}
		}
	}

	/// A sheet's content, which the sheet's size follows.
	private static func sized<Content: View>(_ content: Content) -> NSViewController {
		let controller = NSHostingController(rootView: content)
		controller.sizingOptions = [.preferredContentSize]
		return controller
	}

	private func makeLogWindow() -> NSWindow {
		let window = NSWindow(
			contentRect: NSRect(x: 0, y: 0, width: 720, height: 360),
			styleMask: [.titled, .closable, .miniaturizable, .resizable],
			backing: .buffered,
			defer: false
		)
		window.title = "Log"
		window.isRestorable = false
		window.isReleasedWhenClosed = false
		window.tabbingMode = .disallowed
		window.contentView = NSHostingView(rootView: LogView(log: log))
		window.center()
		return window
	}

}
