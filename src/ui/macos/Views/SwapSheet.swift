// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// The swap prompt, a sheet on the viewer window: "Swap <name> for...", the picker over the
/// player list's matches (by name once the list has loaded, or a unique ID), "Allow duplicates?"
/// and Confirm (the default) and Cancel.
struct SwapSheet: View {
	static let rows = 12

	@ObservedObject var model: ViewerModel
	@ObservedObject var prompt: SwapPrompt
	@ObservedObject var players: ChoiceList

	var body: some View {
		VStack(alignment: .leading, spacing: 10) {
			Text(SwapPrompt.title)
				.font(.headline)
			Text(prompt.label)
				.lineLimit(1)
				.truncationMode(.tail)
				.help(prompt.label)
			SearchPicker(
				text: prompt.text,
				placeholder: prompt.cue,
				matches: prompt.matches,
				limit: SwapPrompt.textLimit,
				enabled: true,
				focusSerial: prompt.focusSerial,
				label: prompt.label,
				onType: { prompt.type($0) },
				onPick: { prompt.pick($0) },
				onSubmit: { model.confirmSwap() },
				onCancel: { model.cancelSwap() }
			)
			.frame(width: 440, height: SearchPicker.height(rows: SwapSheet.rows))
			HStack {
				Toggle(SwapPrompt.duplicatesLabel, isOn: $prompt.allowDuplicates)
					.toggleStyle(.checkbox)
				Spacer()
				Button("Cancel") { model.cancelSwap() }
					.keyboardShortcut(.cancelAction)
				Button("Confirm") { model.confirmSwap() }
					.keyboardShortcut(.defaultAction)
			}
			.frame(width: 440)
		}
		.padding(16)
	}
}
