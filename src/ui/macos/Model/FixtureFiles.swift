// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import Foundation

/// The file a fixture came from, with the JSON the viewer reads: for a binary fixture, its decoded
/// JSON is kept beside its own bytes, so that saving needs no second decode.
struct FixtureSource: Equatable {
	enum Kind: Equatable {
		case json
		case binary
	}

	var url: URL
	var kind: Kind
	var json: Data
	var original: Data?
	/// An export not yet saved by the user (the Windows viewer's temp-fixture.json).
	var temporary: Bool

	var name: String {
		url.lastPathComponent
	}

	/// A fixture's kind from its extension, ignoring case.
	static func kind(of url: URL) -> Kind? {
		switch url.pathExtension.lowercased() {
		case "json": return .json
		case "bin": return .binary
		default: return nil
		}
	}
}

/// Reading and writing fixture files, away from the main actor.
enum FixtureFiles {
	/// Paths reach fm24ctl and the harness as text of at most this many characters, NUL included.
	static let pathLimit = 1024
	static let pathProblem =
		"Select a .json or .bin fixture with a path shorter than 1024 characters."
	static let writeFailed = "Cannot write the fixture. The destination was not changed."

	/// Whether the viewer takes `url` as a fixture to open or save.
	static func acceptable(_ url: URL) -> Bool {
		url.isFileURL && url.path.unicodeScalars.count < pathLimit && FixtureSource.kind(of: url) != nil
	}

	/// Reads a JSON fixture file and the fixture in it.
	static func load(_ url: URL, temporary: Bool) throws -> (FixtureSource, FixtureHandle) {
		guard FixtureSource.kind(of: url) == .json else {
			throw ViewerError(message: "expected a .json fixture")
		}
		let json: Data
		do {
			json = try Data(contentsOf: url)
		} catch {
			throw ViewerError(message: "cannot read fixture")
		}
		let handle = try FixtureHandle(json: json)
		return (
			FixtureSource(url: url, kind: .json, json: json, original: nil, temporary: temporary), handle
		)
	}

	/// Reads a binary fixture and copies it to `copy`, in the viewer's temporary folder, for the game
	/// to decode there; returns its bytes.
	static func copyBinary(_ url: URL, to copy: URL) throws -> Data {
		let bytes: Data
		do {
			bytes = try Data(contentsOf: url)
		} catch {
			throw ViewerError(message: "cannot read fixture")
		}
		do {
			try bytes.write(to: copy, options: .withoutOverwriting)
		} catch {
			throw ViewerError(message: "Cannot copy the fixture into the viewer's temporary folder.")
		}
		return bytes
	}

	/// A binary fixture from its bytes and the JSON the game decoded it into, at `decoded`.
	static func loadDecoded(
		_ url: URL,
		bytes: Data,
		decoded: URL
	) throws -> (FixtureSource, FixtureHandle) {
		let json: Data
		do {
			json = try Data(contentsOf: decoded)
		} catch {
			throw ViewerError(message: "cannot read the decoded fixture")
		}
		let handle = try FixtureHandle(json: json)
		return (
			FixtureSource(url: url, kind: .binary, json: json, original: bytes, temporary: false), handle
		)
	}

	/// Exports a fixture not yet saved, with its pending edits, as JSON at `url`, which must not
	/// exist yet, and returns the exported fixture's source. Nothing to write gives a copy.
	static func export(
		_ source: FixtureSource,
		playerEdits: [UInt8]?,
		venue: [FixtureViewNumber],
		to url: URL
	) throws -> FixtureSource {
		let edits = playerEdits?.contains { $0 != 0 } == true ? playerEdits : nil
		let json =
			edits == nil && !FixtureHandle.hasVenue(venue)
			? source.json
			: try FixtureHandle.write(json: source.json, playerEdits: edits, venue: venue, binary: false)
		let folder = url.deletingLastPathComponent()
		do {
			try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
		} catch {
			throw ViewerError(message: "Cannot create the export folder \(folder.path).")
		}
		try create(url, with: json)
		return FixtureSource(url: url, kind: .json, json: json, original: nil, temporary: false)
	}

