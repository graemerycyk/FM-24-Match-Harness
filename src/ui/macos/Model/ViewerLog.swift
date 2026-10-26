// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Combine
import Foundation

/// What the viewer has done and why things failed: the Log window's text, standing in for the
/// Windows viewer's console. Errors also go to standard error as "fixture viewer: <message>".
@MainActor
final class ViewerLog: ObservableObject {
	@Published private(set) var lines: [String] = []

	private let echo: Bool
	private let time: DateFormatter = {
		let formatter = DateFormatter()
		formatter.dateFormat = "HH:mm:ss"
		formatter.locale = Locale(identifier: "en_US_POSIX")
		return formatter
	}()

	/// `echo` also writes errors to standard error.
	init(echo: Bool = true) {
		self.echo = echo
	}

	var text: String {
		lines.joined(separator: "\n")
	}

	func add(_ line: String) {
		lines.append("\(time.string(from: Date())) \(line)")
	}

	func error(_ message: String) {
		add("Error: \(message)")
		if echo {
			FileHandle.standardError.write(Data("fixture viewer: \(message)\n".utf8))
		}
	}

	func clear() {
		lines.removeAll()
	}
}
