// Script Studio runtime: sandboxed QuickJS with an explicit, permission-gated project API.
// Scripts have no filesystem, network, camera or credential bindings. All edits made by a script are
// applied to a working copy and committed by the host as ONE undoable transaction.
#pragma once
#include "common.hpp"

namespace mf {

struct ScriptRequest {
    std::string name = "script";
    std::string source;
    std::vector<std::string> permissions;  // PROJECT_READ, PROJECT_WRITE, TIMELINE_WRITE, LOCAL_STORAGE, RENDER ...
    json project;                          // current document
    std::string compId;
    std::vector<std::string> selection;    // selected layer ids
    double playhead = 0;
    json storage = json::object();         // persisted per-script key/value store
    double timeoutSec = 10;
    size_t memoryLimit = 64u << 20;
    json args = json::object();            // values from the script's UI panel
};

struct ScriptResult {
    bool ok = false;
    std::string error;        // includes "Line N:" and a suggestion when possible
    int line = 0;
    std::vector<std::string> logs, alerts;
    json project;             // working copy after the script
    bool changed = false;
    int opsApplied = 0;
    json actions = json::array();  // host actions requested (e.g. export jobs)
    json storage = json::object();
    json returnValue;
    double ms = 0;
};

ScriptResult runScript(const ScriptRequest& req);
bool validateScriptSyntax(const std::string& source, std::string& error, int& line);
// Machine-readable API description (for autocomplete / documentation generation).
json scriptApiDescription();
std::vector<std::string> allPermissions();
// Bundled example scripts.
json exampleScripts();

}  // namespace mf
