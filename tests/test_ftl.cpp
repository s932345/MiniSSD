#include "minissd/ftl.hpp"

#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace minissd;
using Bytes = std::vector<std::uint8_t>;
using LogicalData = std::vector<std::optional<Bytes>>;

void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

void expect(FtlStatus status, FtlError error = FtlError::None) {
    require(status.error == error,
        "expected " + std::string(describe(error)) + ", got " +
        std::string(describe(status.error)));
    require(status.nand_error == NandError::None, "unexpected NAND error");
}

Bytes read(const PageFtl& ftl, std::size_t lba) {
    Bytes result;
    expect(ftl.read(lba, result));
    return result;
}

Bytes raw_read(const PageFtl& ftl, PhysicalAddress address) {
    Bytes result;
    require(ftl.nand().read_page(address.block, address.page, result) == NandError::None,
            "raw diagnostic read failed");
    return result;
}

struct Snapshot {
    FtlStats stats;
    std::vector<std::optional<PhysicalAddress>> mappings;
    std::vector<PhysicalPageInfo> metadata;
    std::vector<Bytes> raw_pages;
    bool operator==(const Snapshot&) const = default;
};

Snapshot snapshot(const PageFtl& ftl) {
    Snapshot result;
    result.stats = ftl.stats();
    for (std::size_t lba = 0; lba < ftl.logical_page_count(); ++lba) {
        result.mappings.push_back(ftl.physical_address(lba));
    }
    const auto& geometry = ftl.nand().geometry();
    for (std::size_t block = 0; block < geometry.block_count; ++block) {
        for (std::size_t page = 0; page < geometry.pages_per_block; ++page) {
            const PhysicalAddress address{block, page};
            result.metadata.push_back(ftl.page_info(address).value());
            result.raw_pages.push_back(raw_read(ftl, address));
        }
    }
    return result;
}

void check_invariants(const PageFtl& ftl, const LogicalData& expected) {
    const auto& geometry = ftl.nand().geometry();
    std::vector<bool> mapped(ftl.physical_page_count(), false);
    std::size_t valid_count = 0;
    for (std::size_t lba = 0; lba < expected.size(); ++lba) {
        const auto address = ftl.physical_address(lba);
        if (expected[lba]) {
            require(read(ftl, lba) == *expected[lba], "logical data differs from oracle");
            require(address.has_value(), "written LBA lost its mapping");
            const auto index = address->block * geometry.pages_per_block + address->page;
            require(index < mapped.size() && !mapped[index], "two LBAs share one page");
            mapped[index] = true;
            const auto info = ftl.page_info(*address).value();
            require(info.state == PageState::Valid && info.lba == lba,
                    "mapping has no matching reverse owner");
            ++valid_count;
        } else {
            Bytes untouched{0x12, 0x34};
            expect(ftl.read(lba, untouched), FtlError::UnwrittenLba);
            require(untouched == Bytes({0x12, 0x34}), "unwritten read changed output");
            require(!address, "unwritten LBA has a mapping");
        }
    }
    FtlStats counted;
    for (std::size_t block = 0; block < geometry.block_count; ++block) {
        std::uint64_t erases = 999;
        require(ftl.nand().get_erase_count(block, erases) == NandError::None && erases == 0,
                "FTL unexpectedly erased a block");
        for (std::size_t page = 0; page < geometry.pages_per_block; ++page) {
            const auto info = ftl.page_info({block, page}).value();
            const auto index = block * geometry.pages_per_block + page;
            if (info.state == PageState::Free) {
                ++counted.free_pages;
                require(!info.lba && !mapped[index], "free page has a logical owner");
                require(raw_read(ftl, {block, page}) == Bytes(geometry.bytes_per_page, 0xFF),
                        "free page contains programmed data");
            } else if (info.state == PageState::Invalid) {
                ++counted.invalid_pages;
                require(!info.lba && !mapped[index], "invalid page remains mapped");
            } else {
                ++counted.valid_pages;
                require(info.lba && *info.lba < expected.size() && mapped[index],
                        "valid page has no live logical mapping");
            }
        }
    }
    require(counted == ftl.stats(), "page statistics differ from metadata");
    require(counted.valid_pages == valid_count, "wrong number of live pages");
    require(counted.free_pages + counted.valid_pages + counted.invalid_pages ==
            ftl.physical_page_count(), "page accounting does not cover the device");
}

void construction_and_initial_state() {
    PageFtl ftl({2, 4, 8}, 5, GcMode::Disabled);
    require(ftl.logical_page_count() == 5 && ftl.logical_capacity_bytes() == 40,
            "wrong logical capacity");
    require(ftl.physical_page_count() == 8, "wrong physical capacity");
    require(ftl.stats() == FtlStats{8, 0, 0}, "new FTL has used pages");
    check_invariants(ftl, LogicalData(5));
}

