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
#include "ggml-backend.h"   // GPU device enumeration (ggml_backend_dev_*)
#include "imgui.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

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

// Detected GPU devices (filled once in ensureBackend, on the UI thread). When GPU
// mode is on, the model is pinned to g_gpuDevs[g_gpuDevIdx] via mp.devices.
struct GpuDev { std::string name; ggml_backend_dev_t dev; };
std::vector<GpuDev> g_gpuDevs;
int                 g_gpuDevIdx = 0;

std::function<std::string(const std::string&)> g_insertHandler;   // [g] set once at startup
std::string g_insertStatus;                                       // [g] feedback after an insert

std::function<std::string()> g_grammarProvider;                   // [g] builds GBNF from live project
std::string g_grammar;                                            // refreshed UI-side before each generate
std::function<std::string(const std::string&)> g_lintHandler;     // [g] read-only lint -> issues (thread-safe)
std::function<std::string()> g_contextProvider;                   // [g] per-turn selection snapshot (UI thread)
std::function<std::string()> g_editContextProvider;               // [g] whole-graph context for the Edit button
bool g_useGrammar = false;   // constrain output to node-graph syntax (grammar)
bool g_repair     = true;    // auto-repair graph lint errors by re-prompting
int  g_ctxK       = 16;      // context window in K tokens (16 or 32); applied at load

void setStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_mtx); g_status = s; }

void ensureBackend() {
    if (!g_backendInit.exchange(true)) {
        llama_log_set([](enum ggml_log_level, const char*, void*){}, nullptr);  // silence llama spam
        llama_backend_init();
        // Enumerate GPU devices once so Settings can offer a device picker.
        size_t n = ggml_backend_dev_count();
        for (size_t i = 0; i < n; ++i) {
            ggml_backend_dev_t d = ggml_backend_dev_get(i);
            if (ggml_backend_dev_type(d) == GGML_BACKEND_DEVICE_TYPE_GPU) {
                const char* desc = ggml_backend_dev_description(d);
                std::string label = (desc && *desc) ? desc : "GPU";
                // Tag the device with its driver/backend (Vulkan, CUDA, ...) so the
                // dropdown doubles as a driver picker when several are compiled in.
                ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(d);
                const char* drv = reg ? ggml_backend_reg_name(reg) : nullptr;
                if (drv && *drv) label += std::string(" (") + drv + ")";
                g_gpuDevs.push_back({ label, d });
            }
        }
    }
}

void joinWorker() { if (g_worker.joinable()) g_worker.join(); }

