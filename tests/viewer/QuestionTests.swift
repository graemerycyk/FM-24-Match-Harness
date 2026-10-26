// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// What waits while a question, panel or the swap prompt is open, as everything waits behind a
/// modal dialog on Windows: the prompts here answer later, as sheets do.
@MainActor
enum QuestionTests {
	static func register() {
		Tests.test("nothing starts while a question or panel waits", waitsForQuestions)
		Tests.test("nothing starts while the swap prompt is open", waitsForSwapPrompt)
	}

	/// Opens the plain sample, selects away block 2, and has questions wait for their answers.
	private static func openSample(_ setup: HarnessSetup) -> URL {
		let url = setup.scratch.file("next.json", plainSampleJSON())
		setup.model.open(url)
		check(waitUntil { !setup.model.isBusy }, "the open did not finish")
		setup.model.select(side: 1, blocks: [2])
		setup.prompts.deferring = true
		return url
	}

	/// Tries everything that starts or changes something, and checks none of it did.
	private static func tryEverything(_ setup: HarnessSetup, _ other: URL) {
		let model = setup.model
		let errors = setup.prompts.errors.count
		let questions = setup.prompts.questions.count
		let edited = model.fixture?.hasPlayerEdits
		model.simulate()
		model.requestOpen(other)
		model.requestOpen()
		model.open(other)
		model.requestSave()
		model.save(to: setup.scratch.file("saved.json"), binary: false)
		model.maxPlayers(.happiness, side: 1)
		model.editTeam(side: 0)
		model.requestExportFolder()
		checkEqual(
			setup.prompts.errors.dropFirst(errors).map(\.message),
			Array(repeating: ViewerModel.askingMessage, count: 6)
		)
		checkEqual(setup.prompts.questions.count, questions)
		check(setup.prompts.offered == nil && setup.prompts.offeredFolder == nil)
		check(!model.isBusy && !model.simulation.running)
		check(setup.standIn.runs.allSatisfy { $0.first == "list-players" }, "\(setup.standIn.runs)")
		checkEqual(model.fixture?.hasPlayerEdits, edited)
		check(model.teamEditor == nil)
		checkEqual(model.source?.name, "next.json")
	}

	static func waitsForQuestions() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		_ = openSample(setup)
		let other = setup.scratch.file("other.json", plainSampleJSON())
		model.maxPlayers(.happiness, side: 1)
		check(model.hasUnsavedEdits && !model.isAsking)

		// The close question: answering Discard closes, since nothing could start meanwhile.
		var closes: [Bool] = []
		model.confirmClose { closes.append($0) }
		check(model.isAsking)
		tryEverything(setup, other)
		setup.prompts.answer(true)
		checkEqual(closes, [true])
		check(!model.isAsking)

		// The Open panel, and the question about discarding edits it leads to.
		model.requestOpen()
		check(model.isAsking)
		tryEverything(setup, other)
		setup.prompts.choose(other)
		check(model.isAsking, "the discard question")
		checkEqual(setup.prompts.questions.last?.title, ViewerModel.openTitle)
		tryEverything(setup, other)
		setup.prompts.answer(false)
		check(!model.isAsking)
		checkEqual(model.source?.name, "next.json")

		// Answered, everything works again.
		model.requestOpen(other)
		setup.prompts.answer(true)
		check(waitUntil { !model.isBusy }, "the open did not finish")
		checkEqual(model.source?.name, "other.json")
		check(!model.isAsking)
	}

	static func waitsForSwapPrompt() {
		guard let setup = HarnessSetup() else {
			return
		}
		let model = setup.model
		let url = openSample(setup)
		model.requestSwap(side: 1)
		guard let prompt = model.swapPrompt else {
			check(false, "no swap prompt")
			return
		}
		check(model.isAsking)
		check(waitUntil { model.players.state != .loading }, "the player list did not load")
		tryEverything(setup, setup.scratch.file("other.json", plainSampleJSON()))
		model.requestSwap(side: 1)
		check(model.swapPrompt === prompt, "a second prompt opened")
		// Confirm still swaps.
		setup.standIn.set(["FIXTURE": url.path])
		prompt.type("40347")
		model.confirmSwap()
		check(model.swapPrompt == nil && !model.isAsking)
		checkEqual(model.busy, .replacement)
		check(waitUntil { !model.isBusy }, "the swap did not finish")
		checkEqual(setup.standIn.runs.last?.first, "inject-player")
		check(model.isTemporary)
	}
}
