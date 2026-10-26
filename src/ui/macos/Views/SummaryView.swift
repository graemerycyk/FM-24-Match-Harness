// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// The fixture summary at the top of the window: the file on show (or, for an export not yet
/// saved, a warning with the link that exports it), the competition, venue, date, weather and
/// referee, the save it was exported from if it names one, then the two notes. Its text can be
/// selected and copied.
struct SummaryView: View {
	/// The export link's target, which the viewer handles itself.
	static let exportURL = URL(string: "fm24viewer:export")!

	@ObservedObject var model: ViewerModel
	let fixture: FixtureHandle

	var body: some View {
		Text(summary)
			.textSelection(.enabled)
			.fixedSize(horizontal: false, vertical: true)
			.frame(maxWidth: .infinity, alignment: .leading)
			.environment(
				\.openURL,
				OpenURLAction { url in
					guard url == SummaryView.exportURL else {
						return .systemAction
					}
					model.export()
					return .handled
				}
			)
	}

	private var summary: AttributedString {
		var text = AttributedString()
		if model.isTemporary {
			var warning = bold(ViewerModel.unsavedPrefix)
			warning.foregroundColor = .red
			var link = AttributedString(ViewerModel.exportLink)
			link.link = SummaryView.exportURL
			link.underlineStyle = .single
			text += warning
			text += link
			text += AttributedString(ViewerModel.exportSuffix + "\n")
		} else if let heading = model.heading {
			text += AttributedString(SummaryHeading.prefix)
			text += bold(heading.name)
			text += AttributedString(heading.marker + "\n")
		}
		for line in fixture.summaryLines {
			text += AttributedString(line + "\n")
		}
		// The save the fixture was exported from, which fm24ctl checks before a job (macOS only).
		if let save = fixture.saveLine {
			text += AttributedString(save + "\n")
		}
		// A blank line, then each note with its label in bold.
		for note in FixtureHandle.notes {
			let utf8 = note.text.utf8
			text += AttributedString("\n")
			text += bold(String(decoding: utf8.prefix(note.labelLength), as: UTF8.self))
			text += AttributedString(String(decoding: utf8.dropFirst(note.labelLength), as: UTF8.self))
		}
		return text
	}

	private func bold(_ string: String) -> AttributedString {
		var text = AttributedString(string)
		text.font = .body.bold()
		return text
	}
}