// Compute settings persist in a small ini next to the editor so the GPU choice
// survives restarts.
const char* kSettingsFile = "assistant_prefs.ini";
void saveSettings() {
    std::ofstream f(kSettingsFile, std::ios::trunc);
    if (!f) return;
    f << "useGpu=" << (g_useGpu ? 1 : 0) << "\n"
      << "pct="    << g_pct << "\n"
      << "gpuDev=" << g_gpuDevIdx << "\n"
      << "grammar=" << (g_useGrammar ? 1 : 0) << "\n"
      << "repair="  << (g_repair ? 1 : 0) << "\n"
      << "ctxK="    << g_ctxK << "\n";
}
void loadSettings() {
    std::ifstream f(kSettingsFile);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if      (k == "useGpu")  g_useGpu = (v == "1");
        else if (k == "pct")     g_pct = std::atoi(v.c_str());
        else if (k == "gpuDev")  g_gpuDevIdx = std::atoi(v.c_str());
        else if (k == "grammar") g_useGrammar = (v == "1");
        else if (k == "repair")  g_repair = (v == "1");
        else if (k == "ctxK")    { g_ctxK = std::atoi(v.c_str()); if (g_ctxK != 16 && g_ctxK != 32) g_ctxK = 16; }
    }
}

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
    // Free any previously loaded model first (this runs on a reload / device change).
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_ctx)   { llama_free(g_ctx); g_ctx = nullptr; }
        if (g_model) { llama_model_free(g_model); g_model = nullptr; }
        g_vocab = nullptr;
    }
    unsigned hc = std::thread::hardware_concurrency(); if (!hc) hc = 4;

    // GPU offload: read the model's layer count cheaply (vocab-only) so we can
    // offload g_pct% of the layers (the rest stay on CPU). Inert on a CPU-only
    // llama build (no GPU device -> n_gpu_layers is ignored and it runs on CPU).
    int ngl = 0;
    if (g_useGpu) {
        llama_model_params vp = llama_model_default_params();
        vp.vocab_only = true; vp.n_gpu_layers = 0;
        llama_model* vm = llama_model_load_from_file(path.c_str(), vp);
        if (vm) {
            int nl = llama_model_n_layer(vm);
            if (g_pct >= 100) ngl = -1;                          // -1 = offload ALL layers
            else { ngl = (nl * g_pct) / 100; if (ngl < 1) ngl = 1; }  // clamp only the partial case
            llama_model_free(vm);
        }
        else ngl = -1;   // couldn't read the count; offload all
    }

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = ngl;
    // Pin offload to the GPU chosen in Settings (NULL-terminated device list).
    ggml_backend_dev_t devsel[2] = { nullptr, nullptr };
    if (g_useGpu && g_gpuDevIdx >= 0 && g_gpuDevIdx < (int)g_gpuDevs.size()) {
        devsel[0] = g_gpuDevs[g_gpuDevIdx].dev;
        mp.devices = devsel;
    }
    llama_model* m = llama_model_load_from_file(path.c_str(), mp);
    if (!m) { setStatus("Failed to load model: " + path); g_loading = false; return; }
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx   = (uint32_t)g_ctxK * 1024;   // 16K/32K: catalog prompt + chat + repair of large graphs
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
    std::string savedStatus; { std::lock_guard<std::mutex> lk(g_mtx); savedStatus = g_status; }
    int maxRepairs = g_repair ? 2 : 0;
    std::string pending = userMsg;

    for (int attempt = 0; ; attempt++) {
        g_convo.push_back({ "user", pending });
        std::string formatted = applyTemplate(g_convo, true);
        std::string delta = ((size_t)g_prevLen < formatted.size()) ? formatted.substr(g_prevLen) : formatted;
        std::vector<llama_token> toks = tokenize(delta, g_prevLen == 0);   // BOS only if nothing decoded yet
        if (!decodeTokens(toks)) { std::lock_guard<std::mutex> lk(g_mtx); g_partial += "\n[context full — reload the model to reset]"; }

        llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
        // Grammar first so it masks invalid tokens before the distribution samplers pick.
        // LAZY grammar: the model generates freely (a reasoning model can THINK, any model
        // can write a sentence first) until the first "bpVs..." token appears, then the
        // grammar clamps the rest to valid graph syntax. Pure Q&A never triggers it.
        if (g_useGrammar && !g_grammar.empty()) {
            const char* triggers[] = { "[\\s\\S]*?(bpVs[A-Za-z]+=)" };
            llama_sampler* gr = llama_sampler_init_grammar_lazy_patterns(g_vocab, g_grammar.c_str(), "root", triggers, 1, nullptr, 0);
            if (gr) llama_sampler_chain_add(smpl, gr);
        }
        llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.4f));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(0xFFFFFFFFu));

        { std::lock_guard<std::mutex> lk(g_mtx); g_partial.clear(); }
        std::string resp;
        std::unordered_set<std::string> seenLines; std::string curLine; int dupLines = 0; bool sawGraph = false;
        const int budget = 2048;   // room for larger graphs / repair passes (n_ctx is 16k)
        for (int i = 0; i < budget && !g_stop.load(); i++) {
            llama_token id = llama_sampler_sample(smpl, g_ctx, -1);
            if (llama_vocab_is_eog(g_vocab, id)) break;
            char piece[256];
            int np = llama_token_to_piece(g_vocab, id, piece, sizeof(piece), 0, true);
            bool looped = false;
            if (np > 0) {
                resp.append(piece, np);
                { std::lock_guard<std::mutex> lk(g_mtx); g_partial.append(piece, np); }
                // A valid graph never repeats an exact "bpVs..." line (ids/links are
                // unique), so several duplicate lines mean the model is stuck looping.
                for (int k = 0; k < np; k++) {
                    if (piece[k] == '\n') {
                        if (curLine.compare(0, 4, "bpVs") == 0) {
                            sawGraph = true;
                            if (!seenLines.insert(curLine).second && ++dupLines >= 4) looped = true;   // stuck repeating
                        } else if (sawGraph && curLine.compare(0, 3, "```") == 0) {
                            looped = true;   // closing fence after the graph — the model is done
                        }
                        curLine.clear();
                    } else {
                        curLine.push_back(piece[k]);
                        // Backstop for a runaway GRAPH line (e.g. endless digits) — never trips
                        // on prose/reasoning, which legitimately has long lines.
                        if (curLine.size() > 256 && curLine.compare(0, 4, "bpVs") == 0) looped = true;
                    }
                }
            }
            if (looped) break;
            llama_batch b = llama_batch_get_one(&id, 1);
            if (llama_decode(g_ctx, b) != 0) break;
        }
        llama_sampler_free(smpl);

        g_convo.push_back({ "assistant", resp });
        g_prevLen = (int)applyTemplate(g_convo, false).size();   // next turn's delta starts after this
        { std::lock_guard<std::mutex> lk(g_mtx); g_history.push_back({ "assistant", resp }); g_partial.clear(); }

        // Auto-repair: lint the graph read-only; if it's broken and we still have
        // passes left, feed the exact problems back and regenerate the full graph.
        std::string issues;
        if (maxRepairs > 0 && g_lintHandler && !g_stop.load() && resp.find("bpVsNode=") != std::string::npos)
            issues = g_lintHandler(resp);
        if (issues.empty() || attempt >= maxRepairs) break;

        int n = 0; for (char c : issues) if (c == '\n') n++;
        { std::lock_guard<std::mutex> lk(g_mtx); g_history.push_back({ "user", "auto-repair: fixing " + std::to_string(n) + " graph issue(s)..." }); }
        setStatus("repairing graph (pass " + std::to_string(attempt + 1) + ")...");
        pending = "The node graph you just produced has these problems:\n" + issues +
                  "Output the COMPLETE corrected graph again — every bpVsNode / bpVsLink / bpVsNodeClip "
                  "line — with these fixed. Keep node ids stable where you can. Output only the graph.";
    }

    setStatus(savedStatus);
    g_generating = false;
}

