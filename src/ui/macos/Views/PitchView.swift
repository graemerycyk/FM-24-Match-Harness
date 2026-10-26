// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// The stored formation on a pitch, drawn as the Windows viewer draws it: a numbered circle for
/// each slot whose position is known.
struct PitchView: View {
	let fixture: FixtureHandle
	let side: Int

	private static let grass = Color(red: 32 / 255, green: 100 / 255, blue: 65 / 255)
	private static let lines = Color(red: 220 / 255, green: 236 / 255, blue: 224 / 255)
	private static let inset: CGFloat = 7

	private var slots: [(slot: Int, x: Double, y: Double)] {
		(0..<FixtureHandle.slotCount).compactMap { slot in
			fixture.slotSpot(side: side, slot: slot).map { (slot, $0.x, $0.y) }
		}
	}

	var body: some View {
		Canvas { context, size in
			draw(in: &context, size: size)
		}
		.accessibilityElement(children: .contain)
		.accessibilityLabel("Stored formation")
		.accessibilityChildren {
			ForEach(slots, id: \.slot) { spot in
				Text("Slot \(spot.slot), \(fixture.cell(side: side, row: spot.slot, column: 2))")
			}
		}
	}

	private func draw(in context: inout GraphicsContext, size: CGSize) {
		let width = size.width
		let height = size.height
		let inset = PitchView.inset
		context.fill(Path(CGRect(origin: .zero, size: size)), with: .color(PitchView.grass))
		var markings = Path()
		markings.addRect(
			CGRect(x: inset, y: inset, width: width - 2 * inset, height: height - 2 * inset)
		)
		markings.move(to: CGPoint(x: inset, y: height / 2))
		markings.addLine(to: CGPoint(x: width - inset, y: height / 2))
		let radius = width / 9
		markings.addEllipse(
			in: CGRect(
				x: width / 2 - radius,
				y: height / 2 - radius,
				width: 2 * radius,
				height: 2 * radius
			)
		)
		markings.addRect(CGRect(x: width / 4, y: inset, width: width / 2, height: height / 6 - inset))
		markings.addRect(
			CGRect(x: width / 4, y: height * 5 / 6, width: width / 2, height: height / 6 - inset)
		)
		let diameter = max(height / 12, 14)
		let spots = slots.map { (slot: $0.slot, centre: CGPoint(x: width * $0.x, y: height * $0.y)) }
		for spot in spots {
			markings.addEllipse(
				in: CGRect(
					x: spot.centre.x - diameter / 2,
					y: spot.centre.y - diameter / 2,
					width: diameter,
					height: diameter
				)
			)
		}
		context.stroke(markings, with: .color(PitchView.lines), lineWidth: 1)
		for spot in spots {
			let number = Text("\(spot.slot)")
				.font(.system(size: min(12, diameter * 0.6)))
				.foregroundColor(.white)
			context.draw(number, at: spot.centre, anchor: .center)
		}
	}
}
