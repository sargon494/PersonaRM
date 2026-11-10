#pragma once

#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include "launcher/launcher.h"
#include "loader/disc_reader.h"

namespace personarm {

// ───────────────────────────────────────────────────────────────────────────
// Disc Slot Structure (Main Game vs. Preview / Swap Disc)
// ───────────────────────────────────────────────────────────────────────────

struct DiscSlot {
    int disc_number = 1; // 1 (Main Game) or 2 (Bonus Disc / Preview Disc)
    std::string path;
    bool verified = false;
    Persona2DiscInfo info;
    std::string volume_label;
    size_t file_count = 0;
    std::string boot_exe;
};

// ───────────────────────────────────────────────────────────────────────────
// PersonaRM Game Portal & BYOC Launcher Hub
// ───────────────────────────────────────────────────────────────────────────

class Portal {
public:
    Portal();
    ~Portal() = default;

    bool Initialize(const std::string& config_file = "personarm_config.json");

    // Disc Slot Management (Disc 1 & Disc 2)
    bool LoadDisc(int disc_number, const std::string& disc_path);
    const DiscSlot& GetDiscSlot(int disc_number) const;
    int GetActiveDiscNumber() const { return m_active_disc; }
    bool SetActiveDiscNumber(int disc_number);
    const DiscSlot& GetActiveDiscSlot() const;

    // Disc Discovery & Verification
    std::vector<std::string> AutoScanDiscs(const std::vector<std::string>& search_paths = {".", "roms", "discs", "docs/research"});
    static DiscSlot InspectDisc(const std::string& path, int expected_disc_number = 0);

    // Configuration Access
    LauncherConfig& GetConfig() { return m_config; }
    const LauncherConfig& GetConfig() const { return m_config; }
    void SaveConfig();

    // Portal UI & Menus
    void DisplayBanner(std::ostream& os = std::cout) const;
    void DisplayStatus(std::ostream& os = std::cout) const;
    void RunInteractiveMenu(std::istream& is = std::cin, std::ostream& os = std::cout);

    // Engine Booting & Runtime Disc Swapping
    int LaunchGame();
    bool SwapDisc(int target_disc);

private:
    DiscSlot m_disc1;
    DiscSlot m_disc2;
    int m_active_disc = 1;
    LauncherConfig m_config;
    std::string m_config_path = "personarm_config.json";
};

} // namespace personarm
