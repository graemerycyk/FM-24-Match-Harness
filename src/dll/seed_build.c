// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "seed_build.h"

#include "game.h"
#include "log.h"
#include "match.h"
#include "memory.h"
#include "sites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// Every path of a serialisation split into its RVAs (outermost first), so that records' call
// chains can be compared level by level.
typedef struct Chains {
	uint32_t *rvas;
	size_t *starts;  // per path: where its RVAs begin in `rvas`
	size_t *lengths; // per path: how many there are
	size_t deepest;
} Chains;

typedef struct Side {
	const Serialised *serialised;
	Chains chains;
} Side;

// A pointer in the game's data replaced for a build with another team.
typedef struct Swap {
	uint8_t *slot;
	uint8_t *original;
	const uint8_t *replacement;
} Swap;

enum { MAX_SWAPS = 16 };

// Pairs records of the input with records of a build (see align()).
typedef struct Alignment {
	Side input;
	Side built;
	size_t *source; // per built record: the input record it pairs with, or SIZE_MAX
} Alignment;

// A possible pair of the input's player `input` with the game's build's player `game`.
typedef struct Candidate {
	size_t input;
	size_t game;
	size_t shared; // records with the same bytes
	bool sameId;
} Candidate;

static bool addSwap(
	Swap *swaps,
	size_t *count,
	uint8_t *slot,
	const uint8_t *expected,
	const uint8_t *replacement
);
static bool swapFixture(
	Swap *swaps,
	size_t *count,
	uint8_t *fixture,
	uint8_t *const original[2],
	const uint8_t *const selected[2]
);
static bool swapAttendedCopies(
	Swap *swaps,
	size_t *count,
	const uint8_t *fixture,
	uint8_t *const original[2],
	const uint8_t *const selected[2]
);
static bool swapOpponents(
	Swap *swaps,
	size_t *count,
	uint8_t *const original[2],
	const uint8_t *const selected[2]
);
static bool swapTeams(const SeedBuilder *builder, Swap *swaps, size_t *count);
static void restoreSwaps(const Swap *swaps, size_t count);
static unsigned planBuild(const SeedBuilder *builder, SeedBuilder *build);
static bool findPart(const Side *side, uint32_t part, size_t range[2]);
static uint32_t movedPath(Serialised *serialised, uint32_t path, uint32_t part);
static bool copyRecords(
	Serialised *to,
	Serialised *from,
	size_t start,
	size_t end,
	uint32_t *paths,
	const uint32_t parts[2]
);
static bool mirrorSide(Serialised *built, unsigned from);
static bool
buildSerialised(const SeedBuilder *builder, uint32_t seed, Serialised *out, bool traced);
static bool roundTrip(const ByteBuffer *bytes, Serialised *out);
static bool findEdits(
	const Side *reference,
	const Side *input,
	bool *edited,
	SeedPlayers players[2],
	char *why,
	size_t whySize
);
static bool comparePlayers(
	const Side *reference,
	const Side *input,
	size_t ends[2],
	bool *edited,
	SeedPlayers *players,
	char *why,
	size_t whySize
);
static bool pairPlayers(
	const Serialised *reference,
	const SeedPlayer *game,
	const Serialised *input,
	const SeedPlayer *list,
	size_t count,
	size_t *pairs,
	const char *team,
	char *why,
	size_t whySize
);
static size_t groupOf(
	const Serialised *reference,
	const SeedPlayer *game,
	size_t count,
	const Serialised *serialised,
	const SeedPlayer *player
);
static size_t firstUnpaired(const size_t *gameGroups, const size_t *inputGroups, size_t count);
static int compareCandidates(const void *a, const void *b);
static uint32_t playerPart(const Side *side, size_t record);
static bool startsPlayer(const Side *side, size_t record);
static bool splitPlayers(const Side *side, size_t start, SeedPlayer **players, size_t *count);
static uint32_t personAt(const Serialised *serialised, size_t record);
static bool
sameWrites(const Serialised *a, const SeedPlayer *x, const Serialised *b, const SeedPlayer *y);
static size_t
sharedRecords(const Serialised *a, const SeedPlayer *x, const Serialised *b, const SeedPlayer *y);
static bool editsPlayers(const SeedBuilder *builder, unsigned team);
static bool samePlayers(const SeedBuilder *builder, const Serialised *built, const bool teams[2]);
static size_t countObservers(const Side *side);
static bool sameLayout(const Serialised *a, const Serialised *b);
static bool sameRecord(const Serialised *a, size_t i, const Serialised *b, size_t j);
static bool appendRecord(ByteBuffer *out, const Serialised *from, size_t i);
static bool parseChains(const Serialised *serialised, Chains *chains);
static void freeChains(Chains *chains);
static uint32_t chainKey(const Side *side, size_t record, size_t depth);
static bool isObserver(const Side *side, size_t record);
static size_t skipObservers(const Side *side, size_t from, size_t to);
static bool sameSites(const Alignment *a, size_t i0, size_t i1, size_t j0, size_t j1);
static size_t runEnd(const Side *side, size_t from, size_t to, size_t depth, uint32_t key);
static bool sameRuns(const Alignment *a, size_t i0, size_t i1, size_t j0, size_t j1, size_t depth);
static void alignEnds(const Alignment *a, size_t i0, size_t i1, size_t j0, size_t j1);
static void align(const Alignment *a, size_t i0, size_t i1, size_t j0, size_t j1, size_t depth);
static bool
alignPlayers(const Alignment *a, const SeedBuilder *builder, unsigned team, uint32_t seed);
static bool
alignEdits(Alignment *alignment, const SeedBuilder *builder, const bool teams[2], uint32_t seed);
static bool
mergeEdits(const SeedBuilder *builder, const Serialised *built, uint32_t seed, ByteBuffer *out);