void default_capacity() {
    PageFtl ftl;
    require(ftl.logical_page_count() == 192, "wrong default logical pages");
    require(ftl.logical_capacity_bytes() == 768 * 1024, "wrong logical byte capacity");
    require(ftl.nand().capacity_bytes() == 1024 * 1024, "wrong NAND byte capacity");
}

void invalid_logical_capacity() {
    for (const std::size_t logical : {std::size_t{0}, std::size_t{4}, std::size_t{5}}) {
        bool caught = false;
        try { PageFtl ftl({1, 4, 8}, logical, GcMode::Disabled); }
        catch (const std::invalid_argument&) { caught = true; }
        require(caught, "invalid logical capacity accepted");
    }
}

void first_write_and_block_boundary() {
    PageFtl ftl({2, 2, 8}, 3, GcMode::Disabled);
    LogicalData expected(3);
    for (std::size_t lba = 0; lba < 3; ++lba) {
        expected[lba] = Bytes(8, static_cast<std::uint8_t>(lba + 1));
        expect(ftl.write(lba, *expected[lba]));
    }
    require(ftl.physical_address(0) == PhysicalAddress{0, 0}, "wrong first address");
    require(ftl.physical_address(2) == PhysicalAddress{1, 0}, "wrong block transition");
    check_invariants(ftl, expected);
}

void updates_preserve_neighbors_and_old_physical_bytes() {
    PageFtl ftl({2, 4, 8}, 3, GcMode::Disabled);
    expect(ftl.write(0, Bytes(8, 0xAA)));
    expect(ftl.write(1, Bytes(8, 0xCC)));
    const auto original_address = ftl.physical_address(0).value();
    const auto neighbor_address = ftl.physical_address(1).value();
    expect(ftl.write(0, Bytes(8, 0xBB)));
    require(ftl.physical_address(0) != original_address, "update reused the old page");
    require(ftl.physical_address(1) == neighbor_address, "neighbor mapping changed");
    require(raw_read(ftl, original_address) == Bytes(8, 0xAA), "old data physically erased");
    require(ftl.page_info(original_address) == PhysicalPageInfo{PageState::Invalid, std::nullopt},
            "old page not invalidated");
    require(ftl.stats() == FtlStats{5, 2, 1}, "wrong update accounting");
    check_invariants(ftl, {Bytes(8, 0xBB), Bytes(8, 0xCC), std::nullopt});
}

void identical_all_ff_updates_consume_new_pages() {
    PageFtl ftl({1, 4, 8}, 2, GcMode::Disabled);
    expect(ftl.write(0, Bytes(8, 0xFF)));
    const auto first = ftl.physical_address(0);
    expect(ftl.write(0, Bytes(8, 0xFF)));
    require(ftl.physical_address(0) != first, "all-FF update reused programmed page");
    require(ftl.stats() == FtlStats{2, 1, 1}, "all-FF accounting wrong");
    check_invariants(ftl, {Bytes(8, 0xFF), std::nullopt});
}

void invalid_operations_preserve_all_state() {
    PageFtl ftl({2, 3, 8}, 3, GcMode::Disabled);
    expect(ftl.write(0, Bytes(8, 0xAB)));
    const auto before = snapshot(ftl);
    for (const auto bad_lba : {std::size_t{3}, std::numeric_limits<std::size_t>::max()}) {
        expect(ftl.write(bad_lba, Bytes(8, 0)), FtlError::LbaOutOfRange);
        Bytes output{0x42};
        expect(ftl.read(bad_lba, output), FtlError::LbaOutOfRange);
        require(output == Bytes({0x42}), "bad-LBA read changed output");
    }
    for (const std::size_t length : {std::size_t{0}, std::size_t{7}, std::size_t{9}}) {
        expect(ftl.write(0, Bytes(length, 0)), FtlError::DataSizeMismatch);
        expect(ftl.write(1, Bytes(length, 0)), FtlError::DataSizeMismatch);
    }
    require(snapshot(ftl) == before, "invalid operation changed mappings or NAND");
    expect(ftl.write(1, Bytes(8, 0xCD)));
    require(ftl.physical_address(1) == PhysicalAddress{0, 1}, "failed writes consumed pages");
}

void full_device_keeps_latest_data_and_mapping() {
    PageFtl ftl({1, 4, 8}, 3, GcMode::Disabled);
    expect(ftl.write(0, Bytes(8, 0xA1)));
    expect(ftl.write(1, Bytes(8, 0xB1)));
    expect(ftl.write(0, Bytes(8, 0xA2)));
    expect(ftl.write(0, Bytes(8, 0xA3)));
    require(ftl.stats() == FtlStats{0, 2, 2}, "full-device accounting wrong");
    const auto before = snapshot(ftl);
    expect(ftl.write(0, Bytes(8, 0xA4)), FtlError::NoFreePage);
    expect(ftl.write(2, Bytes(8, 0xC1)), FtlError::NoFreePage);
    require(snapshot(ftl) == before, "out-of-space write lost old data or consumed state");
    check_invariants(ftl, {Bytes(8, 0xA3), Bytes(8, 0xB1), std::nullopt});
}

