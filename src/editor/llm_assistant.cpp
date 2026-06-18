// Local LLM assistant — llama.cpp embedded, CPU, fully offline.
// Heavy work (model load + generation) runs on a single worker thread; the UI
// thread only reads shared state under g_mtx, so the editor never blocks.
//
// Prompt caching: the (large) node-catalog system prompt is decoded into the KV
// cache ONCE at model load. Each chat turn then decodes only the NEW message
// text (the chat template is prefix-stable, so we append the formatted delta and
// let llama_decode advance positions) — the catalog is never reprocessed. This
// is what keeps it usable on a laptop CPU.
#include "llm_assistant.h"
#include "llama.h"
#include "imgui.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>

namespace llm {
namespace {

struct Msg { std::string role, content; };

std::mutex              g_mtx;            // guards items marked [g]
std::vector<Msg>        g_history;        // [g] display log (user/assistant)
std::string             g_partial;        // [g] streaming assistant text
std::string             g_status = "No model loaded.";  // [g]
std::string             g_system;         // [g] system prompt (engine grounding)

llama_model*            g_model = nullptr;   // [g]
llama_context*          g_ctx   = nullptr;   // [g]
const llama_vocab*      g_vocab = nullptr;   // [g]

// Worker-thread-only (load + generate run sequentially on g_worker): the running
// conversation used for templating + the formatted length already in the KV.
std::vector<Msg>        g_convo;
int                     g_prevLen = 0;

std::atomic<bool>       g_loading{false};
std::atomic<bool>       g_generating{false};
std::atomic<bool>       g_stop{false};
std::thread             g_worker;
std::atomic<bool>       g_backendInit{false};

char g_modelPath[512] = "";
char g_input[4096]    = "";

// Compute setting (applied at Load). g_useGpu chooses CPU vs GPU; g_pct is the
// level: CPU% = fraction of logical cores; GPU% = fraction of model layers
// offloaded to the GPU. GPU only does anything with a GPU-enabled llama build.
int  g_pct    = 50;     // 50 or 75
bool g_useGpu = false;

std::function<std::string(const std::string&)> g_insertHandler;   // [g] set once at startup
std::string g_insertStatus;                                       // [g] feedback after an insert

void setStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_mtx); g_status = s; }

void ensureBackend() {
    if (!g_backendInit.exchange(true)) {
        llama_log_set([](enum ggml_log_level, const char*, void*){}, nullptr);  // silence llama spam
        llama_backend_init();
    }
}

void joinWorker() { if (g_worker.joinable()) g_worker.join(); }

std::string applyTemplate(const std::vector<Msg>& msgs, bool addAss) {
    std::vector<llama_chat_message> cm;
    cm.reserve(msgs.size());
    for (auto& m : msgs) cm.push_back({ m.role.c_str(), m.content.c_str() });
    const char* tmpl = llama_model_chat_template(g_model, nullptr);
    if (tmpl) {
        std::vector<char> buf(8192);
        int len = llama_chat_apply_template(tmpl, cm.data(), cm.size(), addAss, buf.data(), (int)buf.size());
        if (len > (int)buf.size()) { buf.resize(len); len = llama_chat_apply_template(tmpl, cm.data(), cm.size(), addAss, buf.data(), (int)buf.size()); }
        if (len >= 0) return std::string(buf.data(), len);
    }
    std::string p;  // ChatML fallback
    for (auto& m : msgs) p += "<|im_start|>" + m.role + "\n" + m.content + "<|im_end|>\n";
    if (addAss) p += "<|im_start|>assistant\n";
    return p;
}

std::vector<llama_token> tokenize(const std::string& s, bool addSpecial) {
    int n = -llama_tokenize(g_vocab, s.c_str(), (int)s.size(), nullptr, 0, addSpecial, true);
    std::vector<llama_token> t(n > 0 ? n : 0);
    if (n > 0) llama_tokenize(g_vocab, s.c_str(), (int)s.size(), t.data(), n, addSpecial, true);
    return t;
}

// Decode tokens into the KV cache in n_batch chunks (positions auto-advance).
bool decodeTokens(std::vector<llama_token>& toks) {
    int nb = (int)llama_n_batch(g_ctx); if (nb < 1) nb = 512;
    for (int i = 0; i < (int)toks.size(); i += nb) {
        int cnt = (int)toks.size() - i; if (cnt > nb) cnt = nb;
        llama_batch b = llama_batch_get_one(toks.data() + i, cnt);
        if (llama_decode(g_ctx, b) != 0) return false;
    }
    return true;
}