#ifdef _WIN32
// Native open-file dialog to pick a .gguf model (any folder). Returns true and
// fills `out` with the chosen path; false if the user cancels.
bool pickGguf(char* out, int outSize) {
    char file[1024] = {};
    if (out[0]) strncpy(file, out, sizeof(file) - 1);   // preselect the current path
    std::error_code ec;
    std::string initDir;
    if (out[0]) initDir = std::filesystem::path(out).parent_path().string();
    else if (std::filesystem::exists("models", ec)) initDir = (std::filesystem::current_path(ec) / "models").string();
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "GGUF models (*.gguf)\0*.gguf\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof(file);
    ofn.lpstrTitle  = "Select a GGUF model";
    if (!initDir.empty()) ofn.lpstrInitialDir = initDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) { strncpy(out, file, outSize - 1); out[outSize - 1] = 0; return true; }
    return false;
}
#endif

void startLoad(const std::string& path) {
    if (g_loading || g_generating) return;
    joinWorker();
    g_loading = true;
    setStatus("Loading model... (first load takes a few seconds)");
    g_worker = std::thread(loadWorker, path);
}

// displayMsg is what the user typed (shown in the chat); modelMsg is what the model
// actually receives (may have the selection snapshot prepended).
void startAsk(const std::string& displayMsg, const std::string& modelMsg) {
    if (!g_ctx || g_loading || g_generating) return;
    { std::lock_guard<std::mutex> lk(g_mtx); g_history.push_back({ "user", displayMsg }); g_partial.clear(); }
    joinWorker();
    g_stop = false; g_generating = true;
    g_worker = std::thread(generateWorker, modelMsg);
}

} // anon namespace

