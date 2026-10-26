// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// One side on the Squads tab: its heading (team, tactic and formation) with the pencil that
/// replaces the team, the Max buttons and Swap player for the selected players, the stored
/// formation on a pitch, and the squad list.
struct TeamPanel: View {
	@ObservedObject var model: ViewerModel
	let fixture: FixtureHandle
	let side: Int

	var body: some View {
		VStack(alignment: .leading, spacing: 6) {
			HStack(spacing: 6) {
				// The heading gives way to the buttons.
				let heading = fixture.heading(side: side)
				Text(heading)
					.font(.system(size: 15, weight: .semibold))
					.lineLimit(1)
					.truncationMode(.tail)
					.help(heading)
				// SwiftUI sets the pencil's enabled state too.
				PencilButton(model: model, side: side)
					.disabled(!model.canEditTeam(side: side))
					.frame(width: 18, height: 18)
				Spacer(minLength: 12)
				ForEach(PlayerEdit.allCases, id: \.self) { edit in
					Button(edit.title) { model.maxPlayers(edit, side: side) }
						.disabled(!model.canEditPlayers(side: side))
						.fixedSize()
						.accessibilityLabel("\(edit.title), \(side == 0 ? "home" : "away") team")
				}
				Button("Swap player") { model.requestSwap(side: side) }
					.disabled(!model.canSwap(side: side))
					.fixedSize()
					.accessibilityLabel("Swap player, \(side == 0 ? "home" : "away") team")
			}
			.frame(minHeight: 27)
			HStack(alignment: .top, spacing: 12) {
				PitchView(fixture: fixture, side: side)
					.frame(width: 220)
				SquadTable(model: model, side: side)
			}
		}
	}
}

/// The Squads tab: the home side above the away side.
struct SquadsTab: View {
	@ObservedObject var model: ViewerModel
	let fixture: FixtureHandle

	var body: some View {
		VStack(spacing: 12) {
			ForEach(0..<FixtureHandle.sideCount, id: \.self) { side in
				TeamPanel(model: model, fixture: fixture, side: side)
					.frame(maxHeight: .infinity)
			}
		}
		.padding(12)
	}
}