void loadWorker(std::string path) {
    ensureBackend();
    unsigned hc = std::thread::hardware_concurrency(); if (!hc) hc = 4;

    // GPU offload: read the model's layer count cheaply (vocab-only) so we can
    // offload g_pct% of the layers (the rest stay on CPU). Inert on a CPU-only
    // llama build (no GPU device -> n_gpu_layers is ignored and it runs on CPU).
    int ngl = 0;
    if (g_useGpu) {
        llama_model_params vp = llama_model_default_params();
        vp.vocab_only = true; vp.n_gpu_layers = 0;
        llama_model* vm = llama_model_load_from_file(path.c_str(), vp);
        if (vm) { int nl = llama_model_n_layer(vm); ngl = (g_pct >= 100) ? -1 : (nl * g_pct) / 100; if (ngl < 1) ngl = 1; llama_model_free(vm); }
        else ngl = -1;   // couldn't read the count; offload all
    }

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = ngl;
    llama_model* m = llama_model_load_from_file(path.c_str(), mp);
    if (!m) { setStatus("Failed to load model: " + path); g_loading = false; return; }
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx   = 8192;     // room for the node-catalog system prompt + a chat
    cp.n_batch = 512;
    // CPU mode: use g_pct% of the cores. GPU mode: GPU does most of the work, so
    // keep CPU light (~half) for the non-offloaded layers + sampling. Always >=1.
    int nt = g_useGpu ? (int)(hc / 2) : (int)(hc * g_pct / 100);
    if (nt < 1) nt = 1;
    cp.n_threads = nt; cp.n_threads_batch = nt;
    llama_context* c = llama_init_from_model(m, cp);
    if (!c) { llama_model_free(m); setStatus("Failed to create context."); g_loading = false; return; }

    { std::lock_guard<std::mutex> lk(g_mtx); g_model = m; g_ctx = c; g_vocab = llama_model_get_vocab(m); g_history.clear(); g_partial.clear(); }

    // Pre-decode the system prompt (node catalog) into the KV cache once.
    g_convo.clear(); g_prevLen = 0;
    std::string sys; { std::lock_guard<std::mutex> lk(g_mtx); sys = g_system; }
    std::string name = std::filesystem::path(path).filename().string();
    if (!sys.empty()) {
        setStatus("Loaded " + name + " — processing engine knowledge...");
        g_convo.push_back({ "system", sys });
        std::string formatted = applyTemplate(g_convo, false);
        std::vector<llama_token> t = tokenize(formatted, true);
        decodeTokens(t);
        g_prevLen = (int)formatted.size();
    }
    setStatus("Loaded: " + name + " (ready)");
    g_loading = false;
}

void generateWorker(std::string userMsg) {
    g_convo.push_back({ "user", userMsg });
    std::string formatted = applyTemplate(g_convo, true);
    std::string delta = ((size_t)g_prevLen < formatted.size()) ? formatted.substr(g_prevLen) : formatted;
    std::vector<llama_token> toks = tokenize(delta, g_prevLen == 0);   // BOS only if nothing decoded yet
    if (!decodeTokens(toks)) { std::lock_guard<std::mutex> lk(g_mtx); g_partial += "\n[context full — reload the model to reset]"; }

    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.4f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(0xFFFFFFFFu));

    std::string resp;
    const int budget = 768;
    for (int i = 0; i < budget && !g_stop.load(); i++) {
        llama_token id = llama_sampler_sample(smpl, g_ctx, -1);
        if (llama_vocab_is_eog(g_vocab, id)) break;
        char piece[256];
        int np = llama_token_to_piece(g_vocab, id, piece, sizeof(piece), 0, true);
        if (np > 0) { resp.append(piece, np); std::lock_guard<std::mutex> lk(g_mtx); g_partial.append(piece, np); }
        llama_batch b = llama_batch_get_one(&id, 1);
        if (llama_decode(g_ctx, b) != 0) break;
    }
    llama_sampler_free(smpl);

    g_convo.push_back({ "assistant", resp });
    g_prevLen = (int)applyTemplate(g_convo, false).size();   // next turn's delta starts after this
    { std::lock_guard<std::mutex> lk(g_mtx); g_history.push_back({ "assistant", resp }); g_partial.clear(); }
    g_generating = false;
}

void startLoad(const std::string& path) {
    if (g_loading || g_generating) return;
    joinWorker();
    g_loading = true;
    setStatus("Loading model... (first load takes a few seconds)");
    g_worker = std::thread(loadWorker, path);
}

void startAsk(const std::string& userMsg) {
    if (!g_ctx || g_loading || g_generating) return;
    { std::lock_guard<std::mutex> lk(g_mtx); g_history.push_back({ "user", userMsg }); g_partial.clear(); }
    joinWorker();
    g_stop = false; g_generating = true;
    g_worker = std::thread(generateWorker, userMsg);
}

} // anon namespace

void SetSystemPrompt(const std::string& sys) { std::lock_guard<std::mutex> lk(g_mtx); g_system = sys; }

void SetInsertHandler(std::function<std::string(const std::string&)> fn) { std::lock_guard<std::mutex> lk(g_mtx); g_insertHandler = std::move(fn); }

