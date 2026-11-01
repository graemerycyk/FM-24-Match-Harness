// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import FixtureCore
import FixtureSamples
import Foundation

/// FixtureHandle: the C core as the views see it.
@MainActor
enum FixtureHandleTests {
	static func register() {
		Tests.test("fixture handle summarises a fixture", summarises)
		Tests.test("fixture handle says why a fixture cannot be opened", refuses)
		Tests.test("fixture handle lists squads and slots", listsSquads)
		Tests.test("fixture handle reads text of any length", readsLongText)
		Tests.test("fixture handle shows the venue", showsVenue)
		Tests.test("fixture handle writes what a save writes", writes)
		Tests.test("fixture handle keeps a new line-up as fixture_view.h does", ordersLineup)
		Tests.test("player changes carry the Max flags and new orders", carriesChanges)
	}

	static func summarises() throws {
		let fixture = try FixtureHandle(json: sampleJSON(FIXTURE_SAMPLE_LABELLED))
		checkEqual(
			fixture.summaryLines,
			[
				"Friendly @ Villa Park, 2023-07-05 19:30",
				"Weather: Unknown (serialized weather fields have not been mapped)",
				"Referee: Aimee Keir",
			]
		)
		checkEqual(fixture.heading(side: 0), "Home: Aston Villa - Custom 4-2-2-2 [4-2-2-2]")
		checkEqual(fixture.heading(side: 1), "Away: Bristol City - Custom 4-2-2-2 [4-2-2-2]")
		checkEqual(fixture.teamName(side: 1), "Bristol City")
		// HARNESS_PROTOCOL_TEXT, which fixture_view_test.c checks: a dotted version.
		let parts = FixtureHandle.protocolVersion.split(
			separator: ".",
			omittingEmptySubsequences: false
		)
		check(parts.count == 3 && parts.allSatisfy { !$0.isEmpty && $0.allSatisfy(\.isNumber) })
		checkEqual(FixtureHandle.notes.map(\.labelLength), [5, 7])
		check(FixtureHandle.notes[0].text.hasPrefix("NOTE: Condition/sharpness are percentages."))
		check(FixtureHandle.notes[1].text.hasPrefix("NOTE 2: AI/unknown control"))
	}

	static func refuses() {
		for json in [
			Data(), Data("{}".utf8), Data("{\"format\":\"fm24-match-config/1\",\"fields\":[]}".utf8),
		] {
			do {
				_ = try FixtureHandle(json: json)
				check(false, "\(String(decoding: json, as: UTF8.self)) opened")
			} catch let error as ViewerError {
				check(!error.message.isEmpty)
			} catch {
				check(false, "unexpected \(error)")
			}
		}
		// Home blocks 10 and 19 exchanged without being renamed, as fm24ctl lineup refuses them.
		let moved = String(decoding: sampleJSON(FIXTURE_SAMPLE_LINEUP), as: UTF8.self)
			.replacingOccurrences(of: "\"home.player[10]", with: "\"home.player[moved]")
			.replacingOccurrences(of: "\"home.player[19]", with: "\"home.player[10]")
			.replacingOccurrences(of: "\"home.player[moved]", with: "\"home.player[19]")
		do {
			_ = try FixtureHandle(json: Data(moved.utf8))
			check(false, "a fixture with moved blocks opened")
		} catch let error as ViewerError {
			checkEqual(
				error.message,
				"home.player[19] is in place 10: its player blocks were moved without renaming them. "
					+ "Use fm24ctl lineup to move players."
			)
		} catch {
			check(false, "unexpected \(error)")
		}
	}

	static func listsSquads() throws {
		let fixture = try FixtureHandle(json: sampleJSON(FIXTURE_SAMPLE_LABELLED))
		checkEqual(FixtureHandle.columnCount, 16)
		checkEqual(
			(0..<16).map(FixtureHandle.columnWidth),
			[38, 210, 65, 185, 75, 65, 85, 90, 70, 85, 100, 125, 105, 85, 85, 85]
		)
		checkEqual(FixtureHandle.columnTitle(11), "Creative Freedom")
		checkEqual(fixture.rowCount(side: 0), 12)
		checkEqual(fixture.rowCount(side: 1), 23)
		checkEqual(
			(0..<16).map { fixture.cell(side: 0, row: 0, column: $0) },
			[
				"0", "Emiliano Martínez", "GK", "Sweeper Keeper", "Defend", "12", "98.76%",
				"81.25 / 56.50%", "-1",
				"-", "-", "-", "-", "-", "-", "-",
			]
		)
		checkEqual(fixture.cell(side: 0, row: 11, column: 2), "Sub")
		checkEqual(fixture.cell(side: 1, row: 4, column: 1), "<Unknown>")
		checkEqual(fixture.rowBlock(side: 0, row: 11), 11)
		checkEqual(fixture.rowBlock(side: 1, row: 4), nil)
		checkEqual(fixture.rowBlock(side: 1, row: 11), 0)
		checkEqual(fixture.cell(side: 1, row: 23, column: 1), "")
		check(fixture.slotSpot(side: 0, slot: 0).map { $0.x == 0.5 && $0.y == 0.91 } == true)
		check(fixture.slotSpot(side: 0, slot: 11) == nil)
		check(!fixture.hasPlayerEdits)
		checkEqual(fixture.playerEdits, [UInt8](repeating: 0, count: 512))
	}

