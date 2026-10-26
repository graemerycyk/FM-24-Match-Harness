// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// Where settings are kept: UserDefaults in the app, memory in the tests.
protocol SettingsStore: AnyObject {
	func string(forKey key: String) -> String?
	/// False for a key that is not set.
	func bool(forKey key: String) -> Bool
	func set(_ value: Any?, forKey key: String)
}

extension UserDefaults: SettingsStore {}

/// The viewer's settings, kept in UserDefaults: the export folder (File > Export Folder…), where
/// exports of fixtures not yet saved go, as the working directory takes them on Windows, and the
/// Launch FM24 sheet's last choice.
final class ViewerSettings {
	static let exportFolderKey = "ExportFolder"
	static let launchSaveKey = "LaunchSave"
	static let launchRosettaKey = "LaunchRosetta"

	private let defaults: SettingsStore
	private let documents: URL

	/// `documents` is the user's Documents folder, where the export folder is until one is chosen.
	init(
		defaults: SettingsStore = UserDefaults.standard,
		documents: URL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
			?? FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Documents")
	) {
		self.defaults = defaults
		self.documents = documents
	}

	/// ~/Documents/FM24 Match Harness.
	var defaultExportFolder: URL {
		documents.appendingPathComponent("FM24 Match Harness", isDirectory: true)
	}

	var exportFolder: URL {
		get {
			defaults.string(forKey: ViewerSettings.exportFolderKey).flatMap {
				$0.hasPrefix("/") ? URL(fileURLWithPath: $0, isDirectory: true) : nil
			} ?? defaultExportFolder
		}
		set {
			defaults.set(newValue.path, forKey: ViewerSettings.exportFolderKey)
		}
	}

	/// What the Launch FM24 sheet last loaded: a save's .fm file, or "" for the start screen; nil
	/// before it has launched anything.
	var launchSave: String? {
		get {
			defaults.string(forKey: ViewerSettings.launchSaveKey)
		}
		set {
			defaults.set(newValue, forKey: ViewerSettings.launchSaveKey)
		}
	}

	/// Whether it last ran the game's x86_64 slice under Rosetta.
	var launchRosetta: Bool {
		get {
			defaults.bool(forKey: ViewerSettings.launchRosettaKey)
		}
		set {
			defaults.set(newValue, forKey: ViewerSettings.launchRosettaKey)
		}
	}
}
