// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// The Launch FM24 sheet: the saves fm24ctl finds, newest first, then the start screen and a .fm
/// file chosen with Other…; a warning while Steam's launch option is not set, as fm24ctl then
/// starts the game directly, with Install Harness… in place of the sheet; on Apple silicon,
/// Rosetta; then fm24ctl's progress as the game starts, or how the launch ended. Launch FM24 is the
/// default button and Cancel ends a launch that runs.
struct LaunchSheet: View {
	static let width: CGFloat = 560

	@ObservedObject var model: ViewerModel
	@ObservedObject var prompt: LaunchPrompt

	private static let modified: DateFormatter = {
		let formatter = DateFormatter()
		formatter.dateStyle = .medium
		formatter.timeStyle = .short
		return formatter
	}()

	var body: some View {
		let launching = prompt.isLaunching
		VStack(alignment: .leading, spacing: 10) {
			Text(LaunchPrompt.title)
				.font(.headline)
			Text("Start FM24 with harness \(FixtureHandle.protocolVersion), loading:")
			List(selection: selection) {
				ForEach(prompt.saves.saves, id: \.url) { save in
					SaveRow(
						name: save.name,
						detail: "\(save.folder) · \(LaunchSheet.modified.string(from: save.modified))"
					)
					.tag(LaunchPrompt.Choice.save(save.url))
				}
				if let other = prompt.other, !prompt.saves.saves.contains(where: { $0.url == other }) {
					SaveRow(
						name: other.deletingPathExtension().lastPathComponent,
						detail: prompt.saves.abbreviated(other.deletingLastPathComponent().path)
					)
					.tag(LaunchPrompt.Choice.save(other))
				}
				if case .named(let save) = prompt.choice {
					SaveRow(name: save, detail: "given to --launch-save")
						.tag(LaunchPrompt.Choice.named(save))
				}
				SaveRow(name: LaunchPrompt.startScreenLabel, detail: "Load a save in FM24 yourself")
					.tag(LaunchPrompt.Choice.startScreen)
			}
			.disabled(launching)
			.frame(height: 210)
			Text(prompt.saves.note)
				.font(.callout)
				.foregroundColor(.secondary)
				.fixedSize(horizontal: false, vertical: true)
			if !prompt.saves.optionSet {
				Label(LaunchSaves.directStartWarning, systemImage: "exclamationmark.triangle")
					.font(.callout)
					.fixedSize(horizontal: false, vertical: true)
			}
			HStack {
				Button("Other…") { model.chooseOtherSave() }
					.disabled(!prompt.offersOther)
				Spacer()
				if prompt.offersRosetta {
					Toggle(
						LaunchPrompt.rosettaLabel,
						isOn: Binding(get: { prompt.rosetta }, set: { model.setLaunchRosetta($0) })
					)
					.toggleStyle(.checkbox)
					.disabled(launching)
				}
			}
			// Room for the progress, so that the sheet keeps its size as the launch goes on.
			LaunchStatus(prompt: prompt)
				.frame(maxWidth: .infinity, minHeight: LaunchStatus.height, alignment: .topLeading)
			HStack {
				Spacer()
				switch prompt.phase {
				case .launching:
					Button("Cancel") { model.cancelLaunch() }
						.keyboardShortcut(.cancelAction)
				case .succeeded:
					Button("Done") { model.cancelLaunch() }
						.keyboardShortcut(.defaultAction)
				case .choosing, .stopped, .failed:
					if prompt.offersInstall {
						Button(ViewerModel.installAction) { model.installInsteadOfLaunch() }
					}
					Button(prompt.phase == .choosing ? "Cancel" : "Close") { model.cancelLaunch() }
						.keyboardShortcut(.cancelAction)
					Button(LaunchPrompt.title) { model.confirmLaunch() }
						.keyboardShortcut(.defaultAction)
						.disabled(model.launchRunning)
				}
			}
		}
		.padding(16)
		.frame(width: LaunchSheet.width)
	}

	private var selection: Binding<LaunchPrompt.Choice?> {
		Binding(
			get: { prompt.choice },
			set: { choice in
				if let choice {
					model.chooseLaunch(choice)
				}
			}
		)
	}
}

/// A row of the sheet's list: a save's name, with where it is and when it was saved.
private struct SaveRow: View {
	let name: String
	let detail: String

	var body: some View {
		VStack(alignment: .leading, spacing: 1) {
			Text(name)
				.lineLimit(1)
				.truncationMode(.middle)
			Text(detail)
				.font(.caption)
				.foregroundColor(.secondary)
				.lineLimit(1)
				.truncationMode(.middle)
		}
		.help(detail)
		.padding(.vertical, 1)
	}
}

/// How the launch is going: fm24ctl's latest lines as it starts the game, or how it ended.
private struct LaunchStatus: View {
	static let shownLines = 6
	static let height: CGFloat = 132

	@ObservedObject var prompt: LaunchPrompt

	var body: some View {
		VStack(alignment: .leading, spacing: 6) {
			switch prompt.phase {
			case .choosing:
				EmptyView()
			case .launching:
				HStack(spacing: 8) {
					ProgressView()
						.controlSize(.small)
					Text(prompt.launchingText)
						.foregroundColor(.secondary)
				}
				.accessibilityElement(children: .combine)
			case .stopped:
				Text(ViewerModel.launchStopped)
					.fixedSize(horizontal: false, vertical: true)
			case .failed(let problem):
				Text(problem)
					.foregroundColor(.red)
					.textSelection(.enabled)
					.fixedSize(horizontal: false, vertical: true)
			case .succeeded:
				Text("FM24 is running with harness \(FixtureHandle.protocolVersion).")
			}
			// The latest lines, one each, so that the newest always shows.
			if !prompt.lines.isEmpty, prompt.phase != .choosing {
				VStack(alignment: .leading, spacing: 1) {
					let lines = Array(prompt.lines.suffix(LaunchStatus.shownLines).enumerated())
					ForEach(lines, id: \.offset) { _, line in
						Text(line)
							.lineLimit(1)
							.truncationMode(.middle)
							.help(line)
					}
				}
				.font(.system(.caption, design: .monospaced))
				.foregroundColor(.secondary)
				.textSelection(.enabled)
				.frame(maxWidth: .infinity, alignment: .leading)
			}
		}
	}
}
