#!/usr/bin/env python3
"""
Integration Tests for Persona 2: Eternal Punishment Native Port

This script runs integration tests that verify the full system works
correctly with actual game data.
"""

import subprocess
import sys
import time
import os
from pathlib import Path
from typing import List, Optional, Dict, Any


# ───────────────────────────────────────────────────────────────────────
# Test Configuration
# ───────────────────────────────────────────────────────────────────────

TEST_TIMEOUT = 300  # seconds
BUILD_DIR = Path(os.environ.get("CMAKE_BINARY_DIR", "build/Release"))
TEST_BINARY = BUILD_DIR / ("personarm.exe" if sys.platform == "win32" and (BUILD_DIR / "personarm.exe").exists() else ("personarm" if (BUILD_DIR / "personarm").exists() else ("persona2ep.exe" if sys.platform == "win32" else "persona2ep")))

# Ensure executable permissions on POSIX systems (artifacts lose +x during zip upload)
if sys.platform != "win32" and TEST_BINARY.exists():
    try:
        import stat
        current_stat = os.stat(TEST_BINARY)
        os.chmod(TEST_BINARY, current_stat.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except Exception as e:
        print(f"Warning: could not set executable permissions on {TEST_BINARY}: {e}")



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
        status = "✓ PASS" if self.passed else "✗ FAIL"
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
            self.test_game_loading,
            self.test_save_system,
            self.test_performance,
        ]

        for test_func in tests:
            result = test_func()
            self.results.append(result)

        return self.results

    def test_startup(self) -> TestResult:
        """Test that the application starts correctly."""
        print("\n🚀 Testing startup...")

        try:
            result = subprocess.run(
                [str(TEST_BINARY), "--version"],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )

            if result.returncode == 0 and "Persona 2" in result.stdout:
                duration = time.time() - self.start_time
                return TestResult(
                    name="startup",
                    passed=True,
                    duration=duration,
                    message=f"Version string found: {result.stdout.strip()}"
                )
            else:
                return TestResult(
                    name="startup",
                    passed=False,
                    duration=duration,
                    message=f"Unexpected output: {result.stdout}"
                )
        except subprocess.TimeoutExpired:
            return TestResult(
                name="startup",
                passed=False,
                duration=TEST_TIMEOUT,
                message="Timeout during startup test"
            )
        except FileNotFoundError:
            return TestResult(
                name="startup",
                passed=False,
                duration=0,
                message="Binary not found"
            )

    def test_renderer(self) -> TestResult:
        """Test the renderer component."""
        print("\n🎨 Testing renderer...")

        try:
            # Test shader compilation
            result = subprocess.run(
                [str(TEST_BINARY), "--test-renderer"],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )

            if result.returncode == 0:
                duration = time.time() - self.start_time
                return TestResult(
                    name="renderer",
                    passed=True,
                    duration=duration,
                    message="Renderer tests passed"
                )
            else:
                return TestResult(
                    name="renderer",
                    passed=False,
                    duration=duration,
                    message=f"Renderer tests failed: {result.stderr}"
                )
        except subprocess.TimeoutExpired:
            return TestResult(
                name="renderer",
                passed=False,
                duration=TEST_TIMEOUT,
                message="Timeout during renderer test"
            )

    def test_input(self) -> TestResult:
        """Test the input system."""
        print("\n⌨️  Testing input...")

        try:
            result = subprocess.run(
                [str(TEST_BINARY), "--test-input"],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )

            if result.returncode == 0:
                duration = time.time() - self.start_time
                return TestResult(
                    name="input",
                    passed=True,
                    duration=duration,
                    message="Input tests passed"
                )
            else:
                return TestResult(
                    name="input",
                    passed=False,
                    duration=duration,
                    message=f"Input tests failed: {result.stderr}"
                )
        except subprocess.TimeoutExpired:
            return TestResult(
                name="input",
                    passed=False,
                    duration=TEST_TIMEOUT,
                    message="Timeout during input test"
                )

    def test_audio(self) -> TestResult:
        """Test the audio system."""
        print("\n🎵 Testing audio...")

        try:
            result = subprocess.run(
                [str(TEST_BINARY), "--test-audio"],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )

            if result.returncode == 0:
                duration = time.time() - self.start_time
                return TestResult(
                    name="audio",
                    passed=True,
                    duration=duration,
                    message="Audio tests passed"
                )
            else:
                return TestResult(
                    name="audio",
                    passed=False,
                    duration=duration,
                    message=f"Audio tests failed: {result.stderr}"
                )
        except subprocess.TimeoutExpired:
            return TestResult(
                name="audio",
                passed=False,
                duration=TEST_TIMEOUT,
                message="Timeout during audio test"
            )

    def test_memory(self) -> TestResult:
        """Test the memory manager."""
        print("\n💾 Testing memory manager...")

        try:
            result = subprocess.run(
                [str(TEST_BINARY), "--test-memory"],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )

            if result.returncode == 0:
                duration = time.time() - self.start_time
                return TestResult(
                    name="memory",
                    passed=True,
                    duration=duration,
                    message="Memory manager tests passed"
                )
            else:
                return TestResult(
                    name="memory",
                    passed=False,
                    duration=duration,
                    message=f"Memory tests failed: {result.stderr}"
                )
        except subprocess.TimeoutExpired:
            return TestResult(
                name="memory",
                passed=False,
                duration=TEST_TIMEOUT,
                message="Timeout during memory test"
            )

    def test_tim_parser(self) -> TestResult:
        """Test the TIM texture parser."""
        print("\n🖼️ Testing TIM texture parser...")

        try:
            result = subprocess.run(
                [str(TEST_BINARY), "--test-tim"],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )

            if result.returncode == 0:
                duration = time.time() - self.start_time
                return TestResult(
                    name="tim_parser",
                    passed=True,
                    duration=duration,
                    message="TIM texture parser tests passed"
                )
            else:
                return TestResult(
                    name="tim_parser",
                    passed=False,
                    duration=duration,
                    message=f"TIM texture parser tests failed: {result.stderr}"
                )
        except subprocess.TimeoutExpired:
            return TestResult(
                name="tim_parser",
                passed=False,
                duration=TEST_TIMEOUT,
                message="Timeout during TIM texture parser test"
            )

    def test_vag_decoder(self) -> TestResult:
        """Test the VAG audio decoder."""
        print("\n🔊 Testing VAG audio decoder...")

        try:
            result = subprocess.run(
                [str(TEST_BINARY), "--test-vag"],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )

            if result.returncode == 0:
                duration = time.time() - self.start_time
                return TestResult(
                    name="vag_decoder",
                    passed=True,
                    duration=duration,
                    message="VAG audio decoder tests passed"
                )
            else:
                return TestResult(
                    name="vag_decoder",
                    passed=False,
                    duration=duration,
                    message=f"VAG audio decoder tests failed: {result.stderr}"
                )
        except subprocess.TimeoutExpired:
            return TestResult(
                name="vag_decoder",
                passed=False,
                duration=TEST_TIMEOUT,
                message="Timeout during VAG audio decoder test"
            )

    def test_game_loading(self) -> TestResult:
        """Test game loading with user-provided binary."""
        print("\n📦 Testing game loading...")

        try:
            # This test requires a user-provided game binary
            # For now, we just verify the loader initializes correctly
            result = subprocess.run(
                [str(TEST_BINARY), "--test-load"],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )

            if result.returncode == 0:
                duration = time.time() - self.start_time
                return TestResult(
                    name="game_loading",
                    passed=True,
                    duration=duration,
                    message="Game loading test passed"
                )
            else:
                return TestResult(
                    name="game_loading",
                    passed=False,
                    duration=duration,
                    message=f"Game loading test failed: {result.stderr}"
                )
        except subprocess.TimeoutExpired:
            return TestResult(
                name="game_loading",
                passed=False,
                duration=TEST_TIMEOUT,
                message="Timeout during game loading test"
            )

    def test_save_system(self) -> TestResult:
        """Test the save system."""
        print("\n💾 Testing save system...")

        try:
            result = subprocess.run(
                [str(TEST_BINARY), "--test-save"],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )

            if result.returncode == 0:
                duration = time.time() - self.start_time
                return TestResult(
                    name="save_system",
                    passed=True,
                    duration=duration,
                    message="Save system tests passed"
                )
            else:
                return TestResult(
                    name="save_system",
                    passed=False,
                    duration=duration,
                    message=f"Save system tests failed: {result.stderr}"
                )
        except subprocess.TimeoutExpired:
            return TestResult(
                name="save_system",
                passed=False,
                duration=TEST_TIMEOUT,
                message="Timeout during save system test"
            )

    def test_performance(self) -> TestResult:
        """Test performance benchmarks."""
        print("\n⚡ Testing performance...")

        try:
            result = subprocess.run(
                [str(TEST_BINARY), "--test-performance"],
                capture_output=True,
                text=True,
                timeout=TEST_TIMEOUT
            )

            if result.returncode == 0:
                duration = time.time() - self.start_time
                return TestResult(
                    name="performance",
                    passed=True,
                    duration=duration,
                    message="Performance benchmarks passed"
                )
            else:
                return TestResult(
                    name="performance",
                    passed=False,
                    duration=duration,
                    message=f"Performance tests failed: {result.stderr}"
                )
        except subprocess.TimeoutExpired:
            return TestResult(
                name="performance",
                passed=False,
                duration=TEST_TIMEOUT,
                message="Timeout during performance test"
            )


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
        print("\n  🎉 All tests passed!")
        return 0
    else:
        print(f"\n  ❌ {total - passed} test(s) failed")
        return 1


if __name__ == "__main__":
    sys.exit(main())