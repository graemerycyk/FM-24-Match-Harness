// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI
import UniformTypeIdentifiers

/// The viewer window's content: the fixture on show, or a start panel when none is open. Fixture
/// files can be dropped onto it.
struct ViewerView: View {
	@ObservedObject var model: ViewerModel

	var body: some View {
		Group {
			if let fixture = model.fixture {
				FixturePage(model: model, fixture: fixture)
			} else {
				StartPanel(model: model)
			}
		}
		.frame(minWidth: 900, minHeight: 600)
		.background(Color(nsColor: .windowBackgroundColor))
		.onDrop(of: [UTType.fileURL], isTargeted: nil) { providers in
			guard let provider = providers.first else {
				return false
			}
			_ = provider.loadObject(ofClass: URL.self) { url, _ in
				if let url {
					DispatchQueue.main.async {
						model.requestOpen(url)
					}
				}
			}
			return true
		}
	}
}

/// A fixture: the summary with the file buttons, then the tabs, and the progress strip while a
/// simulation runs.
private struct FixturePage: View {
	@ObservedObject var model: ViewerModel
	let fixture: FixtureHandle

	var body: some View {
		VStack(alignment: .leading, spacing: 12) {
			HStack(alignment: .top, spacing: 12) {
				SummaryView(model: model, fixture: fixture)
				Button("Open…") { model.requestOpen() }
					.disabled(model.isBusy)
				Button("Save") { model.requestSave() }
					.disabled(model.isBusy)
			}
			TabView(selection: $model.tab) {
				SquadsTab(model: model, fixture: fixture)
					.tabItem { Text("Squads") }
					.tag(ViewerModel.Tab.squads)
				VenueTab(model: model, fixture: fixture)
					.tabItem { Text("Venue") }
					.tag(ViewerModel.Tab.venue)
				SimulationTab(model: model)
					.tabItem { Text("Simulation") }
					.tag(ViewerModel.Tab.simulation)
			}
			if let activity = model.activity {
				ActivityLine(text: activity, detail: model.activityDetail)
			}
			if model.simulation.running {
				SimulationProgressBar(state: model.simulation)
			}
		}
		.padding(12)
	}
}

/// What a harness job is doing, with a spinner, and fm24ctl's latest progress while it starts
/// FM24 for the job.
private struct ActivityLine: View {
	let text: String
	var detail: String?

	var body: some View {
		HStack(spacing: 8) {
			ProgressView()
				.controlSize(.small)
			Text(text)
				.foregroundColor(.secondary)
			if let detail {
				Text(detail)
					.font(.system(.callout, design: .monospaced))
					.foregroundColor(.secondary)
					.lineLimit(1)
					.truncationMode(.middle)
					.help(detail)
			}
		}
		.accessibilityElement(children: .combine)
	}
}

/// What the window shows with no fixture open: the startup export (or a binary fixture's decode)
/// as it runs, with Cancel, or why it (or the first open) failed or stopped, with Retry; Install
/// Harness… while Steam's launch option is not set; and Launch FM24…, which starts the game with
/// the harness and a save.
private struct StartPanel: View {
	@ObservedObject var model: ViewerModel

	var body: some View {
		VStack(spacing: 12) {
			Text("No fixture is open")
				.font(.title2)
			if let activity = model.activity {
				ActivityLine(text: activity)
				if model.canCancelStart {
					Button("Cancel") { model.cancelStart() }
						.keyboardShortcut(.cancelAction)
				}
			} else if !model.status.isEmpty {
				Text(model.status)
					.multilineTextAlignment(.center)
					.textSelection(.enabled)
					.fixedSize(horizontal: false, vertical: true)
					.frame(maxWidth: 640)
				if let monitor = model.monitor {
					GameLine(monitor: monitor)
				}
			}
			Text("Open a .json or .bin fixture, or drop one on this window.")
				.foregroundColor(.secondary)
			// Retry once the startup export or first open has failed.
			let retries = model.retry != nil && !model.isBusy
			HStack(spacing: 12) {
				if retries {
					Button("Retry") { model.retryStart() }
						.keyboardShortcut(.defaultAction)
				}
				Button("Open…") { model.requestOpen() }
					.disabled(model.isBusy)
					.keyboardShortcut(retries ? nil : .defaultAction)
				if let monitor = model.monitor {
					InstallButton(model: model, monitor: monitor)
				}
				Button("Launch FM24…") { model.requestLaunch() }
					.disabled(model.isBusy)
			}
		}
		.padding(24)
		.frame(maxWidth: .infinity, maxHeight: .infinity)
	}
}

/// Install Harness… before Launch FM24… while Steam's launch option is not set: the tested route,
/// as Launch FM24… would then start the game directly.
private struct InstallButton: View {
	@ObservedObject var model: ViewerModel
	@ObservedObject var monitor: GameMonitor

	var body: some View {
		if !monitor.optionSet {
			Button("Install Harness…") { model.requestInstall() }
				.disabled(model.isBusy)
		}
	}
}

/// What the start panel knows of the game.
private struct GameLine: View {
	@ObservedObject var monitor: GameMonitor

	var body: some View {
		if let text = monitor.description {
			Text(text)
				.foregroundColor(.secondary)
				.multilineTextAlignment(.center)
				.fixedSize(horizontal: false, vertical: true)
				.frame(maxWidth: 640)
		}
	}
}