bool seedBuilder_open(
	SeedBuilder *builder,
	const ByteBuffer *input,
	uint32_t inputSeed,
	const uint8_t *const teams[2],
	char *error,
	size_t errorSize
) {
	*builder = (SeedBuilder){.teams = {teams ? teams[0] : NULL, teams ? teams[1] : NULL}};
	const char *why = NULL;
	if (!fixture_findNext(&builder->next, &why)) {
		(void)snprintf(error, errorSize, "no next fixture to rebuild: %s", why);
		return false;
	}
	// Compare the input with a build of its own seed, both loaded and serialised again so that the
	// serialiser's own differences (a header flag the deserialiser sets) cancel out.
	Serialised built = {0};
	Serialised reference = {0};
	Side sides[2] = {{.serialised = &reference}, {.serialised = &builder->input}};
	bool ok = buildSerialised(builder, inputSeed, &built, false) &&
						roundTrip(&built.bytes, &reference) && roundTrip(input, &builder->input) &&
						parseChains(&reference, &sides[0].chains) &&
						parseChains(&builder->input, &sides[1].chains);
	trace_free(&built);
	bool *edited = NULL; // set in the builder once it opens: seedBuilder_close() then has a lock
	if (!ok) {
		(void)snprintf(error, errorSize, "building or loading the match failed; see harness.log");
	} else {
		edited = calloc(builder->input.recordCount, sizeof *edited);
		ok = edited != NULL;
		if (!ok) {
			(void)snprintf(error, errorSize, "out of memory recording the input's edits");
		}
	}
	char reason[192] = "";
	if (ok && !findEdits(&sides[0], &sides[1], edited, builder->players, reason, sizeof reason)) {
		(void)snprintf(
			error,
			errorSize,
			"the input is not the game's next fixture (%s%s) or its layout was edited%s%s",
			builder->next.description,
			builder->teams[0] || builder->teams[1] ? ", with its team overrides" : "",
			reason[0] ? ": " : "",
			reason
		);
		ok = false;
	}
	for (size_t i = 0; ok && i < builder->input.recordCount; ++i) {
		builder->editCount += edited[i];
	}
	size_t observers[2] = {0};
	if (ok) {
		observers[0] = countObservers(&sides[0]);
		observers[1] = countObservers(&sides[1]);
	}
	freeChains(&sides[0].chains);
	freeChains(&sides[1].chains);
	trace_free(&reference);
	if (!ok) {
		free(edited);
		seedBuilder_close(builder);
		return false;
	}
	builder->edited = edited;
	if (observers[0] != observers[1]) {
		log_message(
			"the match's observers differ from the game's build (%zu records in the input, %zu in the "
			"build); each build keeps its own",
			observers[1],
			observers[0]
		);
	}
	size_t moved = builder->players[0].moved + builder->players[1].moved;
	if (moved) {
		log_message(
			"%zu of the input's players are at other places in the game's build (the game can order a "
			"team differently, as in its first build of a club's match); edits follow each player",
			moved
		);
	}
	platform_initLock(&builder->lock);
	log_message(
		"seed builds: %s%s; %zu fields differ from the game's build",
		builder->next.description,
		builder->teams[0] || builder->teams[1] ? " (with team overrides)" : "",
		seedBuilder_editCount(builder)
	);
	return true;
}

bool seedBuilder_dropTeamEdits(SeedBuilder *builder, unsigned teamSide, size_t *dropped) {
	*dropped = 0;
	if (builder->editCount == 0) {
		return true;
	}
	Side side = {.serialised = &builder->input};
	if (!parseChains(&builder->input, &side.chains)) {
		log_message("cannot identify the %s team's edits", teamSide ? "away" : "home");
		return false;
	}
	for (size_t i = 0; i < builder->input.recordCount; ++i) {
		if (builder->edited[i] &&
				chainKey(&side, i, 0) == (teamSide ? SITE_PART_AWAY : SITE_PART_HOME)) {
			builder->edited[i] = false;
			++*dropped;
		}
	}
	builder->editCount -= *dropped;
	freeChains(&side.chains);
	return true;
}

size_t seedBuilder_editCount(const SeedBuilder *builder) {
	return builder->editCount;
}

bool seedBuilder_build(SeedBuilder *builder, uint32_t seed, ByteBuffer *out) {
	Serialised built = {0};
	bool applyEdits = builder->editCount != 0;
	platform_lock(&builder->lock);
	bool ok = buildSerialised(builder, seed, &built, applyEdits);
	platform_unlock(&builder->lock);
	if (ok && applyEdits) {
		ok = mergeEdits(builder, &built, seed, out);
	} else if (ok) {
		buffer_free(out);
		*out = built.bytes;
		built.bytes = (ByteBuffer){0};
	}
	trace_free(&built);
	return ok;
}

void seedBuilder_close(SeedBuilder *builder) {
	if (builder->edited) {
		platform_destroyLock(&builder->lock);
	}
	trace_free(&builder->input);
	free(builder->edited);
	free(builder->players[0].list);
	free(builder->players[1].list);
	*builder = (SeedBuilder){0};
}

// Notes `slot` for swapping if it holds `expected`.
static bool addSwap(
	Swap *swaps,
	size_t *count,
	uint8_t *slot,
	const uint8_t *expected,
	const uint8_t *replacement
) {
	uint8_t *value = NULL;
	if (expected == replacement || !memory_readPointer(slot, 0, &value) || value != expected) {
		return true;
	}
	for (size_t i = 0; i < *count; ++i) {
		if (swaps[i].slot == slot) {
			return true;
		}
	}
	if (*count == MAX_SWAPS) {
		log_message("too many fixture pointers to replace safely");
		return false;
	}
	swaps[(*count)++] = (Swap){slot, value, replacement};
	return true;
}

// Notes both of `fixture`'s team slots for swapping from `original` to `selected`.
static bool swapFixture(
	Swap *swaps,
	size_t *count,
	uint8_t *fixture,
	uint8_t *const original[2],
	const uint8_t *const selected[2]
) {
	for (unsigned side = 0; side < 2; ++side) {
		if (!addSwap(
					swaps,
					count,
					fixture + (side ? FIXTURE_AWAY : FIXTURE_HOME),
					original[side],
					selected[side]
				)) {
			return false;
		}
	}
	return true;
}