void diagnostic_queries_are_bounded_and_return_copies() {
    PageFtl ftl({2, 3, 8}, 2, GcMode::Disabled);
    const auto maximum = std::numeric_limits<std::size_t>::max();
    require(!ftl.physical_address(2) && !ftl.physical_address(maximum), "bad mapping query accepted");
    require(!ftl.page_info({2, 0}) && !ftl.page_info({0, 3}) &&
            !ftl.page_info({maximum, maximum}), "bad physical address accepted");
    expect(ftl.write(0, Bytes(8, 0x55)));
    auto info = ftl.page_info({0, 0}).value();
    info.state = PageState::Free;
    info.lba.reset();
    require(ftl.page_info({0, 0}) == PhysicalPageInfo{PageState::Valid, 0},
            "diagnostic copy modified internal metadata");
}

void buffers_are_owned_independently() {
    PageFtl ftl({1, 4, 8}, 2, GcMode::Disabled);
    Bytes data(8, 0xAA);
    expect(ftl.write(0, data));
    data[0] = 0;
    auto output = read(ftl, 0);
    output[1] = 0;
    require(read(ftl, 0) == Bytes(8, 0xAA), "caller modified stored data");
}

void mixed_workload_against_logical_oracle() {
    // The oracle stores only the newest bytes per LBA, with no physical
    // mapping algorithm. Fresh devices keep successes/updates well exercised.
    constexpr std::size_t physical_pages = 24;
    constexpr std::size_t logical_pages = 12;
    constexpr std::size_t page_bytes = 32;
    for (const std::uint32_t seed : {0x46544C31U, 42U, 20261009U}) {
        std::mt19937 random(seed);
        for (std::size_t device = 0; device < 50; ++device) {
            PageFtl ftl({3, 8, page_bytes}, logical_pages, GcMode::Disabled);
            LogicalData expected(logical_pages);
            std::size_t successful_writes = 0;
            for (std::size_t step = 0; step < 200; ++step) {
                try {
                    const auto lba = static_cast<std::size_t>(random() % (logical_pages + 2));
                    if (random() % 3 != 0) {
                        const auto length = static_cast<std::size_t>(page_bytes - 1 + random() % 3);
                        Bytes data(length);
                        for (auto& byte : data) { byte = static_cast<std::uint8_t>(random() & 0xFFU); }
                        const auto error = lba >= logical_pages ? FtlError::LbaOutOfRange
                            : length != page_bytes ? FtlError::DataSizeMismatch
                            : successful_writes == physical_pages ? FtlError::NoFreePage
                            : FtlError::None;
                        expect(ftl.write(lba, data), error);
                        if (error == FtlError::None) {
                            expected[lba] = data;
                            ++successful_writes;
                        }
                    } else {
                        Bytes output{0xDE, 0xAD};
                        const auto error = lba >= logical_pages ? FtlError::LbaOutOfRange
                            : !expected[lba] ? FtlError::UnwrittenLba : FtlError::None;
                        expect(ftl.read(lba, output), error);
                        require(output == (error == FtlError::None ? *expected[lba] : Bytes({0xDE, 0xAD})),
                                "read output differs from oracle");
                    }
                    check_invariants(ftl, expected);
                    const auto stats = ftl.stats();
                    require(stats.free_pages == physical_pages - successful_writes &&
                            stats.invalid_pages == successful_writes - stats.valid_pages,
                            "successful writes and physical consumption differ");
                } catch (const std::exception& error) {
                    throw std::runtime_error("seed=" + std::to_string(seed) +
                        " device=" + std::to_string(device) + " step=" +
                        std::to_string(step) + ": " + error.what());
                }
            }
        }
    }
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"construction and empty mappings", construction_and_initial_state},
        {"default logical and physical capacities", default_capacity},
        {"invalid logical capacities rejected", invalid_logical_capacity},
        {"first writes and physical block boundary", first_write_and_block_boundary},
        {"updates preserve neighbors and retain invalid physical data", updates_preserve_neighbors_and_old_physical_bytes},
        {"identical all-FF updates use new pages", identical_all_ff_updates_consume_new_pages},
        {"invalid operations preserve all mappings and NAND", invalid_operations_preserve_all_state},
        {"full device preserves latest data and mappings", full_device_keeps_latest_data_and_mapping},
        {"diagnostic bounds and metadata copies", diagnostic_queries_are_bounded_and_return_copies},
        {"input and read buffers cannot alter stored data", buffers_are_owned_independently},
        {"logical oracle: 3 seeds x 50 devices x 200 operations", mixed_workload_against_logical_oracle},
    };
    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << '\n' << tests.size() - failures << '/' << tests.size() << " FTL tests passed.\n";
    return failures == 0 ? 0 : 1;
}
