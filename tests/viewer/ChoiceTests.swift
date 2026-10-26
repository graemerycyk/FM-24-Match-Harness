// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// The team and player lists and their searches, mirroring the Windows viewer's tests
/// (checkTeamPicker and checkPlayerSearch in tests/fixture_window_test.c), with the lists the
/// stand-in writes.
@MainActor
enum ChoiceTests {
	static func register() {
		Tests.test("names fold as the Windows viewer folds them", folds)
		Tests.test("matches rank as the Windows viewer ranks them", ranks)
		Tests.test("team lists are searched as on Windows", searchesTeams)
		Tests.test("player lists are searched by name as on Windows", searchesPlayers)
		Tests.test("lists show at most 200 matches, start matches first", limitsMatches)
		Tests.test("lists sort as Windows sorts words, ignoring hyphens and apostrophes", sorts)
	}

	static let teamList = Data(
		"677\tLuton Town\n12\tAston Villa\r\n99\tUnited\n691\tNorwich City\n98\tUnited\nbad\n".utf8
	)
	/// Five players: one with a common name, one without a club; then a line with too few fields, a
	/// player without names and a malformed line, which are skipped.
	static let playerList = Data(
		("40347\tMartin\tØdegaard\t\tArsenal\n1001\tEdson\tArantes do Nascimento\tPelé\t\n"
			+ "2002\tThomas\tMüller\t\tFC Bayern\n3003\tŁukasz\tFabiański\t\tWest Ham\n"
			+ "5005\tJan\tBodegaard\t\t\r\n6006\tToo\tFew\n7007\t\t\t\t\nbad\n").utf8
	)
	static let odegaard = "Martin Ødegaard – Arsenal (40347)"
	static let pele = "Pelé (Edson Arantes do Nascimento) (1001)"

	private static func folded(_ text: String) -> String? {
		ChoiceSearch.fold(text).map { String(decoding: $0, as: UTF16.self) }
	}

	private static func labels(_ choices: [Choice], _ text: String, _ kind: ChoiceKind) -> [String] {
		ChoiceSearch.matches(choices, text, kind).map(\.label)
	}

	static func folds() {
		checkEqual(folded("ÆßŒéŁ"), "aessoeel")
		// Too long for the Windows viewer's buffer: not folded.
		checkEqual(ChoiceSearch.fold("Æß", limit: 4), nil)
		checkEqual(ChoiceSearch.fold("Æß", limit: 5)?.count, 4)
		checkEqual(folded("Martin Ødegaard"), "martin odegaard")
		checkEqual(folded("Þór Đorđević Ħal-Lija ıIİ"), "thor dordevic hal-lija iii")
		checkEqual(folded("Müller Fabiański Pelé Çağlar"), "muller fabianski pele caglar")
		checkEqual(folded("ASTON Villa (12)"), "aston villa (12)")
		checkEqual(folded(""), "")
	}

	static func ranks() {
		func rank(_ text: String, _ part: String, words: Bool) -> Int? {
			ChoiceSearch.rank(
				ChoiceSearch.fold(text) ?? [],
				ChoiceSearch.fold(part) ?? [],
				words: words
			)
		}
		checkEqual(rank("Ødegaard", "ode", words: false), 0)
		checkEqual(rank("Jan Bodegaard", "ode", words: true), 1)
		checkEqual(rank("Bodegaard Jan", "jan", words: true), 0)
		checkEqual(rank("Bodegaard Jan", "jan", words: false), 1)
		checkEqual(rank("Jean-Pierre", "pierre", words: true), 0)
		// A later occurrence at a word's start still counts as one.
		checkEqual(rank("Odd Codd Odd", "odd", words: true), 0)
		checkEqual(rank("Codd Xodd Odd", "odd", words: true), 0)
		checkEqual(rank("Luton Town (677)", "u", words: false), 1)
		checkEqual(rank("Luton", "Bristol", words: false), nil)
		checkEqual(rank("Lu", "Luton", words: false), nil)
		checkEqual(rank("Luton", "", words: false), 0)
	}

