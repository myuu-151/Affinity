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
    // bpVsLink text) into the open blueprint; returns the number of nodes
    // inserted (0 = none found). When set, the panel shows an "Insert" button
    // whenever the last reply contains node text.
    void SetInsertHandler(std::function<int(const std::string&)> fn);

    // Render the assistant panel (call once per frame from the editor UI).
    void RenderPanel(bool* p_open);

    // Free model/context/backend. Call once at editor shutdown.
    void Shutdown();

    // True while loading a model or generating (UI can disable inputs).
    bool IsBusy();
}