void SetSystemPrompt(const std::string& sys) { std::lock_guard<std::mutex> lk(g_mtx); g_system = sys; }

void SetInsertHandler(std::function<std::string(const std::string&)> fn) { std::lock_guard<std::mutex> lk(g_mtx); g_insertHandler = std::move(fn); }

void SetGrammarProvider(std::function<std::string()> fn) { std::lock_guard<std::mutex> lk(g_mtx); g_grammarProvider = std::move(fn); }

void SetLintHandler(std::function<std::string(const std::string&)> fn) { std::lock_guard<std::mutex> lk(g_mtx); g_lintHandler = std::move(fn); }

void SetContextProvider(std::function<std::string()> fn) { std::lock_guard<std::mutex> lk(g_mtx); g_contextProvider = std::move(fn); }

void SetEditContextProvider(std::function<std::string()> fn) { std::lock_guard<std::mutex> lk(g_mtx); g_editContextProvider = std::move(fn); }

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
        ensureBackend();   // registers backends + fills g_gpuDevs (on the UI thread)
        loadSettings();    // restore saved compute choice (CPU/GPU, %, device)
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
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Settings stays enabled even while busy: the compute choice only applies on
    // the next Load, and locking it during a slow load is what trapped users on CPU.
    if (ImGui::Button("Settings", ImVec2(86, 0))) ImGui::OpenPopup("AsstSettings");
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Load", ImVec2(60, 0))) {
#ifdef _WIN32
        if (pickGguf(g_modelPath, sizeof(g_modelPath))) startLoad(g_modelPath);   // browse for a .gguf
#else
        if (g_modelPath[0]) startLoad(g_modelPath);
