#pragma once

#include "minissd/ftl.hpp"

#include <string>

namespace minissd {

struct ValidationReport {
    std::uint32_t seed = 0;
    std::size_t operations = 0;
    std::size_t writes = 0;
    std::size_t reads = 0;
    std::size_t trims = 0;
    std::size_t rejected = 0;
    std::size_t manual_gc_requests = 0;
    std::size_t logical_checks = 0;
    FtlMetrics metrics;
};

struct BenchmarkReport {
    std::string workload;
    std::uint32_t seed = 0;
    std::size_t measured_writes = 0;
    std::uint64_t nand_programs = 0;
    std::uint64_t gc_cycles = 0;
    std::uint64_t gc_moved_pages = 0;
    double elapsed_seconds = 0;
    double writes_per_second = 0;
    double throughput_mib_s = 0;
    double latency_p50_us = 0;
    double latency_p95_us = 0;
    double latency_p99_us = 0;
    double write_amplification = 0;
};

[[nodiscard]] ValidationReport run_validation(std::uint32_t seed, std::size_t operations);
[[nodiscard]] std::vector<BenchmarkReport> run_benchmarks(std::uint32_t seed, std::size_t writes);
[[nodiscard]] std::string validation_json(const ValidationReport& report);
[[nodiscard]] std::string benchmark_json(const std::vector<BenchmarkReport>& reports);
[[nodiscard]] std::string json_escape(std::string_view text);

}  // namespace minissd