bool IsBusy() { return g_loading.load() || g_generating.load(); }

void Shutdown() {
    g_stop = true;
    joinWorker();
    if (g_ctx)   { llama_free(g_ctx); g_ctx = nullptr; }
    if (g_model) { llama_model_free(g_model); g_model = nullptr; }
    if (g_backendInit.load()) llama_backend_free();
}

void RenderPanel(bool* p_open) {
    if (!*p_open) return;
    ImGui::SetNextWindowSize(ImVec2(500, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Assistant (local LLM)", p_open)) { ImGui::End(); return; }

    static bool s_scanned = false;
    if (!s_scanned) {
        s_scanned = true;
        std::error_code ec;
        if (std::filesystem::exists("models", ec)) {
            for (auto& e : std::filesystem::directory_iterator("models", ec)) {
                if (e.path().extension() == ".gguf") {
                    std::string p = e.path().string();
                    strncpy(g_modelPath, p.c_str(), sizeof(g_modelPath) - 1);
                    break;
                }
            }
        }
    }

    bool busy = IsBusy();
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputText("##modelpath", g_modelPath, sizeof(g_modelPath));
    ImGui::SameLine();
    if (ImGui::Button("Settings", ImVec2(86, 0))) ImGui::OpenPopup("AsstSettings");
    ImGui::SameLine();
    if (ImGui::Button("Load", ImVec2(60, 0)) && g_modelPath[0]) startLoad(g_modelPath);
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("AsstSettings")) {
        ImGui::TextDisabled("Compute (applies on next Load)");
        ImGui::Separator();
        if (ImGui::RadioButton("CPU 50%", !g_useGpu && g_pct == 50)) { g_useGpu = false; g_pct = 50; }
        if (ImGui::RadioButton("CPU 75%", !g_useGpu && g_pct == 75)) { g_useGpu = false; g_pct = 75; }
        if (ImGui::RadioButton("GPU 50%", g_useGpu && g_pct == 50)) { g_useGpu = true; g_pct = 50; }
        if (ImGui::RadioButton("GPU 75%", g_useGpu && g_pct == 75)) { g_useGpu = true; g_pct = 75; }
        if (ImGui::RadioButton("GPU 100% (full offload)", g_useGpu && g_pct >= 100)) { g_useGpu = true; g_pct = 100; }
        ImGui::Separator();
        ImGui::TextUnformatted("CPU = share of cores used.\nGPU = share of layers offloaded;\n100% = whole model on GPU (best if it\nfits in VRAM). Needs a GPU-enabled build.");
        ImGui::EndPopup();
    }
    { std::lock_guard<std::mutex> lk(g_mtx); ImGui::TextWrapped("%s", g_status.c_str()); }
    ImGui::Separator();

    ImGui::BeginChild("hist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4), true);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto& m : g_history) {
            bool user = (m.role == "user");
            ImGui::PushStyleColor(ImGuiCol_Text, user ? ImVec4(0.6f, 0.85f, 1.0f, 1.0f) : ImVec4(0.8f, 1.0f, 0.75f, 1.0f));
            ImGui::TextUnformatted(user ? "You:" : "Assistant:");
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", m.content.c_str());
            ImGui::Spacing();
        }
        if (g_generating.load()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 1.0f, 0.75f, 1.0f));
            ImGui::TextUnformatted("Assistant:");
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", g_partial.empty() ? "..." : g_partial.c_str());
        }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // If the latest reply contains a node graph, offer to insert it.
    {
        std::string lastReply, insStatus; bool haveHandler;
        { std::lock_guard<std::mutex> lk(g_mtx);
          if (!g_history.empty() && g_history.back().role == "assistant") lastReply = g_history.back().content;
          insStatus = g_insertStatus; haveHandler = (bool)g_insertHandler; }
        if (haveHandler && lastReply.find("bpVsNode=") != std::string::npos && !busy) {
            if (ImGui::Button("Insert nodes into open blueprint")) {
                std::string r = g_insertHandler(lastReply);
                std::lock_guard<std::mutex> lk(g_mtx);
                g_insertStatus = r;
            }
            if (!insStatus.empty()) { ImGui::TextWrapped("%s", insStatus.c_str()); }
        }
    }

    bool canSend = (g_ctx != nullptr) && !busy;
    ImGui::BeginDisabled(!canSend);
    ImGui::SetNextItemWidth(-70.0f);
    bool enter = ImGui::InputText("##askinput", g_input, sizeof(g_input), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool send = ImGui::Button("Send", ImVec2(60, 0));
    ImGui::EndDisabled();
    if ((enter || send) && canSend && g_input[0]) { startAsk(g_input); g_input[0] = 0; }
    if (g_generating.load()) { ImGui::SameLine(); if (ImGui::Button("Stop")) g_stop = true; }

    ImGui::End();
}

} // namespace llm
