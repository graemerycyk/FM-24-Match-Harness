// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// The Log window's content: everything the viewer has logged, newest last.
struct LogView: View {
	@ObservedObject var log: ViewerLog

	var body: some View {
		VStack(alignment: .leading, spacing: 8) {
			ScrollViewReader { scroller in
				ScrollView {
					Text(log.lines.isEmpty ? "Nothing has been logged yet." : log.text)
						.font(.system(.body, design: .monospaced))
						.textSelection(.enabled)
						.frame(maxWidth: .infinity, alignment: .leading)
						.padding(8)
					Color.clear
						.frame(height: 1)
						.id("end")
				}
				.onChange(of: log.lines.count) { _ in
					scroller.scrollTo("end")
				}
			}
			HStack {
				Spacer()
				Button("Clear") { log.clear() }
			}
		}
		.padding(12)
		.frame(minWidth: 480, minHeight: 240)
		.background(Color(nsColor: .windowBackgroundColor))
	}
}
