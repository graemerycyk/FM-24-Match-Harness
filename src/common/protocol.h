// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// What the harness DLL (inside fm.exe) and the runner (fm24ctl) agree on.
//
// The runner connects to the harness's job channel, writes a job of `key=value` lines and reads the
// DLL's one-line outcome as the reply. The DLL serves one runner at a time and the others wait for
// it. Jobs carry full paths, so a runner works with a DLL loaded from any folder; the DLL writes
// harness.log next to itself.
//
// Windows: the channel is the named pipe HARNESS_PIPE_NAME, one message each way. The pipe has a
// single instance, so waiting runners queue for it.
// macOS: the dylib is loaded when the game starts (DYLD_INSERT_LIBRARIES), and the channel is the
// Unix socket HARNESS_SOCKET_NAME in the user's temporary directory. Once the dylib takes a
// connection it sends HARNESS_SOCKET_ACCEPTED; the runner writes the job and shuts down its sending
// side, and the dylib replies with the result line and closes the connection. Runners connecting
// meanwhile wait in the socket's backlog.
//
// Bump HARNESS_PROTOCOL_TEXT (a dotted version: 1.0.0 -> 1.0.1) to inject a rebuilt DLL while an
// older copy is still loaded in fm.exe.
// Windows identifies a loaded DLL by its path, so a new build needs a new file name; the old copy
// keeps serving its own, differently named pipe and never sees the new jobs. A second copy of the
// same version (from another folder) cannot create the pipe and stays idle. On macOS a new version
// takes effect when the game is next started; runners of another version find no socket of theirs.
#pragma once

#define HARNESS_PROTOCOL_TEXT "1.3.0"
#define HARNESS_WIDE_TEXT_(text) L##text
#define HARNESS_WIDE_TEXT(text) HARNESS_WIDE_TEXT_(text)
#define HARNESS_PROTOCOL HARNESS_WIDE_TEXT(HARNESS_PROTOCOL_TEXT)

#ifdef _WIN32
#define HARNESS_DLL_NAME (L"fm24-harness-" HARNESS_PROTOCOL L".dll")
#define HARNESS_PIPE_NAME (L"\\\\.\\pipe\\FM24Harness_" HARNESS_PROTOCOL)
#else
#define HARNESS_DLL_NAME (L"fm24-harness-" HARNESS_PROTOCOL L".dylib")
#define HARNESS_SOCKET_NAME ("fm24-harness-" HARNESS_PROTOCOL_TEXT ".sock")
#define HARNESS_SOCKET_ACCEPTED '+'
#endif

#define HARNESS_LOG_FILE L"harness.log"

// Longest job message and result line (in bytes) either side accepts.
enum { HARNESS_JOB_LENGTH = 16384, HARNESS_RESULT_LENGTH = 512 };

// Longest path (in wchar_t units: UTF-16 on Windows, UTF-32 on macOS) either side handles; job
// files carry full paths.
enum { HARNESS_PATH_LENGTH = 1024 };

// Most worker threads a run job may use.
enum { HARNESS_MAX_THREADS = 16 };

// Optional run-job snapshots to the x64 viewer (low/high 32 bits):
// wParam = completed/home wins, lParam = total/draws. Away wins = completed - home wins - draws.
enum { HARNESS_PROGRESS_MESSAGE = 0x8002 }; // WM_APP + 2
