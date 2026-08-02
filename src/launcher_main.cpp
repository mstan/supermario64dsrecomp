#include "recomp_launcher.h"
#include "launcher_profile.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef SM64DS_RUNNER_EXE
#error "SM64DS_RUNNER_EXE must name the nds_runner executable"
#endif
#ifndef SM64DS_BIOS_DIR
#error "SM64DS_BIOS_DIR must name the firmware/BIOS directory"
#endif
#ifndef SM64DS_SOURCE_DIR
#error "SM64DS_SOURCE_DIR must name the game repository"
#endif

namespace {
namespace fs = std::filesystem;

constexpr char kKnownSha1[] =
    "1367529f2cb23e76ef295cb1727333ae8f0a6cd7";

struct ModState {
    bool adaptive_widescreen = true;
    fs::path state_path;
    std::string error;
};

template <size_t N>
void copy_text(char (&target)[N], const char* source) {
    std::snprintf(target, N, "%s", source ? source : "");
}

void load_mod_state(ModState& state) {
    std::ifstream input(state.state_path);
    std::string line;
    while (std::getline(input, line)) {
        if (line == "adaptive_widescreen=1")
            state.adaptive_widescreen = true;
        else if (line == "adaptive_widescreen=0")
            state.adaptive_widescreen = false;
    }
}

int mod_feature_count(void*) {
    return 1;
}

int mod_feature_get(void* context, int index,
                    RecompLauncherCModFeature* output) {
    if (!context || !output || index != 0) return 0;
    const auto* state = static_cast<const ModState*>(context);
    std::memset(output, 0, sizeof(*output));
    copy_text(output->id, "adaptive-widescreen");
    copy_text(output->package_id, "sm64ds-adaptive-widescreen");
    copy_text(output->package_version, "0.1.0");
    copy_text(output->package_name, "SM64DS Adaptive Widescreen");
    copy_text(output->name, "Adaptive Widescreen");
    copy_text(output->author, "ndsrecomp");
    copy_text(
        output->description,
        "Expands the top screen to 21:9 and keeps the HUD anchored to its "
        "appropriate corners. This mod forces Display > Screen layout to "
        "Separate windows; the bottom touchscreen remains native and clickable.");
    copy_text(output->group, "Display enhancements");
    copy_text(output->status,
              state->adaptive_widescreen ? "Enabled" : "Disabled");
    output->enabled = state->adaptive_widescreen ? 1 : 0;
    return 1;
}

int mod_feature_option_get(void*, const char*, const char*, int,
                           RecompLauncherCModOption*) {
    return 0;
}

int mod_feature_enable(void* context, const char* package_id,
                       const char* feature_id, int enabled) {
    if (!context || !package_id || !feature_id ||
        std::strcmp(package_id, "sm64ds-adaptive-widescreen") != 0 ||
        std::strcmp(feature_id, "adaptive-widescreen") != 0) {
        return 0;
    }
    static_cast<ModState*>(context)->adaptive_widescreen = enabled != 0;
    return 1;
}

int mod_feature_set_option(void*, const char*, const char*, const char*,
                           const char*) {
    return 0;
}

int mod_commit(void* context, const char*) {
    if (!context) return 0;
    auto* state = static_cast<ModState*>(context);
    std::ofstream output(state->state_path, std::ios::trunc);
    if (!output) {
        state->error = "Could not save SM64DS mod settings.";
        return 0;
    }
    output << "adaptive_widescreen="
           << (state->adaptive_widescreen ? 1 : 0) << '\n';
    state->error.clear();
    return 1;
}

const char* mod_last_error(void* context) {
    return context ? static_cast<ModState*>(context)->error.c_str()
                   : "Missing mod state.";
}

RecompLauncherCModProvider make_mod_provider(ModState* state) {
    RecompLauncherCModProvider provider{};
    provider.ctx = state;
    provider.commit = mod_commit;
    provider.last_error = mod_last_error;
    provider.feature_count = mod_feature_count;
    provider.feature_get = mod_feature_get;
    provider.feature_option_get = mod_feature_option_get;
    provider.feature_enable = mod_feature_enable;
    provider.feature_set_option = mod_feature_set_option;
    provider.archive_extension = ".ndsmod";
    provider.archive_description = "Nintendo DS mod package";
    return provider;
}

std::wstring quote(const fs::path& value) {
    return L"\"" + value.wstring() + L"\"";
}

bool launch_runner(const fs::path& rom, int display_layout, bool adaptive,
                   int renderer, int supersampling, int antialiasing) {
    const fs::path runner = fs::path(SM64DS_RUNNER_EXE);
    const fs::path bios = fs::path(SM64DS_BIOS_DIR);
    const fs::path source = fs::path(SM64DS_SOURCE_DIR);
    const fs::path config = source / "game.toml";
    const fs::path save = source / "saves" / "sm64ds_europe.sav";

    std::wstring command =
        quote(runner) + L" " + quote(bios) +
        L" --interactive --rom " + quote(rom) +
        L" --save-path " + quote(save) +
        L" --config " + quote(config) +
        L" --screen-layout " +
        (adaptive || display_layout == 1 ? L"separate" : L"stacked") +
        L" --adaptive-widescreen " +
        (adaptive ? L"top" : L"none") +
        L" --supersampling " + std::to_wstring(supersampling) +
        L" --antialiasing " + std::to_wstring(antialiasing) +
        L" --startup-mode automatic";

    wchar_t old_path[32768]{};
    GetEnvironmentVariableW(L"PATH", old_path,
                            static_cast<DWORD>(std::size(old_path)));
    const std::wstring path =
        L"C:\\msys64\\mingw64\\bin;" + std::wstring(old_path);
    SetEnvironmentVariableW(L"PATH", path.c_str());
    static constexpr const wchar_t* kRendererPolicies[] = {
        L"auto", L"soft", L"compute",
    };
    const int renderer_index =
        renderer >= 0 && renderer < static_cast<int>(
            std::size(kRendererPolicies)) ? renderer : 0;
    const DWORD old_renderer_size =
        GetEnvironmentVariableW(L"NDS_3D_RENDERER", nullptr, 0);
    std::wstring old_renderer;
    if (old_renderer_size != 0) {
        old_renderer.resize(old_renderer_size);
        GetEnvironmentVariableW(L"NDS_3D_RENDERER",
                                old_renderer.data(), old_renderer_size);
        old_renderer.resize(old_renderer_size - 1u);
    }
    SetEnvironmentVariableW(
        L"NDS_3D_RENDERER", kRendererPolicies[renderer_index]);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(
        runner.wstring().c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, source.wstring().c_str(),
        &startup, &process);
    SetEnvironmentVariableW(
        L"NDS_3D_RENDERER",
        old_renderer_size != 0 ? old_renderer.c_str() : nullptr);
    if (!ok) {
        std::fprintf(stderr, "CreateProcessW failed: %lu\n",
                     static_cast<unsigned long>(GetLastError()));
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    RecompLauncherCSettings settings{};
    settings.output_method = 2;
    settings.window_scale = 3;
    settings.enable_audio = 1;
    settings.audio_freq = 32768;
    settings.volume = 100;
    settings.player_src[0] = 2;
    settings.supersampling = 1;
    settings.antialiasing = 0;
    settings.display_layout = 1;
    settings.renderer = 0;

    static const char* const sha1[] = { kKnownSha1 };
    static const char* const display_layouts[] = {
        "Stacked window",
        "Separate windows",
    };
    static const char* const renderers[] = {
        "Automatic (OpenGL preferred)",
        "Software",
        "OpenGL 4.3",
    };

    const fs::path exe_dir =
        fs::absolute(argc > 0 ? argv[0] : "sm64ds_launcher.exe")
            .parent_path();
    ModState mod_state{};
    mod_state.state_path = exe_dir / "sm64ds_mods.ini";
    load_mod_state(mod_state);
    RecompLauncherCModProvider mod_provider = make_mod_provider(&mod_state);

    RecompLauncherCGameInfo game{};
    launcher_profile_apply("nds", &game);
    game.name = "Super Mario 64 DS";
    game.region = "Europe";
    game.known_sha1_hex = sha1;
    game.num_known_sha1 = std::size(sha1);
    game.lock_device = 0;
    game.hide_rebind = 0;
    game.display_layout_labels = display_layouts;
    game.num_display_layouts = std::size(display_layouts);
    game.has_renderer = 1;
    game.renderer_labels = renderers;
    game.num_renderers = std::size(renderers);
    game.mods = &mod_provider;

    const fs::path default_rom =
        fs::path(SM64DS_SOURCE_DIR) / "Super Mario 64 DS.nds";
    char selected_rom[1024]{};
    const int result = recomp_launcher_run_window(
        "Super Mario 64 DS - Launcher", &settings, &game,
        exe_dir.string().c_str(), default_rom.string().c_str(),
        selected_rom, sizeof(selected_rom));
    if (result == 1) return 0;
    if (result == 2) {
        std::fprintf(stderr, "recomp-ui unavailable\n");
        return 2;
    }
    return launch_runner(selected_rom, settings.display_layout,
                         mod_state.adaptive_widescreen,
                         settings.renderer,
                         settings.supersampling,
                         settings.antialiasing)
        ? 0 : 3;
}