	/// Writes a new file at `url`, never replacing a file that is there already: through a temporary
	/// file beside it, which takes the name only if nothing has it. Volumes that cannot rename that
	/// way (exFAT) get `url` created directly instead, as Windows' CopyFileW does. `renameNew` is
	/// renamex_np with RENAME_EXCL (tests replace it): 0, or why it failed (errno).
	static func create(
		_ url: URL,
		with bytes: Data,
		renameNew: (String, String) -> Int32 = FixtureFiles.renameNew
	) throws {
		func failure(_ error: Int32) -> ViewerError {
			ViewerError(
				message:
					"Cannot export fixture (\(String(cString: strerror(error)))). No file was overwritten."
			)
		}
		let temporary = url.deletingLastPathComponent()
			.appendingPathComponent(".\(url.lastPathComponent).\(UUID().uuidString).tmp")
		let error = writeNew(temporary.path, bytes)
		if error != 0 {
			throw failure(error)
		}
		let renamed = renameNew(temporary.path, url.path)
		guard renamed != 0 else {
			return
		}
		try? FileManager.default.removeItem(at: temporary)
		guard renamed == ENOTSUP || renamed == EINVAL else {
			throw failure(renamed)
		}
		let direct = writeNew(url.path, bytes)
		if direct != 0 {
			throw failure(direct)
		}
	}

	static func renameNew(_ from: String, _ to: String) -> Int32 {
		renamex_np(from, to, UInt32(RENAME_EXCL)) == 0 ? 0 : errno
	}

	/// Writes `bytes` to a file that must not exist yet, which is removed again if they cannot all be
	/// written; 0, or why it could not (errno).
	private static func writeNew(_ path: String, _ bytes: Data) -> Int32 {
		let file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o644)
		guard file >= 0 else {
			return errno
		}
		var error: Int32 = 0
		bytes.withUnsafeBytes { raw in
			var offset = 0
			while offset < raw.count {
				let written = write(file, raw.baseAddress! + offset, raw.count - offset)
				if written < 0 && errno != EINTR {
					error = errno
					return
				}
				offset += max(written, 0)
			}
		}
		if close(file) != 0 && error == 0 {
			error = errno
		}
		if error != 0 {
			unlink(path)
		}
		return error
	}

	/// Saves `source` with its pending edits as `kind` at `url`, and returns the saved fixture's
	/// source. A file of the same kind with nothing to write is copied as it is; otherwise the player
	/// edits, then the venue (whenever any of its values is stored, which recomputes the values
	/// derived from them), then the binary encoding are applied. A binary file has no place for the
	/// save an export names, so the JSON kept with it has none either, as when it is opened again.
	static func save(
		_ source: FixtureSource,
		playerEdits: [UInt8]?,
		venue: [FixtureViewNumber],
		as kind: FixtureSource.Kind,
		to url: URL
	) throws -> FixtureSource {
		let edits = playerEdits?.contains { $0 != 0 } == true ? playerEdits : nil
		let writesVenue = FixtureHandle.hasVenue(venue)
		var json = source.json
		var bytes: Data
		if edits == nil && !writesVenue && kind == source.kind {
			bytes = kind == .binary ? source.original ?? source.json : source.json
		} else {
			json = try FixtureHandle.write(
				json: source.json,
				playerEdits: edits,
				venue: venue,
				binary: false
			)
			bytes =
				kind == .binary
				? try FixtureHandle.write(json: json, playerEdits: nil, venue: nil, binary: true)
				: json
		}
		if kind == .binary {
			json = try FixtureHandle.withoutSave(json)
		}
		try replace(url, with: bytes)
		return FixtureSource(
			url: url,
			kind: kind,
			json: json,
			original: kind == .binary ? bytes : nil,
			temporary: false
		)
	}

	/// Replaces the file at `url` with `bytes` through a temporary file beside it, so that a failed
	/// save leaves the destination as it was.
	static func replace(_ url: URL, with bytes: Data) throws {
		let temporary = url.deletingLastPathComponent()
			.appendingPathComponent(".\(url.lastPathComponent).\(UUID().uuidString).tmp")
		do {
			try bytes.write(to: temporary, options: .withoutOverwriting)
		} catch {
			try? FileManager.default.removeItem(at: temporary)
			throw ViewerError(message: writeFailed)
		}
		if rename(temporary.path, url.path) != 0 {
			let reason = String(cString: strerror(errno))
			try? FileManager.default.removeItem(at: temporary)
			throw ViewerError(message: "Cannot replace the destination (\(reason)).")
		}
	}
}