	static func searchesTeams() throws {
		let teams = try ChoiceSearch.parse(teamList, .teams).get()
		let all = [
			"Aston Villa (12)", "Luton Town (677)", "Norwich City (691)", "United (98)", "United (99)",
		]
		checkEqual(teams.map(\.label), all)
		checkEqual(teams.map(\.id), [12, 677, 691, 98, 99])
		checkEqual(labels(teams, "", .teams), all)
		// Names starting with the text come first; IDs match too.
		checkEqual(labels(teams, "U", .teams), ["United (98)", "United (99)", "Luton Town (677)"])
		checkEqual(labels(teams, "67", .teams), ["Luton Town (677)"])
		checkEqual(labels(teams, "Bristol", .teams), [])
		checkEqual(labels(teams, "Aston Villa", .teams), ["Aston Villa (12)"])
		// A label picked from the list gives its ID, so that a shared name still resolves.
		checkEqual(ChoiceSearch.listedID(teams, "United (99)"), 99)
		checkEqual(ChoiceSearch.listedID(teams, "United"), nil)
		// A list with nothing to offer is refused.
		checkEqual(
			ChoiceSearch.parse(Data("bad\n\n12 Aston Villa\n".utf8), .teams).map(\.count),
			.failure(ViewerError(message: "the team list is empty"))
		)
		checkEqual(
			ChoiceSearch.parse(Data(), .players).map(\.count),
			.failure(ViewerError(message: "the player list is empty"))
		)
	}

	static func sorts() throws {
		// CompareStringW's word sort gives "-" and "'" no weight, so that "co-op" stays with "coop";
		// spaces come before letters, and case and accents count for less.
		let list = [
			"1\tAl-Wahda", "2\tAlaves", "3\tAl Ain", "4\tCo-op", "5\tCoop", "6\tO'Higgins",
			"7\tOadby Town", "8\tÁlava", "9\tAlb",
		]
		let teams = try ChoiceSearch.parse(Data(list.joined(separator: "\n").utf8), .teams).get()
		checkEqual(
			teams.map(\.label),
			[
				"Al Ain (3)", "Álava (8)", "Alaves (2)", "Alb (9)", "Al-Wahda (1)", "Co-op (4)",
				"Coop (5)", "Oadby Town (7)", "O'Higgins (6)",
			]
		)
		let players = try ChoiceSearch.parse(
			Data("1\tGolo\tKanté\tN'Golo Kanté\tX\n2\tNabil\tFekir\t\tY\n".utf8),
			.players
		).get()
		checkEqual(players.map(\.id), [2, 1])
		// Labels equal but for those marks: the one without them first.
		check(
			ChoiceSearch.sortKey("Coop").lexicographicallyPrecedes(ChoiceSearch.sortKey("Co-op")),
			"co-op before coop"
		)
	}

	static func searchesPlayers() throws {
		let players = try ChoiceSearch.parse(playerList, .players).get()
		checkEqual(players.count, 5)
		checkEqual(
			Set(players.map(\.label)),
			[
				odegaard, pele, "Thomas Müller – FC Bayern (2002)", "Łukasz Fabiański – West Ham (3003)",
				"Jan Bodegaard (5005)",
			]
		)
		// Fewer than three characters list nothing.
		checkEqual(labels(players, "od", .players), [])
		// Accents are ignored either way; a name starting with the text comes first.
		checkEqual(labels(players, "Ode", .players), [odegaard, "Jan Bodegaard (5005)"])
		checkEqual(labels(players, "øDE", .players), [odegaard, "Jan Bodegaard (5005)"])
		checkEqual(labels(players, "muller", .players), ["Thomas Müller – FC Bayern (2002)"])
		checkEqual(labels(players, "lukasz fab", .players), ["Łukasz Fabiański – West Ham (3003)"])
		// The common name leads the label, and it or any name can be searched; clubs cannot.
		checkEqual(labels(players, "pele", .players), [pele])
		checkEqual(labels(players, "nasc", .players), [pele])
		checkEqual(labels(players, "arsenal", .players), [])
		checkEqual(labels(players, "Nobody", .players), [])
		checkEqual(ChoiceSearch.listedID(players, odegaard), 40347)
		checkEqual(ChoiceSearch.listedID(players, "Martin Odegaard"), nil)
	}

	static func limitsMatches() throws {
		// 150 teams named "Zeta …", sorted first by an earlier "Alpha", then 150 "… Zeta".
		var lines = (0..<150).map { "\($0 + 1)\tZeta \(1000 + $0)" }
		lines += (0..<150).map { "\($0 + 2001)\tAlpha Zeta \(1000 + $0)" }
		let teams = try ChoiceSearch.parse(Data(lines.joined(separator: "\n").utf8), .teams).get()
		checkEqual(teams.count, 300)
		checkEqual(labels(teams, "", .teams).count, 200)
		checkEqual(labels(teams, "", .teams).first, "Alpha Zeta 1000 (2001)")
		let zeta = labels(teams, "zeta", .teams)
		checkEqual(zeta.count, 200)
		checkEqual(zeta.first, "Zeta 1000 (1)")
		checkEqual(zeta[149], "Zeta 1149 (150)")
		checkEqual(zeta[150], "Alpha Zeta 1000 (2001)")
	}
}