	static func readsLongText() throws {
		let name = String(repeating: "Ł", count: 200)
		let json = replacing(
			sampleJSON(FIXTURE_SAMPLE_LABELLED),
			"\"str\":\"Aston Villa\"",
			with: "\"str\":\"\(name)\""
		)
		let fixture = try FixtureHandle(json: json)
		checkEqual(fixture.teamName(side: 0), name)
		checkEqual(fixture.heading(side: 0), "Home: \(name) - Custom 4-2-2-2 [4-2-2-2]")
	}

	static func showsVenue() throws {
		let fixture = try FixtureHandle(json: sampleJSON(FIXTURE_SAMPLE_VENUE))
		let venue = fixture.venue
		checkEqual(venue.count, 11)
		check(FixtureHandle.hasVenue(venue))
		checkEqual(
			(0..<11).map { FixtureHandle.venueText($0, venue[$0]) },
			[
				"67.5999985", "105.099998", "-0.944999993", "52.4860229", "42777", "42000", "30000", "2000",
				"30000", "Grass/Synthetic mix", "200",
			]
		)
		checkEqual(FixtureHandle.venueLabel(9), "Surface:")
		checkEqual(FixtureHandle.surfaceNames, ["Grass", "Grass/Synthetic mix"])
		checkEqual(FixtureHandle.surfaceChoice(venue[FixtureHandle.surfaceField]), 1)
		check((0..<11).allSatisfy(fixture.venueEditable))
		let labelled = try FixtureHandle(json: sampleJSON(FIXTURE_SAMPLE_LABELLED))
		checkEqual(FixtureHandle.venueText(0, labelled.venue[0]), "")
		check(!(0..<11).contains(where: labelled.venueEditable))
		check(FixtureHandle.hasVenue(labelled.venue))
		check(!FixtureHandle.hasVenue([FixtureViewNumber](repeating: FixtureViewNumber(), count: 11)))
	}

	static func writes() throws {
		let json = sampleJSON(FIXTURE_SAMPLE_VENUE)
		let venue = try FixtureHandle(json: json).venue
		checkEqual(
			try FixtureHandle.write(json: json, changes: nil, venue: nil, binary: false),
			json
		)
		let edited = try FixtureHandle.write(json: json, changes: nil, venue: venue, binary: false)
		check(edited != json, "the venue write recomputes the derived values")
		let binary = try FixtureHandle.write(json: json, changes: nil, venue: venue, binary: true)
		checkEqual(
			try FixtureHandle.write(json: edited, changes: nil, venue: nil, binary: true),
			binary
		)
		var flags = [UInt8](repeating: 0, count: 512)
		flags[12] = 1
		do {
			_ = try FixtureHandle.write(
				json: sampleJSON(FIXTURE_SAMPLE_LABELLED),
				changes: PlayerChanges(flags: flags),
				venue: nil,
				binary: false
			)
			check(false, "an edit of a missing player was written")
		} catch let error as ViewerError {
			checkEqual(error.message, "Cannot edit home.player[12].")
		}
	}