// Notes the copies of `fixture` in the human's attended matches for swapping.
static bool swapAttendedCopies(
	Swap *swaps,
	size_t *count,
	const uint8_t *fixture,
	uint8_t *const original[2],
	const uint8_t *const selected[2]
) {
	uint8_t *holder = NULL;
	uint8_t *start = NULL;
	uint8_t *end = NULL;
	if (!memory_readPointer(game_address(RVA_HUMAN_MATCHES_HOLDER), 0, &holder) ||
			!memory_readPointer(holder, HUMAN_MATCHES, &start) ||
			!memory_readPointer(holder, HUMAN_MATCHES + sizeof(void *), &end)) {
		return true;
	}
	for (uint8_t *entry = start; entry < end; entry += sizeof(void *)) {
		uint8_t *match = NULL;
		uint8_t *copy = NULL;
		uint8_t *copyHome = NULL;
		uint8_t *copyAway = NULL;
		if (memory_readPointer(entry, 0, &match) && memory_readPointer(match, 0, &copy) &&
				copy != fixture && memory_readPointer(copy, FIXTURE_HOME, &copyHome) &&
				memory_readPointer(copy, FIXTURE_AWAY, &copyAway) && copyHome == original[0] &&
				copyAway == original[1] && !swapFixture(swaps, count, copy, original, selected)) {
			return false;
		}
	}
	return true;
}

// Notes each original team's next-match opponent for swapping to its new opponent.
static bool swapOpponents(
	Swap *swaps,
	size_t *count,
	uint8_t *const original[2],
	const uint8_t *const selected[2]
) {
	for (unsigned side = 0; side < 2; ++side) {
		const uint8_t *opponent = original[side] == selected[0]   ? selected[1]
															: original[side] == selected[1] ? selected[0]
																															: selected[1 - side];
		if (opponent == original[1 - side]) {
			continue;
		}
		uint8_t *data = NULL;
		if (memory_readPointer(original[side], TEAM_IN_GAME_DATA, &data) &&
				!addSwap(swaps, count, data + NEXT_MATCH_OPPONENT, original[1 - side], opponent)) {
			return false;
		}
	}
	return true;
}

// Updates the fixture, its copies in the human's attended matches and each original team's
// next-match opponent. Collect everything before writing so an overflow leaves the game untouched.
static bool swapTeams(const SeedBuilder *builder, Swap *swaps, size_t *count) {
	*count = 0;
	if (!builder->teams[0] && !builder->teams[1]) {
		return true;
	}
	uint8_t *fixture = builder->next.fixture;
	uint8_t *original[2] = {0};
	if (!memory_readPointer(fixture, FIXTURE_HOME, &original[0]) ||
			!memory_readPointer(fixture, FIXTURE_AWAY, &original[1]) || !original[0] || !original[1]) {
		log_message("cannot read the fixture's teams for replacement");
		return false;
	}
	const uint8_t *selected[2] = {builder->teams[0] ? builder->teams[0] : original[0],
		builder->teams[1] ? builder->teams[1] : original[1]};
	if (!swapFixture(swaps, count, fixture, original, selected) ||
			!swapAttendedCopies(swaps, count, fixture, original, selected) ||
			!swapOpponents(swaps, count, original, selected)) {
		return false;
	}
	for (size_t i = 0; i < *count; ++i) {
		memcpy(swaps[i].slot, &swaps[i].replacement, sizeof swaps[i].replacement);
	}
	return true;
}

static void restoreSwaps(const Swap *swaps, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		memcpy(swaps[i].slot, &swaps[i].original, sizeof swaps[i].original);
	}
}

// The game cannot build a team against itself: before the human submits the team for the match,
// the away side gets unnamed filler players (the home side has taken the squad); after, both sides
// do. So a team playing itself is built against the fixture's own team on one side, and its block
// copied over that team's. Sets `build`'s fixture and overrides and returns the side to copy from
// (the team's own side in the fixture, else home), or 2 if the team does not play itself.
static unsigned planBuild(const SeedBuilder *builder, SeedBuilder *build) {
	*build = (SeedBuilder){.next = builder->next, .teams = {builder->teams[0], builder->teams[1]}};
	uint8_t *original[2] = {0};
	if ((!builder->teams[0] && !builder->teams[1]) ||
			!memory_readPointer(builder->next.fixture, FIXTURE_HOME, &original[0]) ||
			!memory_readPointer(builder->next.fixture, FIXTURE_AWAY, &original[1])) {
		return 2;
	}
	const uint8_t *home = builder->teams[0] ? builder->teams[0] : original[0];
	const uint8_t *away = builder->teams[1] ? builder->teams[1] : original[1];
	if (home != away) {
		return 2;
	}
	unsigned from = original[1] == away ? 1 : 0;
	build->teams[from] = home;
	build->teams[1 - from] = NULL;
	return from;
}

// The records [range[0], range[1]) written for `part` (a SITE_PART_*), which must be contiguous.
static bool findPart(const Side *side, uint32_t part, size_t range[2]) {
	size_t count = side->serialised->recordCount;
	size_t start = 0;
	while (start < count && chainKey(side, start, 0) != part) {
		++start;
	}
	size_t end = runEnd(side, start, count, 0, part);
	for (size_t i = end; i < count; ++i) {
		if (chainKey(side, i, 0) == part) {
			return false;
		}
	}
	range[0] = start;
	range[1] = end;
	return start < end;
}

// The counterpart in block `part` of another team block's call chain, added to the path table if
// new.
static uint32_t movedPath(Serialised *serialised, uint32_t path, uint32_t part) {
	const char *rest = strchr(serialised->paths[path], '>');
	char chain[512];
	int written = snprintf(chain, sizeof chain, "%x%s", (unsigned)part, rest ? rest : "");
	if (written < 0 || (size_t)written >= sizeof chain) {
		return UINT32_MAX;
	}
	return trace_findPath(serialised, chain);
}