#endif
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("AsstSettings")) {
        bool changed = false;
        ImGui::TextDisabled("Compute (saved; reloads the model)");
        ImGui::Separator();
        if (ImGui::RadioButton("CPU 50%", !g_useGpu && g_pct == 50)) { g_useGpu = false; g_pct = 50; changed = true; }
        if (ImGui::RadioButton("CPU 75%", !g_useGpu && g_pct == 75)) { g_useGpu = false; g_pct = 75; changed = true; }
        if (ImGui::RadioButton("GPU 50%", g_useGpu && g_pct == 50)) { g_useGpu = true; g_pct = 50; changed = true; }
        if (ImGui::RadioButton("GPU 75%", g_useGpu && g_pct == 75)) { g_useGpu = true; g_pct = 75; changed = true; }
        if (ImGui::RadioButton("GPU 100% (full offload)", g_useGpu && g_pct >= 100)) { g_useGpu = true; g_pct = 100; changed = true; }
        ImGui::Separator();
        ImGui::TextDisabled("GPU device");
        if (g_gpuDevs.empty()) {
            ImGui::TextDisabled("(no GPU detected — CPU-only build?)");
        } else {
            if (g_gpuDevIdx < 0 || g_gpuDevIdx >= (int)g_gpuDevs.size()) g_gpuDevIdx = 0;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##gpudev", g_gpuDevs[g_gpuDevIdx].name.c_str())) {
                for (int i = 0; i < (int)g_gpuDevs.size(); ++i)
                    if (ImGui::Selectable(g_gpuDevs[i].name.c_str(), i == g_gpuDevIdx)) { g_gpuDevIdx = i; changed = true; }
                ImGui::EndCombo();
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Context window");
        if (ImGui::RadioButton("16K", g_ctxK == 16)) { g_ctxK = 16; changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("32K", g_ctxK == 32)) { g_ctxK = 32; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bigger window = larger graphs + more reasoning room,\nbut ~2x the VRAM for the KV cache. 32K may not fully\nfit a 14B on a 16GB GPU (spills to CPU = slower).");
        // Persist the choice, and if a model is already loaded, restart it now so
        // the new compute/context setting takes effect immediately.
        if (changed) {
            saveSettings();
            if (g_ctx && !IsBusy() && g_modelPath[0]) startLoad(g_modelPath);
        }
        ImGui::Separator();
        ImGui::TextDisabled("Node-graph generation");
        if (ImGui::Checkbox("Constrain output (grammar)", &g_useGrammar)) saveSettings();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clamps the GRAPH part of a reply to valid syntax\n(real node types + clip names). Prose, reasoning,\nand plain Q&A stay free (the graph is only\nconstrained once it starts), so you can leave this on.");
        if (ImGui::Checkbox("Auto-repair graph errors", &g_repair)) saveSettings();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("After generating, lint the graph and re-prompt\nthe model to fix any broken links / bad pins /\nunwired Key pins (up to 2 passes).");
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
        bool hasNodes = lastReply.find("bpVsNode=") != std::string::npos;
        bool hasEdits = lastReply.find("bpVsSet=") != std::string::npos || lastReply.find("bpVsSetBit=") != std::string::npos;
        if (haveHandler && (hasNodes || hasEdits) && !busy) {
            const char* label = hasNodes ? "Insert nodes into open blueprint"
                                         : "Apply edits to selected nodes";
            if (ImGui::Button(label)) {
                std::string r = g_insertHandler(lastReply);
                std::lock_guard<std::mutex> lk(g_mtx);
                g_insertStatus = r;
            }
            ImGui::SameLine();
            if (ImGui::Button("Repair")) {
                // Re-lint the last graph and, if it has problems, ask the model to fix it.
                std::string issues = g_lintHandler ? g_lintHandler(lastReply) : std::string();
                if (issues.empty()) { std::lock_guard<std::mutex> lk(g_mtx); g_insertStatus = "Lint found no issues to repair."; }
                else {
                    if (g_useGrammar && g_grammarProvider) g_grammar = g_grammarProvider();
                    std::string msg = "The node graph above has these problems:\n" + issues +
                        "Output the COMPLETE corrected graph again — every bpVsNode / bpVsLink / bpVsSet line — with these fixed. Output only the graph.";
                    startAsk("(repair the graph)", msg);
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-check the graph and ask the model to fix any\nbroken links / out-of-range pins / unwired Key nodes.");
            if (!insStatus.empty()) { ImGui::TextWrapped("%s", insStatus.c_str()); }
        }
    }

    bool canSend = (g_ctx != nullptr) && !busy;
    ImGui::BeginDisabled(!canSend);
    ImGui::SetNextItemWidth(-126.0f);
    bool enter = ImGui::InputText("##askinput", g_input, sizeof(g_input), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool edit = ImGui::Button("Edit", ImVec2(54, 0));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Apply your request as a CHANGE to the existing graph\n(adds nodes, wires them to what's there, tweaks params)\ninstead of building a new graph from scratch.");
    ImGui::SameLine();
    bool send = ImGui::Button("Send", ImVec2(54, 0));
    ImGui::EndDisabled();
    if ((enter || send || edit) && canSend && g_input[0]) {
        // Rebuild grammar here (UI thread) so it reflects the current project.
        if (g_useGrammar && g_grammarProvider) g_grammar = g_grammarProvider();
        std::string disp = g_input;
        // Edit => feed the WHOLE current graph (extend/modify it). Send => normal
        // (selection snapshot if any nodes are selected, else just the prompt).
        std::string ctx;
        if (edit) { if (g_editContextProvider) ctx = g_editContextProvider(); }
        else      { if (g_contextProvider)     ctx = g_contextProvider(); }
        std::string model = ctx.empty() ? disp : (ctx + "\nUser request: " + disp);
        startAsk(disp, model);
        g_input[0] = 0;
    }
    if (g_generating.load()) { ImGui::SameLine(); if (ImGui::Button("Stop")) g_stop = true; }

    ImGui::End();
}

} // namespace llm
