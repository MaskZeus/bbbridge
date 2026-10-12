// Points an installed bbport (bloodborne_pc for Windows) at the bridge, or back.
// "Routed" means (a backup of every file is kept as *.before-bbbridge):
//  1. %APPDATA%\bbport-launcher\settings.json: online on; co-op server
//     127.0.0.1:31313; WebAPI http://127.0.0.1:31315; extra variable
//     SHADPS4_P2P_PORT=3658 (the bridge must know the port's P2P UDP port).
//  2. <saves>\host_overrides.custom.json: the game's own server traffic goes
//     through the bridge (the account token, and the play log to The Hunter's
//     Dream instead of FromSoftware's closed upload bucket).
//  3. <bbport>\patches\bbbridge-live-map.xml (optional): play log every 5 s,
//     position every 0.5 s - bbhost's own patch - for the site's live map.
// The bbport launcher must be closed while settings.json changes: it rewrites
// the file when it saves.
#pragma once

#include <filesystem>
#include <string>

#include "util.hpp"

namespace bb {

namespace fs = std::filesystem;

struct SetupError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

bool process_running(const wchar_t* image);
bool launcher_running();   // Bloodborne.exe
bool game_running();       // Play Bloodborne.exe / bb-probe.exe
bool is_bbport_dir(const fs::path& dir);

class BbportSetup {
public:
    // bbport_dir: "" = from the launcher's settings, or the folder above this exe.
    explicit BbportSetup(const fs::path& exe_dir, const fs::path& bbport_dir = {});
    void reload();

    fs::path bbport, settings_file, user_dir, patches_dir;
    nlohmann::ordered_json settings;   // the launcher's file, key order kept

    std::string npid() const { return jstr(settings, "online_npid"); }
    bool routed() const;
    bool patched() const;

    // the caller makes sure the launcher is closed
    void set_npid(const std::string& npid);
    void enable();
    void disable();
    void set_patch(bool on);

    fs::path play_exe() const { return bbport / L"Play Bloodborne.exe"; }
    fs::path launcher_exe() const { return bbport / L"Bloodborne.exe"; }

private:
    void save_settings();
    fs::path overrides_file() const { return user_dir / L"host_overrides.custom.json"; }
    fs::path patch_file() const { return patches_dir / L"bbbridge-live-map.xml"; }
    fs::path exe_dir_, chosen_;
};

}  // namespace bb
