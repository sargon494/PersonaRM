/**
 * @file main.cpp
 * @brief Entry point for Persona 2: Eternal Punishment Native Port
 *
 * This is the main entry point for the native port. It handles:
 * - Command-line argument parsing
 * - System initialization
 * - Main loop execution
 * - Graceful shutdown
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

// Project headers
#include "core/memory_manager.h"

// ───────────────────────────────────────────────────────────────────────
// Version Information
// ───────────────────────────────────────────────────────────────────────

static constexpr const char* PROJECT_NAME    = "Persona 2: Eternal Punishment - Native Port";
static constexpr const char* PROJECT_VERSION = "0.1.0";
static constexpr const char* PROJECT_LICENSE = "GPL v3.0 or later";

// ───────────────────────────────────────────────────────────────────────
// Command-line Helpers
// ───────────────────────────────────────────────────────────────────────

static void PrintVersion() {
    std::cout << PROJECT_NAME << "\n"
              << "Version: " << PROJECT_VERSION << "\n"
              << "License: " << PROJECT_LICENSE << "\n"
              << "Build: " << __DATE__ << " " << __TIME__ << "\n";
}

static void PrintUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  --version          Show version information\n"
              << "  --help             Show this help message\n"
              << "  --test-memory      Run memory manager self-test\n"
              << "  --test-renderer    Run renderer self-test\n"
              << "  --test-input       Run input self-test\n"
              << "  --test-audio       Run audio self-test\n"
              << "  --test-load        Run game loader self-test\n"
              << "  --test-save        Run save system self-test\n"
              << "  --test-performance Run performance benchmarks\n"
              << "\n"
              << "Note: You must provide your own legal copy of the game.\n";
}

// ───────────────────────────────────────────────────────────────────────
// Self-Test Stubs
// ───────────────────────────────────────────────────────────────────────

static int RunMemoryTest() {
    std::cout << "[TEST] Memory Manager self-test...\n";

    try {
        persona2ep::MemoryManager mgr;
        mgr.Initialize(1024 * 1024);  // 1MB

        std::cout << "  Regions: " << mgr.NumRegions() << "\n";
        std::cout << "  Total available: " << mgr.TotalAvailable() << " bytes\n";

        mgr.Shutdown();
        std::cout << "[PASS] Memory Manager self-test passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Memory Manager self-test failed: " << e.what() << "\n";
        return 1;
    }
}

static int RunStubTest(const char* component_name) {
    std::cout << "[TEST] " << component_name << " self-test...\n";
    std::cout << "[SKIP] " << component_name << " not yet implemented.\n";
    return 0;
}

// ───────────────────────────────────────────────────────────────────────
// Main Entry Point
// ───────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Handle command-line arguments
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--version") == 0) {
                PrintVersion();
                return EXIT_SUCCESS;
            }
            if (std::strcmp(argv[i], "--help") == 0) {
                PrintUsage(argv[0]);
                return EXIT_SUCCESS;
            }
            if (std::strcmp(argv[i], "--test-memory") == 0) {
                return RunMemoryTest();
            }
            if (std::strcmp(argv[i], "--test-renderer") == 0) {
                return RunStubTest("Renderer");
            }
            if (std::strcmp(argv[i], "--test-input") == 0) {
                return RunStubTest("Input");
            }
            if (std::strcmp(argv[i], "--test-audio") == 0) {
                return RunStubTest("Audio");
            }
            if (std::strcmp(argv[i], "--test-load") == 0) {
                return RunStubTest("Game Loader");
            }
            if (std::strcmp(argv[i], "--test-save") == 0) {
                return RunStubTest("Save System");
            }
            if (std::strcmp(argv[i], "--test-performance") == 0) {
                return RunStubTest("Performance");
            }

            std::cerr << "Unknown option: " << argv[i] << "\n";
            PrintUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    // Default: show version and status
    PrintVersion();
    std::cout << "\n";
    std::cout << "Status: Project is in early development.\n";
    std::cout << "Run with --help for available options.\n";

    return EXIT_SUCCESS;
}

