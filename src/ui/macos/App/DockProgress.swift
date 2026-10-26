// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import AppKit
import Combine

/// Shows a simulation's progress on the Dock icon, as the Windows viewer shows it on its taskbar
/// button: a badge with the percentage played ("…" until it is known), gone when the run ends.
@MainActor
final class DockProgress {
	private var watcher: AnyCancellable?

	init(_ model: ViewerModel) {
		watcher = model.$simulation
			.map(\.badge)
			.removeDuplicates()
			.sink { badge in
				NSApp.dockTile.badgeLabel = badge
			}
	}
}
