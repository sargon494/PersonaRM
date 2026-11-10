#!/usr/bin/env python3
"""
Integration Tests for Persona 2: Eternal Punishment Native Port (PersonaRM)

This script runs integration tests that verify the full system works
correctly with native binaries and test flags.
"""

import subprocess
import sys
import time
import os
from pathlib import Path
from typing import List, Optional

# Force UTF-8 encoding on standard output/err where supported
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# ───────────────────────────────────────────────────────────────────────
# Test Configuration
# ───────────────────────────────────────────────────────────────────────

TEST_TIMEOUT = 120  # seconds
BUILD_DIR = Path(os.environ.get("CMAKE_BINARY_DIR", "build/Release"))
TEST_BINARY = BUILD_DIR / ("personarm.exe" if sys.platform == "win32" and (BUILD_DIR / "personarm.exe").exists() else "personarm")

# Ensure executable permissions on POSIX systems (artifacts lose +x during zip upload)
if sys.platform != "win32" and TEST_BINARY.exists():
    try:
        import stat
        current_stat = os.stat(TEST_BINARY)
        os.chmod(TEST_BINARY, current_stat.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except Exception as e:
        print(f"[WARN] could not set executable permissions on {TEST_BINARY}: {e}")

# ───────────────────────────────────────────────────────────────────────
# Test Results
# ───────────────────────────────────────────────────────────────────────

class TestResult:
    """Represents the result of a single test."""

    def __init__(self, name: str, passed: bool, duration: float,
                 message: Optional[str] = None):
        self.name = name
        self.passed = passed
        self.duration = duration
        self.message = message

    def __repr__(self) -> str:
        status = "PASS" if self.passed else "FAIL"
        return f"[{status}] {self.name} ({self.duration:.2f}s) {self.message}"

# ───────────────────────────────────────────────────────────────────────
# Test Suite
# ───────────────────────────────────────────────────────────────────────

class IntegrationTestSuite:
    """Runs all integration tests for the Persona 2 port."""

    def __init__(self):
        self.results: List[TestResult] = []
        self.start_time: float = 0

    def run_all(self) -> List[TestResult]:
        """Run all integration tests and return results."""
        self.start_time = time.time()

        tests = [
            self.test_startup,
            self.test_renderer,
            self.test_input,
            self.test_audio,
            self.test_memory,
            self.test_tim_parser,
            self.test_vag_decoder,
            self.test_gpu,
            self.test_bios,
            self.test_gte,
            self.test_exe,
            self.test_spu,
            self.test_str,
            self.test_engine,
            self.test_window,
            self.test_mdec,
            self.test_game_loading,
            self.test_save_system,
            self.test_performance,
        ]

        for test_func in tests:
            result = test_func()
            self.results.append(result)

        return self.results

    def _run_subcommand(self, name: str, arg: str, check_fn=None) -> TestResult:
        t0 = time.time()
        try:
            result = subprocess.run(
                [str(TEST_BINARY), arg],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )
            duration = time.time() - t0
            passed = (result.returncode == 0)
            if passed and check_fn:
                passed = check_fn(result.stdout)

            msg = "OK" if passed else f"Exit code {result.returncode}: {result.stderr or result.stdout}"
            return TestResult(name=name, passed=passed, duration=duration, message=msg)
        except subprocess.TimeoutExpired:
            return TestResult(name=name, passed=False, duration=time.time() - t0, message="Timeout")
        except Exception as e:
            return TestResult(name=name, passed=False, duration=time.time() - t0, message=str(e))

    def test_startup(self) -> TestResult:
        print("\n[INFO] Testing startup...")
        return self._run_subcommand("startup", "--version", lambda out: "Persona 2" in out)

    def test_renderer(self) -> TestResult:
        print("\n[INFO] Testing renderer...")
        return self._run_subcommand("renderer", "--test-renderer")

    def test_input(self) -> TestResult:
        print("\n[INFO] Testing input...")
        return self._run_subcommand("input", "--test-input")

    def test_audio(self) -> TestResult:
        print("\n[INFO] Testing audio...")
        return self._run_subcommand("audio", "--test-audio")

    def test_memory(self) -> TestResult:
        print("\n[INFO] Testing memory manager...")
        return self._run_subcommand("memory", "--test-memory")

    def test_tim_parser(self) -> TestResult:
        print("\n[INFO] Testing TIM texture parser...")
        return self._run_subcommand("tim_parser", "--test-tim")

    def test_vag_decoder(self) -> TestResult:
        print("\n[INFO] Testing VAG audio decoder...")
        return self._run_subcommand("vag_decoder", "--test-vag")

    def test_gpu(self) -> TestResult:
        print("\n[INFO] Testing PS1 GPU & VRAM...")
        return self._run_subcommand("gpu", "--test-gpu")

    def test_bios(self) -> TestResult:
        print("\n[INFO] Testing PS1 BIOS HLE Dispatcher...")
        return self._run_subcommand("bios", "--test-bios")

    def test_gte(self) -> TestResult:
        print("\n[INFO] Testing PS1 GTE Geometry Transformation Engine...")
        return self._run_subcommand("gte", "--test-gte")

    def test_exe(self) -> TestResult:
        print("\n[INFO] Testing PS-X EXE Loader & SYSTEM.CNF parser...")
        return self._run_subcommand("exe", "--test-exe")

    def test_spu(self) -> TestResult:
        print("\n[INFO] Testing PS1 SPU 24-Voice Audio Emulator...")
        return self._run_subcommand("spu", "--test-spu")

    def test_str(self) -> TestResult:
        print("\n[INFO] Testing PS1 STR Video Demuxer...")
        return self._run_subcommand("str", "--test-str")

    def test_engine(self) -> TestResult:
        print("\n[INFO] Testing PersonaRM Core Engine & Frame Loop...")
        return self._run_subcommand("engine", "--test-engine")

    def test_window(self) -> TestResult:
        print("\n[INFO] Testing Window Context & GL Presentation pipeline...")
        return self._run_subcommand("window", "--test-window")

    def test_mdec(self) -> TestResult:
        print("\n[INFO] Testing PS1 MDEC Video Macroblock Decoder...")
        return self._run_subcommand("mdec", "--test-mdec")

    def test_game_loading(self) -> TestResult:
        print("\n[INFO] Testing disc/game loader...")
        return self._run_subcommand("game_loading", "--test-load")

    def test_save_system(self) -> TestResult:
        print("\n[INFO] Testing save system...")
        return self._run_subcommand("save_system", "--test-save")

    def test_performance(self) -> TestResult:
        print("\n[INFO] Testing performance benchmarks...")
        return self._run_subcommand("performance", "--test-performance")

# ───────────────────────────────────────────────────────────────────────
# Main Entry Point
# ───────────────────────────────────────────────────────────────────────

def main() -> int:
    """Main entry point for the integration test suite."""
    suite = IntegrationTestSuite()

    print("=" * 60)
    print("  Persona 2: Eternal Punishment - Integration Tests")
    print("=" * 60)

    results = suite.run_all()

    # Print results
    print("\n" + "=" * 60)
    print("  Test Results")
    print("=" * 60)

    for result in results:
        print(f"  {result}")

    # Summary
    passed = sum(1 for r in results if r.passed)
    total = len(results)
    print(f"\n  Passed: {passed}/{total}")
    print(f"  Failed: {total - passed}/{total}")

    if passed == total:
        print("\n  [PASS] All integration tests passed successfully!")
        return 0
    else:
        print(f"\n  [FAIL] {total - passed} test(s) failed")
        return 1

if __name__ == "__main__":
    sys.exit(main())