// Appends `from`'s records [start, end) to `to`'s bytes and records. With `paths` (per path: its
// counterpart in block `toPart`, or UINT32_MAX until looked up), they move from block `fromPart`.
static bool copyRecords(
	Serialised *to,
	Serialised *from,
	size_t start,
	size_t end,
	uint32_t *paths,
	const uint32_t parts[2]
) {
	for (size_t i = start; i < end; ++i) {
		TraceRecord record = from->records[i];
		const uint8_t *bytes = from->bytes.data + record.offset;
		if (paths) {
			if (paths[record.path] == UINT32_MAX) {
				paths[record.path] = movedPath(from, record.path, parts[1]);
			}
			if (paths[record.path] == UINT32_MAX) {
				return false;
			}
			record.path = paths[record.path];
			record.caller = record.caller == parts[0] ? parts[1] : record.caller;
		}
		record.offset = (uint32_t)to->bytes.length;
		if (!buffer_append(&to->bytes, bytes, record.length) || !trace_appendRecord(to, record)) {
			return false;
		}
	}
	return true;
}

// Writes side `from`'s team block (0: home, 1: away) in the other side's place: the same players,
// line-up and tactic on both sides, the same for every seed.
static bool mirrorSide(Serialised *built, unsigned from) {
	const uint32_t parts[2] = {
		from ? SITE_PART_AWAY : SITE_PART_HOME, from ? SITE_PART_HOME : SITE_PART_AWAY};
	Side side = {.serialised = built};
	size_t source[2] = {0};
	size_t target[2] = {0};
	bool ok = parseChains(built, &side.chains) && findPart(&side, parts[0], source) &&
						findPart(&side, parts[1], target);
	freeChains(&side.chains);
	if (!ok) {
		log_message("cannot find both teams' blocks to copy one over the other");
		return false;
	}
	uint32_t *paths = malloc(built->pathCount * sizeof *paths);
	ok = paths != NULL;
	for (size_t p = 0; ok && p < built->pathCount; ++p) {
		paths[p] = UINT32_MAX;
	}
	Serialised mirrored = {0};
	ok = ok && copyRecords(&mirrored, built, 0, target[0], NULL, parts) &&
			 copyRecords(&mirrored, built, source[0], source[1], paths, parts) &&
			 copyRecords(&mirrored, built, target[1], built->recordCount, NULL, parts);
	free(paths);
	if (!ok) {
		log_message("cannot copy one team's block over the other's");
		trace_free(&mirrored);
		return false;
	}
	log_message(
		"same team on both sides: %zu %s fields replace %zu %s fields",
		source[1] - source[0],
		from ? "away" : "home",
		target[1] - target[0],
		from ? "home" : "away"
	);
	buffer_free(&built->bytes);
	free(built->records);
	built->bytes = mirrored.bytes;
	built->records = mirrored.records;
	built->recordCount = mirrored.recordCount;
	built->recordCapacity = mirrored.recordCapacity;
	return true;
}

// The game's build of the fixture with `seed` (with team overrides if set), serialised with
// its write records. A team playing itself is traced either way, to copy its block (planBuild()).
static bool
buildSerialised(const SeedBuilder *builder, uint32_t seed, Serialised *out, bool traced) {
	SeedBuilder build;
	unsigned from = planBuild(builder, &build);
	bool selfPlay = from < 2;
	Swap swaps[MAX_SWAPS];
	size_t swapped = 0;
	if (!swapTeams(&build, swaps, &swapped)) {
		return false;
	}
	uint8_t *match = match_build(build.next.fixture, seed);
	bool ok = match && trace_serialize(match, out, traced || selfPlay) &&
						(!selfPlay || mirrorSide(out, from));
	if (match) {
		match_freeBuilt(match);
	}
	restoreSwaps(swaps, swapped);
	return ok;
}

// Loads serialised match bytes and serialises the match again, with its write records.
static bool roundTrip(const ByteBuffer *bytes, Serialised *out) {
	ByteBuffer copy = {0};
	if (!buffer_append(&copy, bytes->data, bytes->length)) {
		return false;
	}
	uint8_t *match = match_deserialize(&copy);
	bool ok = match && trace_serialize(match, out, true);
	if (match) {
		match_delete(match);
	}
	buffer_free(&copy);
	return ok;
}

// Marks the input's records that differ from the reference build's (`edited` starts cleared) and
// lists each team's players in `players` (comparePlayers()). False, with the reason in `why` where
// there is one, unless both make the same writes in the same order, apart from the match's
// observers, which are never edits: each build lists its own (isObserver()), and the order of each
// team's players.
static bool findEdits(
	const Side *reference,
	const Side *input,
	bool *edited,
	SeedPlayers players[2],
	char *why,
	size_t whySize
) {
	size_t referenceCount = reference->serialised->recordCount;
	size_t inputCount = input->serialised->recordCount;
	bool compared[2] = {false, false};
	size_t i = skipObservers(reference, 0, referenceCount);
	size_t j = skipObservers(input, 0, inputCount);
	while (i < referenceCount && j < inputCount) {
		uint32_t part = playerPart(reference, i);
		unsigned team = part == SITE_PART_AWAY;
		if (part && !compared[team] && playerPart(input, j) == part) {
			compared[team] = true;
			size_t ends[2] = {i, j};
			if (!comparePlayers(reference, input, ends, edited, &players[team], why, whySize)) {
				return false;
			}
			if (players[team].count) {
				i = skipObservers(reference, ends[0], referenceCount);
				j = skipObservers(input, ends[1], inputCount);
				continue;
			}
		}
		if (reference->serialised->records[i].site != input->serialised->records[j].site) {
			return false;
		}
		edited[j] = !sameRecord(reference->serialised, i, input->serialised, j);
		i = skipObservers(reference, i + 1, referenceCount);
		j = skipObservers(input, j + 1, inputCount);
	}
	return i == referenceCount && j == inputCount;
}

