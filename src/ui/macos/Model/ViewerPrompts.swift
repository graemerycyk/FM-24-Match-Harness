// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// The questions and file panels the viewer puts to the user: sheets on the window in the app
/// (App/SystemPrompts.swift), scripted answers in the tests.
@MainActor
protocol ViewerPrompts: AnyObject {
	/// An error box, titled as the Windows viewer titles it.
	func showError(_ message: String, title: String)
	/// An error box for a job no harness of this protocol served, with OK (the default) and
	/// `action` (Launch FM24… or Install Harness…); `completion` gets true for `action`.
	func offerLaunch(
		_ message: String,
		title: String,
		action: String,
		completion: @escaping (Bool) -> Void
	)
	/// A question whose answer, `action` (such as "Discard"), loses something; Cancel is the
	/// default, as No is on Windows. `completion` gets true for `action`.
	func confirm(
		_ message: String,
		title: String,
		action: String,
		completion: @escaping (Bool) -> Void
	)
	/// The Open panel; `completion` gets the chosen file, or nil if it was cancelled.
	func chooseFixture(completion: @escaping (URL?) -> Void)
	/// The Save panel, offering `name` in `directory` as JSON or (`binary`) a binary fixture;
	/// `completion` gets the chosen file and whether it is binary, or nil.
	func chooseDestination(
		name: String,
		directory: URL?,
		binary: Bool,
		completion: @escaping (URL?, Bool) -> Void
	)
	/// A folder panel for the export folder, starting at `current`; `completion` gets the chosen
	/// folder, or nil.
	func chooseExportFolder(current: URL, completion: @escaping (URL?) -> Void)
	/// An Open panel for a save (a .fm file) to load, starting in `directory` if it is given;
	/// `completion` gets the chosen file, or nil.
	func chooseSave(directory: URL?, completion: @escaping (URL?) -> Void)
}
