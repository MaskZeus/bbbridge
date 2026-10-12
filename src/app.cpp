// bbbridge.exe: the bridge with a window. It sits in a folder inside bbport's
// (E:\bloodborne\bbbridge\), keeps its config, account and log there, points
// bbport at itself, and starts the game.
#include "bridge.hpp"   // first: winsock2.h before anything pulls in windows.h's winsock.h

#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>

#include "account_link.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "setup.hpp"

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using namespace bb;

// ---- D3D11 boilerplate (from Dear ImGui's example_win32_directx11) ----
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
bool g_close_requested = false;

void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

void release_rtv() {
    if (g_rtv) g_rtv->Release();
    g_rtv = nullptr;
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)   // no GPU driver: the software rasterizer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    create_rtv();
    return true;
}

void destroy_device() {
    release_rtv();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_resize_w = LOWORD(lp);
            g_resize_h = HIWORD(lp);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        g_close_requested = true;   // the app decides (the game may still be running)
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- the app ----

fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

bool launch(const fs::path& exe, const fs::path& cwd) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::optional<fs::path> pick_folder(HWND owner) {
    std::optional<fs::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS);
    dlg->SetTitle(L"bbport folder (the one with Bloodborne.exe)");
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

class App {
public:
    explicit App(HWND hwnd) : hwnd_(hwnd), dir_(exe_dir()) {
        set_log_file((dir_ / L"bbbridge.log").wstring());
        set_log_sink([this](const std::string& line) {
            std::lock_guard<std::mutex> lk(log_mu_);
            lines_.push_back(line);
            while (lines_.size() > 3000) lines_.pop_front();
            scroll_ = true;
        });
        load_config();
        reload_setup();
        log(fmt("bbbridge %s - %s", kVersion, setup_ ? "bbport found" : "bbport not found yet"));
        if (setup_ && setup_->routed()) start_bridge();   // routed already: be ready when the game starts
    }

    ~App() { stop_bridge(); }

    void frame();
    bool wants_close() {
        if (!g_close_requested) return false;
        if (game_running_ && bridge_) {
            confirm_close_ = true;
            g_close_requested = false;
            return false;
        }
        return true;
    }

private:
    void load_config();
    void save_config();
    void reload_setup();
    void refresh();
    void start_bridge();
    void stop_bridge();
    void play();
    void status_panel();
    void setup_panel();
    void account_row();
    void account_popups();
    void log_panel();
    void use_npid(const std::string& npid);
    std::wstring state_path() const { return (dir_ / L"bbbridge_state.json").wstring(); }
    std::string auth_server() const { return jstr(config_, "auth_server", "https://thehuntersdream.com"); }

    HWND hwnd_;
    fs::path dir_;
    json config_ = json::object();
    std::unique_ptr<Bridge> bridge_;
    std::optional<BbportSetup> setup_;
    std::string setup_error_;
    fs::path chosen_bbport_;
    char npid_[64] = {};
    bool launcher_running_ = false, game_running_ = false, routed_ = false, patched_ = false;
    double last_refresh_ = 0;
    std::string notice_;
    bool confirm_close_ = false;
    std::unique_ptr<DiscordLink> link_;
    bool open_link_ = false, open_web_ = false;
    std::string web_code_, web_error_;
    std::mutex log_mu_;
    std::deque<std::string> lines_;
    bool scroll_ = false;
};

void App::load_config() {
    std::ifstream in(dir_ / L"bbbridge_config.json");
    if (in) {
        json j = json::parse(in, nullptr, false);
        if (j.is_object()) config_ = j;
    }
    if (jhas(config_, "bbport_dir")) chosen_bbport_ = fs::path(widen(jstr(config_, "bbport_dir")));
}

void App::save_config() {
    std::ofstream(dir_ / L"bbbridge_config.json", std::ios::trunc) << config_.dump(2) << "\n";
}

void App::reload_setup() {
    try {
        setup_.emplace(dir_, chosen_bbport_);
        setup_error_.clear();
        const std::string n = setup_->npid();
        strncpy_s(npid_, n.c_str(), _TRUNCATE);
    } catch (const std::exception& e) {
        setup_.reset();
        setup_error_ = e.what();
    }
    refresh();
}

void App::refresh() {
    launcher_running_ = launcher_running();
    game_running_ = game_running();
    if (setup_) {
        try {
            if (!launcher_running_) {
                setup_->reload();   // the launcher may have saved new settings
                strncpy_s(npid_, setup_->npid().c_str(), _TRUNCATE);
            }
        } catch (...) {
        }
        routed_ = setup_->routed();
        patched_ = setup_->patched();
    }
    last_refresh_ = now();
}

void App::start_bridge() {
    if (bridge_) return;
    Config cfg = Config::from_json(config_);
    cfg.state_file = narrow((dir_ / L"bbbridge_state.json").wstring());   // the account, next to the exe
    auto b = std::make_unique<Bridge>(cfg);
    std::string error;
    if (!b->start(&error)) {
        notice_ = "The bridge could not start: " + error;
        return;
    }
    bridge_ = std::move(b);
    notice_.clear();
}

void App::stop_bridge() {
    if (!bridge_) return;
    bridge_->stop();
    bridge_.reset();
    log("bridge stopped");
}

void App::play() {
    if (!setup_) return;
    if (launcher_running_) {
        notice_ = "Close the bbport launcher first - it would undo bbbridge's settings when it saves.";
        return;
    }
    try {
        if (std::string(npid_) != setup_->npid()) setup_->set_npid(npid_);
        if (!routed_) setup_->enable();
    } catch (const std::exception& e) {
        notice_ = std::string("Could not set bbport up: ") + e.what();
        return;
    }
    start_bridge();
    if (!bridge_) return;
    if (!launch(setup_->play_exe(), setup_->bbport)) {
        notice_ = "Could not start " + narrow(setup_->play_exe().wstring());
        return;
    }
    log("starting the game: " + narrow(setup_->play_exe().wstring()));
    refresh();
}

void App::status_panel() {
    const BridgeStatus st = bridge_ ? bridge_->status() : BridgeStatus{};
    const ImVec4 green(0.35f, 0.85f, 0.45f, 1), grey(0.6f, 0.6f, 0.6f, 1), amber(0.95f, 0.75f, 0.3f, 1);
    ImGui::TextColored(bridge_ ? green : grey, bridge_ ? "  Bridge running" : "  Bridge stopped");
    if (bridge_) {
        ImGui::SameLine();
        if (st.relay_port)
            ImGui::TextDisabled("  relay port %d%s", st.relay_port,
                                st.mapped ? (" - seen as " + st.mapped->str()).c_str() : "");
        else
            ImGui::TextDisabled("  waiting for The Hunter's Dream's STUN answer");
    }
    if (bridge_) {
        if (st.logged_in) ImGui::Text("Signed in as %s", st.npid.c_str());
        else if (!st.npid.empty()) ImGui::TextColored(amber, "Signing in as %s ...", st.npid.c_str());
        else ImGui::TextDisabled("Waiting for the game to connect");
        if (st.room_id) ImGui::Text("Room %llu - %d member(s)", static_cast<unsigned long long>(st.room_id), st.room_members);
        if (!st.peers.empty() && ImGui::BeginTable("peers", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Player");
            ImGui::TableSetupColumn("Member");
            ImGui::TableSetupColumn("Packets out");
            ImGui::TableSetupColumn("Packets in");
            ImGui::TableHeadersRow();
            for (const auto& p : st.peers) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.online_id.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", p.member_id);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(p.to_peer));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(p.to_port));
            }
            ImGui::EndTable();
        }
    }
}

void App::setup_panel() {
    ImGui::SeparatorText("Setup");
    if (!setup_) {
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", setup_error_.c_str());
        if (ImGui::Button("Choose bbport folder...")) {
            if (auto p = pick_folder(hwnd_)) {
                chosen_bbport_ = *p;
                config_["bbport_dir"] = narrow(p->wstring());
                save_config();
                reload_setup();
            }
        }
        return;
    }
    ImGui::Text("bbport:");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", narrow(setup_->bbport.wstring()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Change...")) {
        if (auto p = pick_folder(hwnd_)) {
            chosen_bbport_ = *p;
            config_["bbport_dir"] = narrow(p->wstring());
            save_config();
            reload_setup();
        }
    }
    account_row();
    ImGui::BeginDisabled(launcher_running_);
    bool routed = routed_;
    if (ImGui::Checkbox("Route bbport through bbbridge", &routed)) {
        try {
            if (routed) setup_->enable();
            else setup_->disable();
            log(routed ? "bbport now uses bbbridge" : "bbport back on the public shadNet server");
        } catch (const std::exception& e) {
            notice_ = e.what();
        }
        refresh();
    }
    bool patched = patched_;
    if (ImGui::Checkbox("Live map patch (show me on thehuntersdream.com's map)", &patched)) {
        setup_->set_patch(patched);
        refresh();
    }
    ImGui::EndDisabled();
    if (launcher_running_)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "The bbport launcher is open - close it to change these.");
    bool trace = jint(config_, "trace", 0) != 0;
    if (ImGui::Checkbox("Detailed log (for bug reports; applies when the bridge restarts)", &trace)) {
        config_["trace"] = trace;
        save_config();
    }
}