// Compares a team's players, which start at reference record ends[0] and input record ends[1],
// in the pairs pairPlayers() makes, lists the input's in `players` and sets `ends` to the records
// after them. The game does not always list a team's players in the same order: the AI's line-up
// for another seed or opponent, and a club's first build after the game starts, can order players
// it rates alike differently (docs/TECHNICAL.md#known-limits). If the reference's players cannot
// be split, `players` stays empty and they are compared in order.
static bool comparePlayers(
	const Side *reference,
	const Side *input,
	size_t ends[2],
	bool *edited,
	SeedPlayers *players,
	char *why,
	size_t whySize
) {
	const char *team = playerPart(reference, ends[0]) == SITE_PART_AWAY ? "away" : "home";
	SeedPlayer *game = NULL;
	size_t count = 0;
	if (!splitPlayers(reference, ends[0], &game, &count)) {
		log_message("cannot split the %s players of the game's build; pairing them in order", team);
		return true;
	}
	SeedPlayer *list = NULL;
	size_t listed = 0;
	size_t *pairs = NULL;
	bool ok = splitPlayers(input, ends[1], &list, &listed);
	if (ok && listed != count) {
		(void)snprintf(
			why, whySize, "the %s team has %zu players, the game's build %zu", team, listed, count
		);
		ok = false;
	}
	if (ok) {
		pairs = malloc(count * sizeof *pairs);
		if (!pairs) {
			(void)snprintf(why, whySize, "out of memory pairing the %s players", team);
		}
		ok = pairs &&
				 pairPlayers(
					 reference->serialised, game, input->serialised, list, count, pairs, team, why, whySize
				 );
	}
	for (size_t x = 0; ok && x < count; ++x) {
		const SeedPlayer *own = &game[pairs[x]];
		for (size_t k = 0; k < own->end - own->start; ++k) {
			edited[list[x].start + k] =
				!sameRecord(reference->serialised, own->start + k, input->serialised, list[x].start + k);
		}
		list[x].person = own->person;
		list[x].occurrence = own->occurrence;
		players->moved += pairs[x] != x;
	}
	if (ok) {
		ends[0] = game[count - 1].end;
		ends[1] = list[count - 1].end;
		players->list = list;
		players->count = count;
		list = NULL;
	}
	free(pairs);
	free(list);
	free(game);
	return ok;
}

// Pairs each of the input's players x with a player pairs[x] of the game's build that makes the
// same writes. Of those, an unedited player shares every record with its own, and an edited or
// injected one most of them (an injected player keeps the replaced one's familiarity, role ratings
// and squad number), so pairs sharing more records go first, then those with the same unique ID,
// then those at the same or a nearer place. False, with the reason, unless every player pairs.
static bool pairPlayers(
	const Serialised *reference,
	const SeedPlayer *game,
	const Serialised *input,
	const SeedPlayer *list,
	size_t count,
	size_t *pairs,
	const char *team,
	char *why,
	size_t whySize
) {
	size_t *gameGroups = malloc(count * sizeof *gameGroups);
	size_t *inputGroups = malloc(count * sizeof *inputGroups);
	Candidate *candidates = malloc(count * count * sizeof *candidates);
	bool *taken = calloc(count, sizeof *taken);
	bool ok = gameGroups && inputGroups && candidates && taken;
	if (!ok) {
		(void)snprintf(why, whySize, "out of memory pairing the %s players", team);
	}
	for (size_t k = 0; ok && k < count; ++k) {
		gameGroups[k] = groupOf(reference, game, count, reference, &game[k]);
		inputGroups[k] = groupOf(reference, game, count, input, &list[k]);
	}
	size_t unpaired = ok ? firstUnpaired(gameGroups, inputGroups, count) : count;
	if (unpaired < count) {
		(void)snprintf(
			why,
			whySize,
			"%s.player[%zu] (unique ID %u) matches no player of the game's build",
			team,
			unpaired,
			list[unpaired].person
		);
		ok = false;
	}
	size_t candidateCount = 0;
	for (size_t x = 0; ok && x < count; ++x) {
		pairs[x] = SIZE_MAX;
		for (size_t y = 0; y < count; ++y) {
			if (gameGroups[y] == inputGroups[x]) {
				candidates[candidateCount++] = (Candidate){
					.input = x,
					.game = y,
					.shared = sharedRecords(input, &list[x], reference, &game[y]),
					.sameId = list[x].person == game[y].person,
				};
			}
		}
	}
	if (ok) {
		qsort(candidates, candidateCount, sizeof *candidates, compareCandidates);
	}
	for (size_t c = 0; ok && c < candidateCount; ++c) {
		const Candidate *candidate = &candidates[c];
		if (pairs[candidate->input] == SIZE_MAX && !taken[candidate->game]) {
			pairs[candidate->input] = candidate->game;
			taken[candidate->game] = true;
		}
	}
	free(gameGroups);
	free(inputGroups);
	free(candidates);
	free(taken);
	return ok;
}

// The first of the game's players that makes the same writes as `player`, or `count` if none does.
static size_t groupOf(
	const Serialised *reference,
	const SeedPlayer *game,
	size_t count,
	const Serialised *serialised,
	const SeedPlayer *player
) {
	size_t group = 0;
	while (group < count && !sameWrites(reference, &game[group], serialised, player)) {
		++group;
	}
	return group;
}

// Each group of players that make the same writes (groupOf()) needs as many of the input's as of
// the game's. The first input player that has no group, or is one too many for its group, or
// `count` if every one pairs.
static size_t firstUnpaired(const size_t *gameGroups, const size_t *inputGroups, size_t count) {
	for (size_t x = 0; x < count; ++x) {
		size_t inputs = 0;
		size_t games = 0;
		for (size_t k = 0; k < count; ++k) {
			inputs += k <= x && inputGroups[k] == inputGroups[x];
			games += gameGroups[k] == inputGroups[x];
		}
		if (inputGroups[x] == count || inputs > games) {
			return x;
		}
	}
	return count;
}

