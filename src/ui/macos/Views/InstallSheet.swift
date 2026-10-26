// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// The Install Harness sheet: what `fm24ctl install` did, the Steam launch option that loads the
/// harness, in a field that can be selected, with Copy, the account's own option if it has another,
/// whether Steam already has it, install's advice, and that the new harness loads the next time the
/// game starts.
struct InstallSheet: View {
	static let title = "Install Harness"

	@ObservedObject var model: ViewerModel

	var body: some View {
		VStack(alignment: .leading, spacing: 10) {
			Text(InstallSheet.title)
				.font(.headline)
			switch model.installState {
			case .installing, nil:
				HStack(spacing: 8) {
					ProgressView()
						.controlSize(.small)
					Text(ViewerModel.installingActivity)
						.foregroundColor(.secondary)
				}
				.accessibilityElement(children: .combine)
			case .installed(let report):
				InstalledReport(model: model, report: report)
			case .failed(let problem):
				Text(problem)
					.foregroundColor(.red)
					.textSelection(.enabled)
					.fixedSize(horizontal: false, vertical: true)
			}
			// The sheet keeps its size once the install has finished.
			Spacer(minLength: 0)
			HStack {
				Spacer()
				Button("Done") { model.closeInstall() }
					.keyboardShortcut(.defaultAction)
					.disabled(model.installState == .installing)
			}
		}
		.padding(16)
		.frame(width: 600)
		.frame(minHeight: 320, alignment: .top)
	}
}

private struct InstalledReport: View {
	@ObservedObject var model: ViewerModel
	let report: InstallReport

	private func abbreviated(_ path: String) -> String {
		LaunchSaves.abbreviated(path, home: model.home)
	}

	var body: some View {
		VStack(alignment: .leading, spacing: 10) {
			Text("Installed harness \(report.version) in \(abbreviated(report.folder)).")
				.textSelection(.enabled)
				.fixedSize(horizontal: false, vertical: true)
			Text(
				report.game.map { "Football Manager 2024: \(abbreviated($0))" }
					?? "Football Manager 2024 was not found in your Steam libraries."
			)
			.foregroundColor(.secondary)
			.textSelection(.enabled)
			.fixedSize(horizontal: false, vertical: true)
			Text("Steam launch option for Football Manager 2024:")
			HStack(alignment: .center, spacing: 8) {
				Text(report.launchOption)
					.font(.system(.body, design: .monospaced))
					.textSelection(.enabled)
					.fixedSize(horizontal: false, vertical: true)
					.frame(maxWidth: .infinity, alignment: .leading)
					.padding(6)
					.background(Color(nsColor: .textBackgroundColor))
					.overlay(
						RoundedRectangle(cornerRadius: 4)
							.stroke(Color(nsColor: .separatorColor), lineWidth: 1)
					)
					.accessibilityLabel("Steam launch option")
				Button(model.installCopied ? "Copied" : "Copy") { model.copyLaunchOption() }
			}
			if let current = report.currentOption, !report.optionSet {
				Text(
					"FM24's launch option in Steam is now: \(current)\nKeep any arguments of your own after "
						+ "%command% when you set this one."
				)
				.textSelection(.enabled)
				.fixedSize(horizontal: false, vertical: true)
			}
			Text(
				report.optionSet
					? "Steam's launch option for FM24 already uses this launcher: start the game from "
						+ "Steam as usual, or with Launch FM24…."
					: "Set it once in Steam: Library > Football Manager 2024 > Properties > General > "
						+ "Launch Options. Steam may save it only when it quits: choose Install Harness… "
						+ "again until it says that Steam has the option, quitting Steam if it does not. "
						+ "Until then, Launch FM24… starts the game without the option, which has not "
						+ "been verified: the game crashed in its one attempt."
			)
			.fixedSize(horizontal: false, vertical: true)
			ForEach(report.notes, id: \.self) { note in
				Text(note)
					.textSelection(.enabled)
					.fixedSize(horizontal: false, vertical: true)
			}
			Text(
				"The new harness loads the next time FM24 starts; a game running now keeps the harness "
					+ "it loaded."
			)
			.foregroundColor(.secondary)
			.fixedSize(horizontal: false, vertical: true)
		}
	}
}
