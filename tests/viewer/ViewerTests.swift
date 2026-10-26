// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// fm24viewer_tests: the macOS viewer's models, without a window (ctest's viewer and
/// viewer_x86_64).
@main
enum ViewerTests {
	@MainActor
	static func main() {
		FixtureHandleTests.register()
		ViewerModelTests.register()
		ViewerEditTests.register()
		HarnessTests.register()
		HarnessModelTests.register()
		SimulationTests.register()
		ChoiceTests.register()
		ReplacementTests.register()
		QuestionTests.register()
		SaveCheckTests.register()
		LaunchTests.register()
		exit(Tests.finish())
	}
}
