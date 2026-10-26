// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// The files FM24 reads and writes for the viewer, all in the user's temporary directory, which
/// the game can always use: temp-fixture.json, the next fixture's export (as on Windows), and the
/// viewer's own folder, fm24-viewer-<pid>, for copies of fixtures to decode and what the game
/// writes back. The game never touches the user's own folders, where macOS would ask whether FM24
/// may (docs/MACOS.md); the viewer reads and moves the files itself.
@MainActor
final class WorkFiles {
	nonisolated static let temporaryFixtureName = "temp-fixture.json"
	nonisolated static let folderPrefix = "fm24-viewer-"
	/// Another viewer's folder is removed once its viewer has gone and it is this old.
	nonisolated static let staleAge: TimeInterval = 24 * 60 * 60

	/// The user's temporary directory.
	let directory: URL
	/// This viewer's own folder in it.
	let folder: URL
	private var count = 0

	nonisolated init(
		directory: URL = WorkFiles.userTemporaryDirectory(),
		processID: Int32 = getpid()
	) {
		self.directory = directory
		folder = directory.appendingPathComponent(
			"\(WorkFiles.folderPrefix)\(processID)",
			isDirectory: true
		)
	}

	/// Where the next fixture is exported to, as `fm24ctl` with no arguments also does.
	var temporaryFixture: URL {
		directory.appendingPathComponent(WorkFiles.temporaryFixtureName)
	}

	/// Whether `url` is that export, which the viewer shows as a fixture not yet saved.
	func isTemporaryFixture(_ url: URL) -> Bool {
		WorkFiles.samePath(url, temporaryFixture)
	}

	/// A new file in the viewer's folder, `<stem>-<n>.<extension>` with a number not used before,
	/// creating the folder (for the user alone) if need be.
	func file(_ stem: String, extension fileExtension: String) throws -> URL {
		do {
			try FileManager.default.createDirectory(
				at: folder,
				withIntermediateDirectories: true,
				attributes: [.posixPermissions: 0o700]
			)
		} catch {
			throw ViewerError(message: "Cannot create the viewer's temporary folder \(folder.path).")
		}
		count += 1
		return folder.appendingPathComponent("\(stem)-\(count).\(fileExtension)")
	}

	/// Removes files the viewer no longer needs (each new one replaces the last, as on Windows).
	func remove(_ urls: URL...) {
		for url in urls {
			try? FileManager.default.removeItem(at: url)
		}
	}

	/// Removes the viewer's folder, when it quits.
	func removeFolder() {
		try? FileManager.default.removeItem(at: folder)
	}

	/// Removes other viewers' folders in `directory` that are over a day old and whose viewer has
	/// gone (one that crashed, or was forced to quit).
	nonisolated static func sweep(
		_ directory: URL,
		now: Date = Date(),
		alive: (Int32) -> Bool = WorkFiles.processAlive
	) {
		let manager = FileManager.default
		guard
			let names = try? manager.contentsOfDirectory(atPath: directory.path)
		else {
			return
		}
		for name in names where name.hasPrefix(folderPrefix) {
			guard let pid = Int32(name.dropFirst(folderPrefix.count)), pid > 0, !alive(pid) else {
				continue
			}
			let url = directory.appendingPathComponent(name, isDirectory: true)
			let modified = (try? manager.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
			if let modified, now.timeIntervalSince(modified) > staleAge {
				try? manager.removeItem(at: url)
			}
		}
	}

	nonisolated static func processAlive(_ pid: Int32) -> Bool {
		kill(pid, 0) == 0 || errno == EPERM
	}

	/// The user's temporary directory, as fm24ctl and the harness find it
	/// (_CS_DARWIN_USER_TEMP_DIR), not TMPDIR, which may differ.
	nonisolated static func userTemporaryDirectory() -> URL {
		let size = confstr(_CS_DARWIN_USER_TEMP_DIR, nil, 0)
		if size > 1 {
			var buffer = [CChar](repeating: 0, count: size)
			if confstr(_CS_DARWIN_USER_TEMP_DIR, &buffer, size) == size {
				return URL(fileURLWithPath: String(cString: buffer), isDirectory: true)
			}
		}
		return FileManager.default.temporaryDirectory
	}

	/// Whether two file URLs name the same path, through symbolic links such as /var.
	nonisolated static func samePath(_ first: URL, _ second: URL) -> Bool {
		func resolved(_ url: URL) -> String {
			let folder = url.deletingLastPathComponent().resolvingSymlinksInPath().standardizedFileURL
			return folder.appendingPathComponent(url.lastPathComponent).path
		}
		return resolved(first) == resolved(second)
	}
}