void App::use_npid(const std::string& npid) {
    strncpy_s(npid_, npid.c_str(), _TRUNCATE);
    if (!setup_) return;
    if (launcher_running_) {
        notice_ = "Close the bbport launcher, then sign in again to use '" + npid + "'.";
        return;
    }
    try {
        setup_->set_npid(npid);
        log(fmt("online id set to '%s'", npid.c_str()));
    } catch (const std::exception& e) {
        notice_ = e.what();
    }
}

void App::account_row() {
    // Everyone signs in with their own Discord account on The Hunter's Dream;
    // the account's name becomes bbport's online id (NPID).
    const std::string kind = account_kind(state_path(), npid_);
    if (!kind.empty()) {
        ImGui::TextColored(ImVec4(0.55f, 0.65f, 1.0f, 1), "Signed in as '%s'%s", npid_,
                           kind == "discord" ? " (Discord)" : " (bbbridge account)");
        ImGui::BeginDisabled(launcher_running_);
        if (ImGui::Button("Website sign-in code")) {
            web_code_ = website_code(auth_server(), state_path(), npid_, web_error_);
            open_web_ = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A code for thehuntersdream.com/account, 'Sign in with a code from the game'.");
        ImGui::SameLine();
        if (ImGui::Button("Switch account...")) {
            link_ = std::make_unique<DiscordLink>(auth_server(), state_path());
            link_->start();
            open_link_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Sign out")) {
            const std::string err = account_sign_out(auth_server(), state_path(), npid_);
            notice_ = err.empty() ? "" : "Sign out failed: " + err;
        }
        ImGui::EndDisabled();
        return;
    }
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "Not signed in. Sign in with your Discord account to play online.");
    ImGui::BeginDisabled(launcher_running_);
    if (ImGui::Button("Sign in with Discord...", ImVec2(220, 0))) {
        link_ = std::make_unique<DiscordLink>(auth_server(), state_path());
        link_->start();
        open_link_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("opens The Hunter's Dream: sign in there with Discord and approve the code");
}

void App::account_popups() {
    if (open_link_) {
        ImGui::OpenPopup("Sign in with Discord");
        open_link_ = false;
    }
    if (ImGui::BeginPopupModal("Sign in with Discord", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const DiscordLink::State st = link_ ? link_->state() : DiscordLink::State::Idle;
        if (st == DiscordLink::State::Starting) {
            ImGui::TextUnformatted("Asking The Hunter's Dream for a code ...");
        } else if (st == DiscordLink::State::Waiting) {
            ImGui::TextUnformatted("1. Open the sign-in page and sign in with Discord.");
            ImGui::TextUnformatted("2. Enter this code there:");
            ImGui::SetWindowFontScale(2.2f);
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.5f, 1), "   %s", link_->user_code().c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextDisabled("%s", link_->uri().c_str());
            if (ImGui::Button("Open sign-in page"))
                ShellExecuteW(nullptr, L"open", widen(link_->uri()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            if (ImGui::Button("Copy code")) ImGui::SetClipboardText(link_->user_code().c_str());
            ImGui::TextDisabled("Waiting for the approval ... (%d s left)", static_cast<int>(link_->seconds_left()));
        } else if (st == DiscordLink::State::Approved) {
            ImGui::Text("Signed in as '%s'.", link_->online_id().c_str());
            ImGui::TextUnformatted("bbport will use this account as its online id.");
        } else if (st == DiscordLink::State::Failed) {
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "Sign-in failed: %s", link_->message().c_str());
        }
        ImGui::Separator();
        if (st == DiscordLink::State::Approved) {
            if (ImGui::Button("Done")) {
                use_npid(link_->online_id());
                link_.reset();
                ImGui::CloseCurrentPopup();
            }
        } else {
            if (st == DiscordLink::State::Failed && ImGui::Button("Try again")) link_->start();
            if (st == DiscordLink::State::Failed) ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                link_.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (open_web_) {
        ImGui::OpenPopup("Website sign-in code");
        open_web_ = false;
    }
    if (ImGui::BeginPopupModal("Website sign-in code", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (web_code_.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "No code: %s", web_error_.c_str());
        } else {
            ImGui::Text("For thehuntersdream.com/account, 'Sign in with a code from the game' (account '%s'):", npid_);
            ImGui::SetWindowFontScale(2.2f);
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.5f, 1), "   %s", web_code_.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextDisabled("Valid for a few minutes.");
            if (ImGui::Button("Copy code")) ImGui::SetClipboardText(web_code_.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Open the account page"))
                ShellExecuteW(nullptr, L"open", widen(auth_server() + "/account").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Separator();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::log_panel() {
    ImGui::SeparatorText("Log");
    if (ImGui::SmallButton("Copy log")) {
        std::string all;
        {
            std::lock_guard<std::mutex> lk(log_mu_);
            for (const auto& l : lines_) all += l + "\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open folder")) ShellExecuteW(nullptr, L"open", dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(log_mu_);
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(lines_.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextUnformatted(lines_[i].c_str());
        if (scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
        scroll_ = false;
    }
    ImGui::EndChild();
}

void App::frame() {
    if (now() - last_refresh_ > 1.5) refresh();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("bbbridge", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::Text("bbbridge %s", kVersion);
    ImGui::SameLine();
    ImGui::TextDisabled("- co-op with bbhost players on The Hunter's Dream");
    ImGui::Spacing();
    status_panel();
    ImGui::Spacing();
    setup_panel();
    ImGui::Spacing();

    const bool signed_in = !account_kind(state_path(), npid_).empty();
    ImGui::BeginDisabled(!setup_ || game_running_ || !signed_in);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.16f, 0.16f, 1));
    if (ImGui::Button(game_running_ ? "  PLAYING  " : "    PLAY    ", ImVec2(160, 40))) play();
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    if (!signed_in && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Sign in with Discord first.");
    ImGui::SameLine();
    if (ImGui::Button(bridge_ ? "Stop bridge" : "Start bridge", ImVec2(130, 40))) {
        if (bridge_) stop_bridge();
        else start_bridge();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!setup_ || launcher_running_);
    if (ImGui::Button("bbport settings...", ImVec2(160, 40))) launch(setup_->launcher_exe(), setup_->bbport);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Opens the bbport launcher for graphics, mods and cheats. Close it before pressing PLAY here.");
    if (!notice_.empty()) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", notice_.c_str());
    ImGui::Spacing();
    log_panel();

    account_popups();
    if (confirm_close_) ImGui::OpenPopup("Close bbbridge?");
    if (ImGui::BeginPopupModal("Close bbbridge?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The game is still running. Closing bbbridge takes it offline.");
        if (ImGui::Button("Close anyway")) {
            confirm_close_ = false;
            g_close_requested = true;
            game_running_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            confirm_close_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // One bbbridge at a time: a second would fight the first over the ports.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\bbbridge-app");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"bbbridge", nullptr)) SetForegroundWindow(other);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    net_init();
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), nullptr,
                   nullptr, nullptr, L"bbbridge", nullptr};
    RegisterClassExW(&wc);
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"bbbridge", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              static_cast<int>(820 * scale), static_cast<int>(760 * scale), nullptr, nullptr, inst, nullptr);
    if (!create_device(hwnd)) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"bbbridge", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(14, 12);
    style.FrameRounding = 4;
    style.ScaleAllSizes(scale);
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2013, 0x2026, 0};   // Latin-1 + Ext-A
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * scale, nullptr, ranges);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    {
        App app(hwnd);
        bool done = false;
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;
            if (app.wants_close()) {
                DestroyWindow(hwnd);
                continue;
            }
            if (g_resize_w) {
                release_rtv();
                g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
                g_resize_w = g_resize_h = 0;
                create_rtv();
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            const float clear[4] = {0.07f, 0.07f, 0.08f, 1};
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_context->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_swap->Present(1, 0);
        }
    }   // the App stops the bridge here
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
