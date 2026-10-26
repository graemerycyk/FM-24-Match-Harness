// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import SwiftUI

/// The Simulation tab, laid out as the Windows tab: the thread count with its stepper, the seed
/// pattern, the two options and Simulate fixture (the default button, so Return starts it), then
/// the run's summary and status, and where the last run's results are.
struct SimulationTab: View {
	@ObservedObject var model: ViewerModel

	var body: some View {
		let form = model.simulationForm
		let editable = model.simulationEditable
		VStack(alignment: .leading, spacing: 14) {
			Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 14) {
				GridRow {
					Text(SimulationForm.threadsLabel)
						.frame(width: 180, alignment: .leading)
					HStack(spacing: 2) {
						SimulationTextField(model: model, field: .threads)
							.frame(width: 58)
						Stepper(
							"Number of threads",
							onIncrement: { model.stepThreads(by: 1) },
							onDecrement: { model.stepThreads(by: -1) }
						)
						.labelsHidden()
					}
				}
				GridRow {
					Text("Seed pattern (optional):")
						.frame(width: 180, alignment: .leading)
					SimulationTextField(model: model, field: .seeds)
						.frame(width: 220)
				}
			}
			.disabled(!editable)
			VStack(alignment: .leading, spacing: 10) {
				Toggle(
					"Generate detailed reports?",
					isOn: Binding(get: { form.detailed }, set: { model.setDetailed($0) })
				)
				Toggle(
					"Skip rebuild",
					isOn: Binding(get: { form.keepBuild }, set: { model.setKeepBuild($0) })
				)
			}
			.toggleStyle(.checkbox)
			.disabled(!editable)
			Button("Simulate fixture") { model.simulate() }
				.keyboardShortcut(.defaultAction)
				.disabled(!model.canSimulate)
				.padding(.top, 4)
			VStack(alignment: .leading, spacing: 10) {
				Text(model.simulation.summary)
					.textSelection(.enabled)
				Text(model.simulationStatus)
					.fixedSize(horizontal: false, vertical: true)
				if !model.simulation.running, let results = model.simulation.results.first {
					ResultsLine(model: model, results: results)
				}
			}
			.frame(maxWidth: 620, alignment: .leading)
			.padding(.top, 8)
		}
		.padding(12)
		.frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
	}
}

/// Where the last run's results are, with Show in Finder.
private struct ResultsLine: View {
	@ObservedObject var model: ViewerModel
	let results: URL

	var body: some View {
		HStack(spacing: 8) {
			Text((results.path as NSString).abbreviatingWithTildeInPath)
				.lineLimit(1)
				.truncationMode(.middle)
				.textSelection(.enabled)
				.help(results.path)
			Button("Show in Finder") { model.revealResults() }
				.fixedSize()
		}
	}
}

/// The progress strip along the bottom of the window while a simulation runs: indeterminate until
/// results arrive, then how many of the matches have been played.
struct SimulationProgressBar: View {
	let state: SimulationState

	var body: some View {
		Group {
			if let fraction = state.fraction {
				ProgressView(value: fraction)
			} else {
				ProgressView()
			}
		}
		.progressViewStyle(.linear)
		.accessibilityLabel("Simulation progress")
		.accessibilityValue(state.summary)
	}
}

/// The thread count's or seed pattern's entry: an AppKit field, which holds typing to the Windows
/// fields' length (the thread count to digits).
private struct SimulationTextField: NSViewRepresentable {
	@ObservedObject var model: ViewerModel
	let field: SimulationForm.Field

	func makeCoordinator() -> Coordinator {
		Coordinator(self)
	}

	func makeNSView(context: Context) -> NSTextField {
		let text = NSTextField(string: "")
		let threads = field == .threads
		text.formatter = LengthLimit(
			threads ? SimulationForm.threadsLimit : SimulationForm.seedsLimit,
			digitsOnly: threads
		)
		text.placeholderString = threads ? nil : SimulationForm.seedsPlaceholder
		text.usesSingleLineMode = true
		text.lineBreakMode = .byClipping
		text.delegate = context.coordinator
		text.setAccessibilityLabel(threads ? "Number of threads" : "Seed pattern")
		return text
	}

	func updateNSView(_ text: NSTextField, context: Context) {
		context.coordinator.parent = self
		let form = model.simulationForm
		let value = field == .threads ? form.threads : form.seeds
		if text.stringValue != value {
			text.stringValue = value
		}
		let field = self.field
		let model = self.model
		let request = model.simulationFocus?.field == field ? model.simulationFocus : nil
		context.coordinator.focus.update(
			text,
			serial: request?.serial,
			current: { model.simulationFocus?.field == field ? model.simulationFocus?.serial : nil },
			done: {
				if let request {
					model.focusedSimulation(request)
				}
			}
		)
	}

	@MainActor
	final class Coordinator: NSObject, NSTextFieldDelegate {
		var parent: SimulationTextField
		let focus = FocusTaker()

		init(_ parent: SimulationTextField) {
			self.parent = parent
		}

		func controlTextDidChange(_ notification: Notification) {
			guard let text = notification.object as? NSTextField else {
				return
			}
			switch parent.field {
			case .threads: parent.model.setThreads(text.stringValue)
			case .seeds: parent.model.setSeeds(text.stringValue)
			}
		}
	}
}
