#pragma once
#include <functional>
#include <string>
#include <vector>

namespace tb {

// Quiver lab channel (design/LAB.md in the Quiver repo). Inert outside the lab (no QUIVER_LAB).
//  - lab_state("in-world", {"k=v"}): %QUIVER_LAB%\state, replaced atomically.
//  - lab_verb("where", fn): answers `quiver lab call <i> where ...`; fn returns a JSON value
//    for "result" (or throws/returns "" for null). Requests are polled on a lab thread.
void lab_init();
bool lab_active();
void lab_state(const std::string& state, const std::vector<std::string>& kv = {});

using LabVerb = std::function<std::string(const std::vector<std::string>& args, bool& ok)>;
void lab_verb(const std::string& verb, LabVerb fn);

// JSON string literal for a value (quotes and escapes).
std::string json_str(const std::string& s);

}  // namespace tb