// More shared records first, then the same unique ID, then the same place, then nearer places.
static int compareCandidates(const void *a, const void *b) {
	const Candidate *x = a;
	const Candidate *y = b;
	size_t xDistance = x->input > x->game ? x->input - x->game : x->game - x->input;
	size_t yDistance = y->input > y->game ? y->input - y->game : y->game - y->input;
	if (x->shared != y->shared) {
		return x->shared > y->shared ? -1 : 1;
	}
	if (x->sameId != y->sameId) {
		return x->sameId ? -1 : 1;
	}
	if (xDistance != yDistance) {
		return xDistance < yDistance ? -1 : 1;
	}
	if (x->input != y->input) {
		return x->input < y->input ? -1 : 1;
	}
	return x->game < y->game ? -1 : x->game > y->game;
}

// SITE_PART_HOME or SITE_PART_AWAY if `record` is written for one of that team's players, else 0.
static uint32_t playerPart(const Side *side, size_t record) {
	uint32_t part = chainKey(side, record, 0);
	bool team = part == SITE_PART_HOME || part == SITE_PART_AWAY;
	return team && chainKey(side, record, 1) == SITE_TEAM_PLAYER ? part : 0;
}

// A player's first write, by MATCH_PLAYER__serialize itself (sites.h).
static bool startsPlayer(const Side *side, size_t record) {
	return side->serialised->records[record].site == SITE_PLAYER_FIRST &&
				 chainKey(side, record, 2) == SITE_PLAYER_FIRST;
}

// Splits a team's players, whose records start at `start`, into one entry per player, with the
// first 4-byte SITE_PLAYER_UID write as the player's unique ID (as labels.c names it). False unless
// each player starts with its first write and no later record belongs to them.
static bool splitPlayers(const Side *side, size_t start, SeedPlayer **players, size_t *count) {
	const Serialised *serialised = side->serialised;
	*players = NULL;
	*count = 0;
	uint32_t part = start < serialised->recordCount ? playerPart(side, start) : 0;
	size_t end = start;
	size_t total = 0;
	while (end < serialised->recordCount && playerPart(side, end) == part) {
		total += startsPlayer(side, end++);
	}
	bool ok = part && total && startsPlayer(side, start);
	for (size_t i = end; ok && i < serialised->recordCount; ++i) {
		ok = playerPart(side, i) != part;
	}
	SeedPlayer *list = ok ? calloc(total, sizeof *list) : NULL;
	if (!list) {
		return false;
	}
	size_t k = 0;
	for (size_t i = start; i < end; ++i) {
		if (startsPlayer(side, i)) {
			if (k) {
				list[k - 1].end = i;
			}
			list[k++] = (SeedPlayer){.start = i, .uidRecord = SIZE_MAX, .person = UINT32_MAX};
		}
		const TraceRecord *record = &serialised->records[i];
		if (list[k - 1].uidRecord == SIZE_MAX && record->site == SITE_PLAYER_UID &&
				record->length == sizeof(uint32_t)) {
			list[k - 1].uidRecord = i;
			list[k - 1].person = personAt(serialised, i);
		}
	}
	list[k - 1].end = end;
	for (k = 0; k < total; ++k) {
		for (size_t earlier = 0; earlier < k; ++earlier) {
			list[k].occurrence += list[earlier].person == list[k].person;
		}
	}
	*players = list;
	*count = total;
	return true;
}

// The unique ID written by a 4-byte record, or UINT32_MAX.
static uint32_t personAt(const Serialised *serialised, size_t record) {
	if (record >= serialised->recordCount || serialised->records[record].length != sizeof(uint32_t)) {
		return UINT32_MAX;
	}
	return memory_loadU32(serialised->bytes.data + serialised->records[record].offset);
}

// Whether two players make the same writes in the same order.
static bool
sameWrites(const Serialised *a, const SeedPlayer *x, const Serialised *b, const SeedPlayer *y) {
	if (x->end - x->start != y->end - y->start) {
		return false;
	}
	for (size_t k = 0; k < x->end - x->start; ++k) {
		if (a->records[x->start + k].site != b->records[y->start + k].site) {
			return false;
		}
	}
	return true;
}

// How many records two players that make the same writes have in common.
static size_t
sharedRecords(const Serialised *a, const SeedPlayer *x, const Serialised *b, const SeedPlayer *y) {
	size_t shared = 0;
	for (size_t k = 0; k < x->end - x->start; ++k) {
		shared += sameRecord(a, x->start + k, b, y->start + k);
	}
	return shared;
}

// Whether the input's edits include one of `team`'s players, paired by person.
static bool editsPlayers(const SeedBuilder *builder, unsigned team) {
	const SeedPlayers *players = &builder->players[team];
	if (players->count == 0) {
		return false;
	}
	for (size_t i = players->list[0].start; i < players->list[players->count - 1].end; ++i) {
		if (builder->edited[i]) {
			return true;
		}
	}
	return false;
}

// Whether a build with the input's layout lists the players of each team in `teams` in the
// input's order (with the same layout, its players' unique IDs are where the input's are).
static bool samePlayers(const SeedBuilder *builder, const Serialised *built, const bool teams[2]) {
	for (unsigned team = 0; team < 2; ++team) {
		const SeedPlayers *players = &builder->players[team];
		for (size_t k = 0; teams[team] && k < players->count; ++k) {
			uint32_t person = personAt(built, players->list[k].uidRecord);
			size_t occurrence = 0;
			for (size_t earlier = 0; earlier < k; ++earlier) {
				occurrence += personAt(built, players->list[earlier].uidRecord) == person;
			}
			if (person != players->list[k].person || occurrence != players->list[k].occurrence) {
				return false;
			}
		}
	}
	return true;
}

static size_t countObservers(const Side *side) {
	size_t count = 0;
	for (size_t i = 0; i < side->serialised->recordCount; ++i) {
		count += isObserver(side, i);
	}
	return count;
}

// The same writes in the same order (values, and so string lengths, may differ).
static bool sameLayout(const Serialised *a, const Serialised *b) {
	if (a->recordCount != b->recordCount) {
		return false;
	}
	for (size_t i = 0; i < a->recordCount; ++i) {
		if (a->records[i].site != b->records[i].site) {
			return false;
		}
	}
	return true;
}

