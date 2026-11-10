#include "portal.h"
#include "engine/engine.h"
#include <iostream>
#include <filesystem>
#include <iomanip>

namespace personarm {

namespace fs = std::filesystem;

Portal::Portal() {
    m_disc1.disc_number = 1;
    m_disc2.disc_number = 2;
    m_active_disc = 1;
}

bool Portal::Initialize(const std::string& config_file) {
    m_config_path = config_file;
    Launcher launcher;
    launcher.LoadConfig(config_file);
    m_config = launcher.GetConfig();

    if (!m_config.game.disc1_path.empty()) {
        LoadDisc(1, m_config.game.disc1_path);
    } else if (!m_config.game.legal_rom_path.empty()) {
        LoadDisc(1, m_config.game.legal_rom_path);
    }

    if (!m_config.game.disc2_path.empty()) {
        LoadDisc(2, m_config.game.disc2_path);
    }

    if (m_config.game.active_disc == 1 || m_config.game.active_disc == 2) {
        m_active_disc = m_config.game.active_disc;
    } else {
        m_active_disc = 1;
    }

    return true;
}

DiscSlot Portal::InspectDisc(const std::string& path, int expected_disc_number) {
    DiscSlot slot;
    slot.disc_number = expected_disc_number > 0 ? expected_disc_number : 1;
    slot.path = path;
    slot.verified = false;

    if (!fs::exists(path)) {
        return slot;
    }

    DiscReader reader;
    if (reader.Open(path)) {
        std::string boot_exe = reader.DetectBootExecutable();
        slot.boot_exe = boot_exe;
        slot.volume_label = reader.GetVolumeLabel();
        slot.file_count = reader.GetFileCount();

        if (!boot_exe.empty()) {
            slot.info = DiscReader::IdentifyPersona2(boot_exe);
            if (slot.info.recognized) {
                slot.verified = true;
                if (slot.info.disc_number > 0) {
                    slot.disc_number = slot.info.disc_number;
                }
                return slot;
            }
        }

        // Even if SYSTEM.CNF boot wasn't specifically P2, if valid ISO, mark verified
        if (slot.file_count > 0) {
            slot.verified = true;
            return slot;
        }
    }

    // Fallback: check file size > 50 MB
    try {
        uintmax_t sz = fs::file_size(path);
        if (sz > 50 * 1024 * 1024) {
            slot.verified = true;
            slot.volume_label = "RAW_PS1_IMAGE";
        }
    } catch (...) {
    }

    return slot;
}

bool Portal::LoadDisc(int disc_number, const std::string& disc_path) {
    if (disc_number != 1 && disc_number != 2) {
        return false;
    }

    DiscSlot slot = InspectDisc(disc_path, disc_number);
    slot.disc_number = disc_number;

    if (disc_number == 1) {
        m_disc1 = slot;
        m_config.game.disc1_path = disc_path;
        m_config.game.legal_rom_path = disc_path;
        m_config.game.is_verified = slot.verified;
    } else {
        m_disc2 = slot;
        m_config.game.disc2_path = disc_path;
    }

    SaveConfig();
    return slot.verified;
}

const DiscSlot& Portal::GetDiscSlot(int disc_number) const {
    return disc_number == 2 ? m_disc2 : m_disc1;
}

bool Portal::SetActiveDiscNumber(int disc_number) {
    if (disc_number != 1 && disc_number != 2) {
        return false;
    }
    m_active_disc = disc_number;
    m_config.game.active_disc = disc_number;
    SaveConfig();
    return true;
}

const DiscSlot& Portal::GetActiveDiscSlot() const {
    return GetDiscSlot(m_active_disc);
}

std::vector<std::string> Portal::AutoScanDiscs(const std::vector<std::string>& search_paths) {
    std::vector<std::string> discovered;

    for (const auto& dir : search_paths) {
        try {
            if (!fs::exists(dir)) continue;

            for (const auto& entry : fs::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;

                std::string ext = entry.path().extension().string();
                for (auto& c : ext) c = static_cast<char>(std::tolower(c));

                if (ext == ".cue" || ext == ".bin" || ext == ".iso" || ext == ".img") {
                    std::string p = entry.path().string();
                    DiscSlot inspected = InspectDisc(p);

                    if (inspected.verified) {
                        discovered.push_back(p);
                        if (inspected.info.recognized) {
                            if (inspected.info.disc_number == 2 && m_disc2.path.empty()) {
                                LoadDisc(2, p);
                            } else if (inspected.info.disc_number == 1 && m_disc1.path.empty()) {
                                LoadDisc(1, p);
                            }
                        }
                    }
                }
            }
        } catch (...) {
        }
    }

    return discovered;
}

void Portal::SaveConfig() {
    Launcher launcher;
    launcher.GetConfig() = m_config;
    launcher.SaveConfig(m_config_path);
}

void Portal::DisplayBanner(std::ostream& os) const {
    os << "\n=======================================================================\n";
    os << "       🎮 PersonaRM — Portal & Launcher Hub for Persona 2 EP          \n";
    os << "              Native PC Runtime • Bring Your Own Copy (BYOC)          \n";
    os << "=======================================================================\n";
}

void Portal::DisplayStatus(std::ostream& os) const {
    DisplayBanner(os);

    os << "\n[DISC SLOTS]\n";
    // Disc 1
    os << "  Disc 1 (Main Story) : ";
    if (m_disc1.verified) {
        os << "[VERIFIED] " << m_disc1.path << "\n";
        if (m_disc1.info.recognized) {
            os << "    ID: " << m_disc1.info.product_id << " (" << m_disc1.info.region
               << ", " << m_disc1.info.edition << ", Boot: " << m_disc1.boot_exe << ")\n";
        }
    } else {
        os << (m_disc1.path.empty() ? "[NOT SET]" : "[UNVERIFIED] " + m_disc1.path) << "\n";
    }

    // Disc 2
    os << "  Disc 2 (Bonus/Swap) : ";
    if (m_disc2.verified) {
        os << "[VERIFIED] " << m_disc2.path << "\n";
        if (m_disc2.info.recognized) {
            os << "    ID: " << m_disc2.info.product_id << " (" << m_disc2.info.region
               << ", " << m_disc2.info.edition << ", Boot: " << m_disc2.boot_exe << ")\n";
        }
    } else {
        os << (m_disc2.path.empty() ? "[NOT SET]" : "[UNVERIFIED] " + m_disc2.path) << "\n";
    }

    os << "  Active Disc Target  : Disc " << m_active_disc << "\n";

    os << "\n[SETTINGS & ENHANCEMENTS]\n";
    os << "  Resolution          : " << m_config.video.width << "x" << m_config.video.height
       << " @ " << m_config.video.target_fps << " FPS (VSync: " << (m_config.video.vsync ? "ON" : "OFF") << ")\n";
    os << "  Widescreen Viewport : " << (m_config.video.enable_widescreen_patch ? "16:9 Active" : "Original 4:3") << "\n";
    os << "  Turbo Mode          : " << m_config.input.turbo_multiplier << "x Fast-Forward\n";
    os << "  Audio Volume        : Master " << static_cast<int>(m_config.audio.master_volume * 100) << "%, BGM "
       << static_cast<int>(m_config.audio.bgm_volume * 100) << "%\n";
    os << "-----------------------------------------------------------------------\n";
}

void Portal::RunInteractiveMenu(std::istream& is, std::ostream& os) {
    DisplayStatus(os);

    os << "Portal Actions:\n";
    os << "  1. Launch Game (Boot Active Disc)\n";
    os << "  2. Switch Active Disc (Current: Disc " << m_active_disc << ")\n";
    os << "  3. Auto-Scan Storage for Game Discs\n";
    os << "  4. Set Disc 1 Path\n";
    os << "  5. Set Disc 2 Path\n";
    os << "  6. Toggle Widescreen (16:9 / 4:3)\n";
    os << "  7. Set Turbo Multiplier\n";
    os << "  8. Exit Portal\n";
    os << "Select option (1-8): ";

    std::string choice;
    if (std::getline(is, choice)) {
        if (choice == "1") {
            LaunchGame();
        } else if (choice == "2") {
            SetActiveDiscNumber(m_active_disc == 1 ? 2 : 1);
            os << "Switched active disc to: Disc " << m_active_disc << "\n";
        } else if (choice == "3") {
            os << "Scanning local directories for game discs...\n";
            auto found = AutoScanDiscs();
            os << "Discovered " << found.size() << " valid disc image(s).\n";
        } else if (choice == "6") {
            m_config.video.enable_widescreen_patch = !m_config.video.enable_widescreen_patch;
            SaveConfig();
            os << "Widescreen viewport is now: "
               << (m_config.video.enable_widescreen_patch ? "16:9" : "4:3") << "\n";
        }
    }
}

int Portal::LaunchGame() {
    const DiscSlot& slot = GetActiveDiscSlot();

    std::cout << "\n[Portal] Preparing to launch Persona 2: Eternal Punishment (Disc " << m_active_disc << ")...\n";

    std::string target_path = slot.path;
    if (target_path.empty() || !slot.verified) {
        std::cout << "[Portal] No verified disc in slot " << m_active_disc << ". Running auto-scan...\n";
        auto found = AutoScanDiscs();
        if (!found.empty()) {
            target_path = GetActiveDiscSlot().path;
        }
    }

    if (target_path.empty() || !fs::exists(target_path)) {
        std::cout << "\n=======================================================================\n";
        std::cout << "  [BYOC NOTICE] Legitimate game disc image not found.                 \n";
        std::cout << "  Please provide your Persona 2: Eternal Punishment copy:              \n";
        std::cout << "    - Persona 2 - Batsu - Eternal Punishment Disc 1 (.cue/.bin/.iso)  \n";
        std::cout << "    - Persona 2 - Batsu - Eternal Punishment Disc 2 (Deluxe Pack)     \n";
        std::cout << "  Place the disc image in './discs/' or use '--disc1 <path>'.         \n";
        std::cout << "=======================================================================\n";
        return 1;
    }

    auto& engine = Engine::Instance();
    std::cout << "[Portal] Mounting disc image: " << target_path << "\n";

    if (!engine.BootFromDisc(target_path)) {
        std::cerr << "[Portal] Engine failed to boot disc: " << target_path << "\n";
        return 1;
    }

    std::cout << "[Portal] Game booted successfully into PersonaRM Engine!\n";
    std::cout << "[Portal] CPU Entry Point initialized. Engine state: RUNNING.\n";

    // Run sample initial hardware frames
    for (int f = 0; f < 3; ++f) {
        engine.RunFrame();
    }
    std::cout << "[Portal] Executed initial hardware frames successfully.\n";

    return 0;
}

bool Portal::SwapDisc(int target_disc) {
    if (target_disc != 1 && target_disc != 2) {
        return false;
    }

    const DiscSlot& target_slot = GetDiscSlot(target_disc);
    if (!target_slot.verified || target_slot.path.empty()) {
        std::cerr << "[Portal] Cannot swap to unverified or empty Disc " << target_disc << "\n";
        return false;
    }

    m_active_disc = target_disc;
    m_config.game.active_disc = target_disc;
    SaveConfig();

    std::cout << "[Portal] Swapped disc to Disc " << target_disc << " (" << target_slot.path << ")\n";
    return true;
}

} // namespace personarm

