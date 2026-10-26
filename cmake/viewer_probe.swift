// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// Linked once per slice when the build is configured (cmake/macos_viewer.cmake): the toolchain
// builds the viewer only if this, which uses the same kinds of SwiftUI feature, links.
import AppKit
import SwiftUI

final class ProbeModel: ObservableObject {
	@Published var text = ""
}

struct ProbeView: View {
	@StateObject private var model = ProbeModel()
	@FocusState private var focused: Bool

	var body: some View {
		Grid {
			GridRow {
				TextField("Probe", text: $model.text)
					.focused($focused)
					.onSubmit { focused = false }
				Canvas { context, size in
					context.fill(Path(CGRect(origin: .zero, size: size)), with: .color(.green))
				}
			}
		}
		.textSelection(.enabled)
		.task { await Task.yield() }
	}
}

print(NSHostingView(rootView: ProbeView()).fittingSize)