static bool sameRecord(const Serialised *a, size_t i, const Serialised *b, size_t j) {
	const TraceRecord *x = &a->records[i];
	const TraceRecord *y = &b->records[j];
	return x->length == y->length &&
				 memcmp(a->bytes.data + x->offset, b->bytes.data + y->offset, x->length) == 0;
}

static bool appendRecord(ByteBuffer *out, const Serialised *from, size_t i) {
	const TraceRecord *record = &from->records[i];
	return buffer_append(out, from->bytes.data + record->offset, record->length);
}

static bool parseChains(const Serialised *serialised, Chains *chains) {
	*chains = (Chains){0};
	for (size_t i = 0; i < serialised->recordCount; ++i) {
		if (serialised->records[i].path >= serialised->pathCount) {
			log_message("invalid call chain at record %zu", i);
			return false;
		}
	}
	size_t total = 0;
	for (size_t p = 0; p < serialised->pathCount; ++p) {
		for (const char *c = serialised->paths[p]; *c; ++c) {
			total += *c == '>';
		}
		++total;
	}
	size_t paths = serialised->pathCount ? serialised->pathCount : 1;
	chains->rvas = malloc((total ? total : 1) * sizeof *chains->rvas);
	chains->starts = malloc(paths * sizeof *chains->starts);
	chains->lengths = malloc(paths * sizeof *chains->lengths);
	if (!chains->rvas || !chains->starts || !chains->lengths) {
		freeChains(chains);
		return false;
	}
	size_t used = 0;
	for (size_t p = 0; p < serialised->pathCount; ++p) {
		chains->starts[p] = used;
		for (const char *text = serialised->paths[p]; *text && used < total;) {
			char *end = NULL;
			chains->rvas[used++] = (uint32_t)strtoul(text, &end, 16);
			text = *end == '>' ? end + 1 : end + strlen(end);
		}
		chains->lengths[p] = used - chains->starts[p];
		if (chains->lengths[p] > chains->deepest) {
			chains->deepest = chains->lengths[p];
		}
	}
	return true;
}

static void freeChains(Chains *chains) {
	free(chains->rvas);
	free(chains->starts);
	free(chains->lengths);
	*chains = (Chains){0};
}

// A record's call chain at `depth`: the return address there, then the write's own call site,
// then 0.
static uint32_t chainKey(const Side *side, size_t record, size_t depth) {
	const TraceRecord *entry = &side->serialised->records[record];
	size_t length = side->chains.lengths[entry->path];
	if (depth < length) {
		return side->chains.rvas[side->chains.starts[entry->path] + depth];
	}
	return depth == length ? entry->site : 0;
}

// A write of the match's observers (sites.h): their count, each entry and each team name.
static bool isObserver(const Side *side, size_t record) {
	if (chainKey(side, record, 0) != SITE_PART_MATCH) {
		return false;
	}
	uint32_t key = chainKey(side, record, 1);
	return key == SITE_OBSERVER_COUNT || key == SITE_OBSERVER_ENTRY || key == SITE_OBSERVER_TEAM;
}

static size_t skipObservers(const Side *side, size_t from, size_t to) {
	while (from < to && isObserver(side, from)) {
		++from;
	}
	return from;
}

static bool sameSites(const Alignment *a, size_t i0, size_t i1, size_t j0, size_t j1) {
	if (i1 - i0 != j1 - j0) {
		return false;
	}
	for (size_t k = 0; k < i1 - i0; ++k) {
		if (a->input.serialised->records[i0 + k].site != a->built.serialised->records[j0 + k].site) {
			return false;
		}
	}
	return true;
}

// The end of the run of records from `from` whose chain at `depth` is `key`.
static size_t runEnd(const Side *side, size_t from, size_t to, size_t depth, uint32_t key) {
	while (from < to && chainKey(side, from, depth) == key) {
		++from;
	}
	return from;
}

// Whether both ranges are the same sequence of runs at `depth` (e.g. the same parts of a team,
// whatever each part holds). The match's observers are not compared.
static bool sameRuns(const Alignment *a, size_t i0, size_t i1, size_t j0, size_t j1, size_t depth) {
	size_t i = skipObservers(&a->input, i0, i1);
	size_t j = skipObservers(&a->built, j0, j1);
	while (i < i1 && j < j1) {
		uint32_t key = chainKey(&a->input, i, depth);
		if (chainKey(&a->built, j, depth) != key) {
			return false;
		}
		i = skipObservers(&a->input, runEnd(&a->input, i, i1, depth, key), i1);
		j = skipObservers(&a->built, runEnd(&a->built, j, j1, depth, key), j1);
	}
	return i == i1 && j == j1;
}

// Pairs the records two ranges share at their start and at their end.
static void alignEnds(const Alignment *a, size_t i0, size_t i1, size_t j0, size_t j1) {
	const TraceRecord *input = a->input.serialised->records;
	const TraceRecord *built = a->built.serialised->records;
	size_t shorter = i1 - i0 < j1 - j0 ? i1 - i0 : j1 - j0;
	size_t head = 0;
	while (head < shorter && input[i0 + head].site == built[j0 + head].site) {
		a->source[j0 + head] = i0 + head;
		++head;
	}
	size_t tail = 0;
	while (head + tail < shorter && input[i1 - 1 - tail].site == built[j1 - 1 - tail].site) {
		a->source[j1 - 1 - tail] = i1 - 1 - tail;
		++tail;
	}
}