	static func ordersLineup() throws {
		let json = lineupSampleJSON()
		let fixture = try FixtureHandle(json: json)
		check(fixture.lineupKnown(side: 0) && !fixture.lineupKnown(side: 1))
		check(fixture.lineupEditable(side: 0) && !fixture.lineupEditable(side: 1))
		check(!fixture.lineupMarked(side: 0) && !fixture.lineupEdited(side: 0))
		checkEqual([0, 1].map(fixture.benchSize), [9, 2])
		checkEqual(fixture.order(side: 0), Array(0..<FixtureHandle.blockCount))
		checkEqual(fixture.playerChanges, PlayerChanges())
		checkEqual(fixture.personId(side: 0, block: 19), 91_193_048)
		checkEqual(fixture.personId(side: 0, block: 22), nil)
		checkEqual(fixture.personId(side: 0, block: -1), nil)
		checkEqual(fixture.positionRating(side: 0, block: 10, slot: 10), 7)
		checkEqual(fixture.positionRating(side: 0, block: 19, slot: 10), 16)
		checkEqual(fixture.positionRating(side: 0, block: 19, slot: 11), nil)
		checkEqual(fixture.positionRating(side: 0, block: -1, slot: 0), nil)
		checkEqual(
			fixture.lineupWarnings(side: 0, order: exchangedPlaces(10, 11)),
			[
				"Caoimhin Kelleher, a goalkeeper, is in slot 10 (ST).",
				"No goalkeeper among the substitutes.",
			]
		)
		checkEqual(fixture.lineupWarnings(side: 0, order: [0, 0]), [])
		checkEqual(fixture.lineupWarnings(side: 0, order: [300]), [])

		// Only an order that places each of the side's players once, on a side it can change.
		check(!fixture.setOrder(side: 0, [1, 1] + Array(2..<22)))
		check(!fixture.setOrder(side: 0, [300]))
		check(!fixture.setOrder(side: 0, [-1]))
		check(!fixture.setOrder(side: 1, exchangedPlaces(0, 1, count: 13)))
		check(!fixture.hasPlayerEdits)
		check(fixture.setOrder(side: 0, exchangedPlaces(10, 19)))
		check(fixture.lineupEdited(side: 0) && fixture.hasPlayerEdits)
		checkEqual(fixture.placeOf(side: 0, block: 19), 10)
		checkEqual(fixture.placeOf(side: 0, block: 10), 19)
		checkEqual(fixture.placeOf(side: 0, block: 3), 3)
		checkEqual(fixture.placeOf(side: 0, block: 22), nil)
		checkEqual(fixture.placeOf(side: 0, block: -1), nil)
		checkEqual(fixture.playerChanges, PlayerChanges(lineup: exchangedOrder(10, 19)))
		// A shorter order leaves the rest in their places.
		check(fixture.setOrder(side: 0, [1, 0]))
		checkEqual(fixture.playerChanges.lineup, exchangedOrder(0, 1))
		check(fixture.setOrder(side: 0, []))
		check(!fixture.lineupEdited(side: 0))
		checkEqual(fixture.playerChanges, PlayerChanges())

		// Written, the blocks are renamed and the header marked; committed, the view follows.
		check(fixture.setOrder(side: 0, exchangedPlaces(10, 19)))
		let written = try FixtureHandle.write(
			json: json,
			changes: fixture.playerChanges,
			venue: nil,
			binary: false
		)
		checkEqual(occurrences("\"home_lineup\": true", in: written), 1)
		checkEqual(
			try FixtureHandle.write(json: written, changes: nil, venue: nil, binary: true).count,
			try FixtureHandle.write(json: json, changes: nil, venue: nil, binary: true).count
		)
		fixture.commitOrder()
		check(!fixture.lineupEdited(side: 0) && fixture.lineupMarked(side: 0))
		checkEqual(fixture.playerName(side: 0, block: 10), "Florian Wirtz")
		checkEqual(fixture.personId(side: 0, block: 10), 91_193_048)
		let reopened = try FixtureHandle(json: written)
		check(reopened.lineupMarked(side: 0) && !reopened.lineupEdited(side: 0))
		checkEqual(reopened.playerName(side: 0, block: 10), "Florian Wirtz")
	}

	static func carriesChanges() {
		let blocks = FixtureHandle.blockCount
		var changes = PlayerChanges()
		checkEqual(changes.flags, [UInt8](repeating: 0, count: 2 * blocks))
		check(changes.isEmpty && !changes.movesPlayers)
		// An order that moves nobody is no change.
		changes.lineup = exchangedOrder(4, 4)
		check(changes.isEmpty && !changes.movesPlayers)
		changes.lineup = exchangedOrder(0, 1, side: 1)
		check(!changes.isEmpty && changes.movesPlayers)
		changes.lineup = exchangedOrder(10, 19)
		check(!changes.isEmpty)
		changes.flags[3] = 1
		changes.flags[blocks + 4] = 2
		// A team replacement drops the side's flags and order; a swap, the player's flags.
		let home = changes.dropping(side: 0)
		checkEqual(home.lineup, nil)
		checkEqual([home.flags[3], home.flags[blocks + 4]], [0, 2])
		let away = changes.dropping(side: 1)
		checkEqual(away.lineup, exchangedOrder(10, 19))
		checkEqual([away.flags[3], away.flags[blocks + 4]], [1, 0])
		var both = changes
		both.lineup?.swapAt(blocks, blocks + 1)
		checkEqual(both.dropping(side: 0).lineup, exchangedOrder(0, 1, side: 1))
		let swapped = changes.dropping(side: 0, block: 3)
		checkEqual(swapped.lineup, changes.lineup)
		checkEqual([swapped.flags[3], swapped.flags[blocks + 4]], [0, 2])
		checkEqual(ReplacementJob.team(side: 0, team: "677").carriedEdits(changes), home)
		checkEqual(ReplacementJob.team(side: 1, team: "677").carriedEdits(changes), away)
		checkEqual(
			ReplacementJob.swap(side: 0, block: 3, person: "1", allowDuplicate: false)
				.carriedEdits(changes),
			swapped
		)
		check(PlayerChanges(flags: home.flags).isEmpty == false)
	}
}
