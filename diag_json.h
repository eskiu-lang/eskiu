#pragma once

// `--diagnostics=json`: the compiler's diagnostics as JSON lines on stderr (diag_json.cpp).
// Every diagnostic is written to std::cerr in its text form at the place it is raised;
// in JSON mode std::cerr goes through a converter that turns each `error:` / `warning:` /
// `note:` line into one JSON object, so the JSON form carries the same diagnostics as
// the text form. The format and the id scheme are in docs/dev/diagnostics.md; the
// self-hosted driver has the same converter (selfhost/diag_json.esk), byte for byte.

#include <string>

// True once installJsonDiagnostics ran: the text-only summary lines ("Parse failed!",
// "error: parse failed") are not printed then.
extern bool g_jsonDiagnostics;

// Route std::cerr through the converter (until the process exits).
void installJsonDiagnostics();
// Write the diagnostic still held back (it may get notes from the next lines).
void flushJsonDiagnostics();

// The stable id of a diagnostic message (docs/dev/diagnostics.md, "Ids").
std::string diagnosticId(const std::string& message, const std::string& severity);