// Pairs the input's records [i0, i1) with the build's [j0, j1). Where their writes differ, both
// are split into the parts written by each call at `depth` of the serialiser's call chains (the
// match's blocks, then a team's parts, then a tactic's, ...) and, if both have the same parts in
// the same order, each pair of parts is aligned one level deeper. Otherwise only the records the
// two ranges share at their start and end are paired. The match's observers are skipped when
// splitting, so the parts around them still pair; they are never edits, so the build keeps its own.
static void align(const Alignment *a, size_t i0, size_t i1, size_t j0, size_t j1, size_t depth) {
	if (sameSites(a, i0, i1, j0, j1)) {
		for (size_t j = j0; j < j1; ++j) {
			a->source[j] = i0 + (j - j0);
		}
		return;
	}
	size_t deepest = a->input.chains.deepest > a->built.chains.deepest ? a->input.chains.deepest
																																		 : a->built.chains.deepest;
	if (depth > deepest || !sameRuns(a, i0, i1, j0, j1, depth)) {
		alignEnds(a, i0, i1, j0, j1);
		return;
	}
	size_t i = skipObservers(&a->input, i0, i1);
	size_t j = skipObservers(&a->built, j0, j1);
	while (i < i1 && j < j1) {
		uint32_t key = chainKey(&a->input, i, depth);
		size_t iEnd = runEnd(&a->input, i, i1, depth, key);
		size_t jEnd = runEnd(&a->built, j, j1, depth, key);
		align(a, i, iEnd, j, jEnd, depth + 1);
		i = skipObservers(&a->input, iEnd, i1);
		j = skipObservers(&a->built, jEnd, j1);
	}
}

// Pairs the build's players of `team` with the input's by person, in place of align()'s pairing,
// which follows their places: the game can list a team's players in another order
// (comparePlayers()). Fails if the build leaves out an edited player.
static bool
alignPlayers(const Alignment *a, const SeedBuilder *builder, unsigned team, uint32_t seed) {
	const SeedPlayers *players = &builder->players[team];
	const char *name = team ? "away" : "home";
	uint32_t part = team ? SITE_PART_AWAY : SITE_PART_HOME;
	size_t builtCount = a->built.serialised->recordCount;
	size_t start = 0;
	while (start < builtCount && playerPart(&a->built, start) != part) {
		++start;
	}
	SeedPlayer *built = NULL;
	size_t count = 0;
	if (!splitPlayers(&a->built, start, &built, &count)) {
		log_message("seed %u: cannot find the %s players in the game's build", seed, name);
		return false;
	}
	size_t first = players->list[0].start;
	size_t last = players->list[players->count - 1].end;
	for (size_t j = 0; j < builtCount; ++j) {
		size_t i = a->source[j];
		if ((j >= built[0].start && j < built[count - 1].end) || (i >= first && i < last)) {
			a->source[j] = SIZE_MAX;
		}
	}
	bool ok = true;
	for (size_t x = 0; x < players->count; ++x) {
		const SeedPlayer *player = &players->list[x];
		size_t b = 0;
		while (b < count &&
					 (built[b].person != player->person || built[b].occurrence != player->occurrence)) {
			++b;
		}
		if (b < count) {
			align(a, player->start, player->end, built[b].start, built[b].end, 2);
			continue;
		}
		bool edited = false;
		for (size_t i = player->start; i < player->end; ++i) {
			edited = edited || builder->edited[i];
		}
		if (edited) {
			log_message(
				"seed %u: the game's build leaves out %s.player[%zu] (unique ID %u), whose fields are "
				"edited",
				seed,
				name,
				x,
				player->person
			);
			ok = false;
		}
	}
	free(built);
	return ok;
}

static bool
alignEdits(Alignment *alignment, const SeedBuilder *builder, const bool teams[2], uint32_t seed) {
	size_t inputCount = alignment->input.serialised->recordCount;
	size_t builtCount = alignment->built.serialised->recordCount;
	alignment->source = malloc((builtCount ? builtCount : 1) * sizeof *alignment->source);
	bool *placed = calloc(inputCount ? inputCount : 1, sizeof *placed);
	bool ok = alignment->source && placed &&
						parseChains(alignment->input.serialised, &alignment->input.chains) &&
						parseChains(alignment->built.serialised, &alignment->built.chains);
	if (!ok) {
		log_message("seed %u: cannot align the input's edits", seed);
	} else {
		for (size_t j = 0; j < builtCount; ++j) {
			alignment->source[j] = SIZE_MAX;
		}
		align(alignment, 0, inputCount, 0, builtCount, 0);
		for (unsigned team = 0; ok && team < 2; ++team) {
			ok = !teams[team] || alignPlayers(alignment, builder, team, seed);
		}
		for (size_t j = 0; ok && j < builtCount; ++j) {
			if (alignment->source[j] < inputCount) {
				placed[alignment->source[j]] = true;
			}
		}
	}
	for (size_t i = 0; ok && i < inputCount; ++i) {
		if (builder->edited[i] && !placed[i]) {
			log_message(
				"seed %u: the game's build has a different structure around edited field #%zu", seed, i
			);
			ok = false;
		}
	}
	free(placed);
	return ok;
}

// The game's build for `seed` with the input's edits re-applied. The build's structure can differ
// from the input's (the AI manager picks its tactic with the seed, which changes how many player
// instructions it writes; another away team has another squad), so the two are aligned by
// align(), and the players of a team with edited players by person (alignPlayers()). An edit
// that cannot be paired with a record of the build fails the seed.
static bool
mergeEdits(const SeedBuilder *builder, const Serialised *built, uint32_t seed, ByteBuffer *out) {
	const Serialised *input = &builder->input;
	size_t builtCount = built->recordCount;
	const bool teams[2] = {editsPlayers(builder, 0), editsPlayers(builder, 1)};
	Alignment a = {.input = {.serialised = input}, .built = {.serialised = built}};
	bool ok = (sameLayout(input, built) && samePlayers(builder, built, teams)) ||
						alignEdits(&a, builder, teams, seed);
	if (ok) {
		buffer_clear(out);
	}
	for (size_t j = 0; ok && j < builtCount; ++j) {
		size_t i = a.source ? a.source[j] : j;
		bool fromInput = i != SIZE_MAX && builder->edited[i];
		ok = appendRecord(out, fromInput ? input : built, fromInput ? i : j);
	}
	freeChains(&a.input.chains);
	freeChains(&a.built.chains);
	free(a.source);
	return ok;
}
