#pragma once
#include <string>
#include <functional>

// Local LLM assistant (llama.cpp, embedded, CPU). Fully offline. Lets you ask
// the engine how to set things up and generate node graphs to insert. All heavy
// work (model load + token generation) runs on a worker thread so the editor UI
// never blocks.
namespace llm {
    // Provide the engine's node catalog / capabilities as the system prompt
    // grounding. Call whenever the catalog changes (cheap; just stores a string).
    void SetSystemPrompt(const std::string& sys);

    // Handler that inserts a generated node graph (the assistant's bpVsNode/
    // bpVsLink text) into the open blueprint; returns a status + lint message to
    // display (inserted count + any warnings). When set, the panel shows an
    // "Insert" button whenever the last reply contains node text.
    void SetInsertHandler(std::function<std::string(const std::string&)> fn);

    // Provide a GBNF grammar (built from the live node catalog + rig clips) that
    // constrains generation to valid node-graph syntax. Called on the UI thread
    // right before each generation (so it reflects the current project) when the
    // "Constrain output (grammar)" setting is on.
    void SetGrammarProvider(std::function<std::string()> fn);

    // Handler that lints generated node-graph text WITHOUT inserting it, returning
    // an empty string if clean or a bullet list of problems. Used by the auto-repair
    // loop to feed errors back to the model. Must be thread-safe (read-only).
    void SetLintHandler(std::function<std::string(const std::string&)> fn);

    // Provider for a per-turn context snapshot (e.g. the currently-selected nodes
    // and their live params) prepended to the user's message so the model can edit
    // them in place. Called on the UI thread before each generation; returns "" when
    // there's nothing to add. Lets you select nodes and say "reduce the speed", etc.
    void SetContextProvider(std::function<std::string()> fn);

    // Provider for the WHOLE current graph, used by the "Edit" button: lets the model
    // EXTEND the existing graph (add nodes/links, change params) instead of rebuilding.
    // Returns "" when the graph is empty.
    void SetEditContextProvider(std::function<std::string()> fn);

    // Render the assistant panel (call once per frame from the editor UI).
    void RenderPanel(bool* p_open);

    // Free model/context/backend. Call once at editor shutdown.
    void Shutdown();

    // True while loading a model or generating (UI can disable inputs).
    bool IsBusy();
}
