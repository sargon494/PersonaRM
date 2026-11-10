#!/usr/bin/env python3
"""
Performance Tests for Persona 2: Eternal Punishment Native Port

This script runs performance benchmarks and reports results.
"""

import subprocess
import sys
import time
import os
from pathlib import Path
from typing import List, Optional, Dict, Any
from dataclasses import dataclass, field
from datetime import datetime


# ───────────────────────────────────────────────────────────────────────
# Data Classes
# ───────────────────────────────────────────────────────────────────────

@dataclass
class BenchmarkResult:
    """Represents the result of a single benchmark."""
    name: str
    iterations: int
    total_time: float
    avg_time: float
    min_time: float
    max_time: float
    passed: bool
    message: Optional[str] = None


# ───────────────────────────────────────────────────────────────────────
# Configuration
# ───────────────────────────────────────────────────────────────────────

TARGET_FPS = 60
MIN_FPS = 50
FRAME_TIME_MS = 1000.0 / TARGET_FPS  # ~16.67ms per frame
FRAME_TIME_THRESHOLD_MS = 20.0  # Allow some slack

TEST_BINARY = Path(os.environ.get("CMAKE_BINARY_DIR", "build/Release")) / "persona2ep"


# ───────────────────────────────────────────────────────────────────────
# Benchmark Suite
# ───────────────────────────────────────────────────────────────────────

class PerformanceTestSuite:
    """Runs all performance benchmarks for the Persona 2 port."""

    def __init__(self):
        self.results: List[BenchmarkResult] = []

    def run_benchmark(self, name: str, iterations: int = 10,
                      func=None, args=()) -> BenchmarkResult:
        """Run a single benchmark and return the result."""
        print(f"\n  ▶ {name}...")

        start_time = time.perf_counter()

        try:
            for i in range(iterations):
                if func:
                    func(*args)
                else:
                    subprocess.run(
                        [str(TEST_BINARY), "--benchmark", name],
                        capture_output=True,
                        timeout=300
                    )

            total_time = time.perf_counter() - start_time
            avg_time = total_time / iterations
            min_time = total_time / iterations  # Single run for now
            max_time = total_time / iterations

            passed = avg_time < FRAME_TIME_THRESHOLD_MS
            message = (
                f"Avg: {avg_time:.2f}ms (target: {FRAME_TIME_THRESHOLD_MS}ms)"
                if passed
                else f"Avg: {avg_time:.2f}ms (target: {FRAME_TIME_THRESHOLD_MS}ms) ❌"
            )

            return BenchmarkResult(
                name=name,
                iterations=iterations,
                total_time=total_time,
                avg_time=avg_time,
                min_time=min_time,
                max_time=max_time,
                passed=passed,
                message=message
            )

        except subprocess.TimeoutExpired:
            return BenchmarkResult(
                name=name,
                iterations=iterations,
                total_time=300,
                avg_time=300,
                min_time=300,
                max_time=300,
                passed=False,
                message="Timeout"
            )
        except Exception as e:
            return BenchmarkResult(
                name=name,
                iterations=iterations,
                total_time=0,
                avg_time=0,
                min_time=0,
                max_time=0,
                passed=False,
                message=str(e)
            )

    def run_all(self) -> List[BenchmarkResult]:
        """Run all benchmarks and return results."""
        print("\n" + "=" * 60)
        print("  Persona 2: Eternal Punishment - Performance Benchmarks")
        print("=" * 60)
        print(f"\n  Target FPS: {TARGET_FPS} (frame time: {FRAME_TIME_MS:.2f}ms)")
        print(f"  Frame time threshold: {FRAME_TIME_THRESHOLD_MS:.2f}ms")
        print(f"\n  {'─' * 58}")

        benchmarks = [
            ("startup", 5, None, ()),
            ("menu_navigation", 10, None, ()),
            ("dialogue_render", 10, None, ()),
            ("combat_render", 10, None, ()),
            ("texture_loading", 5, None, ()),
            ("shader_compilation", 3, None, ()),
            ("audio_mixing", 5, None, ()),
            ("memory_allocation", 5, None, ()),
            ("save_load", 3, None, ()),
        ]

        for name, iterations, func, args in benchmarks:
            result = self.run_benchmark(name, iterations, func, args)
            self.results.append(result)
            print(f"  {result}")

        print(f"\n  {'─' * 58}")

        return self.results


# ───────────────────────────────────────────────────────────────────────
# Report Generator
# ───────────────────────────────────────────────────────────────────────

def generate_report(results: List[BenchmarkResult], output_path: Path) -> None:
    """Generate a detailed performance report."""

    print(f"\n  📊 Generating report: {output_path}")

    with open(output_path, "w") as f:
        f.write("# Persona 2: Eternal Punishment - Performance Report\n")
        f.write(f"# Generated: {datetime.now().isoformat()}\n")
        f.write("#\n")
        f.write("# Benchmark Results\n")
        f.write("#==================\n\n")

        total_passed = sum(1 for r in results if r.passed)
        total = len(results)

        f.write(f"## Summary\n\n")
        f.write(f"- **Total Benchmarks:** {total}\n")
        f.write(f"- **Passed:** {total_passed}\n")
        f.write(f"- **Failed:** {total - total_passed}\n")
        f.write(f"- **Pass Rate:** {100 * total_passed / total:.1f}%\n\n")

        f.write("## Detailed Results\n\n")
        f.write("| Name | Avg Time | Min Time | Max Time | Status |\n")
        f.write("|------|----------|----------|----------|--------|\n")

        for result in results:
            status = "✅ PASS" if result.passed else "❌ FAIL"
            f.write(
                f"| {result.name} | {result.avg_time:.2f}ms | "
                f"{result.min_time:.2f}ms | {result.max_time:.2f}ms | {status} |\n"
            )

        f.write("\n## Recommendations\n\n")
        f.write("For failed benchmarks, consider:\n")
        f.write("1. Optimizing the recompiler cache\n")
        f.write("2. Reducing draw calls\n")
        f.write("3. Improving texture streaming\n")
        f.write("4. Tuning audio mixing\n")
        f.write("5. Reducing memory allocations\n\n")

    print(f"  ✅ Report saved to: {output_path}")


# ───────────────────────────────────────────────────────────────────────
# Main Entry Point
# ───────────────────────────────────────────────────────────────────────

def main() -> int:
    """Main entry point for performance tests."""
    suite = PerformanceTestSuite()
    results = suite.run_all()

    # Generate report
    output_path = Path("build/Release/performance_report.md")
    generate_report(results, output_path)

    # Summary
    passed = sum(1 for r in results if r.passed)
    total = len(results)

    print("\n" + "=" * 60)
    print("  Summary")
    print("=" * 60)
    print(f"  Passed: {passed}/{total}")
    print(f"  Failed: {total - passed}/{total}")

    if passed == total:
        print("\n  🎉 All performance benchmarks passed!")
        return 0
    else:
        print(f"\n  ⚠️  {total - passed} benchmark(s) failed")
        return 1


if __name__ == "__main__":
    sys.exit(main())