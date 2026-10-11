#include "minissd/nand_flash.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using minissd::NandError;
using minissd::NandFlash;
using minissd::NandGeometry;
using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

void expect_error(NandError actual, NandError expected) {
    require(actual == expected,
            "expected " + std::string(minissd::describe(expected)) +
            ", got " + std::string(minissd::describe(actual)));
}

Bytes read(const NandFlash& nand, std::size_t block, std::size_t page) {
    Bytes data;
    expect_error(nand.read_page(block, page, data), NandError::None);
    return data;
}

std::uint64_t erase_count(const NandFlash& nand, std::size_t block) {
    std::uint64_t count = 0;
    expect_error(nand.get_erase_count(block, count), NandError::None);
    return count;
}

template <typename Exception, typename Function>
void expect_throw(Function function) {
    bool caught = false;
    try { function(); }
    catch (const Exception&) { caught = true; }
    require(caught, "expected constructor exception");
}

void initial_state() {
    NandFlash nand({3, 4, 8});
    require(nand.capacity_bytes() == 96, "wrong capacity");
    for (std::size_t block = 0; block < 3; ++block) {
        require(erase_count(nand, block) == 0, "new device has erase history");
        for (std::size_t page = 0; page < 4; ++page) {
            require(read(nand, block, page) == Bytes(8, 0xFF), "not erased");
        }
    }
}

void default_geometry() {
    NandFlash nand;
    require(nand.geometry().block_count == 16, "wrong default block count");
    require(nand.geometry().pages_per_block == 16, "wrong default page count");
    require(nand.geometry().bytes_per_page == 4096, "wrong default page size");
    require(nand.capacity_bytes() == 1024 * 1024, "wrong default capacity");
    require(read(nand, 15, 15) == Bytes(4096, 0xFF), "last page inaccessible");
}

void program_round_trip() {
    NandFlash nand({2, 3, 256});
    Bytes data(256);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<std::uint8_t>(index);
    }
    expect_error(nand.program_page(1, 2, data), NandError::None);
    require(read(nand, 1, 2) == data, "payload changed on round trip");
    require(read(nand, 1, 1) == Bytes(256, 0xFF), "neighbor page changed");
    require(read(nand, 0, 2) == Bytes(256, 0xFF), "other block changed");
    require(erase_count(nand, 1) == 0, "program must not count as erase");
    data[0] = 0xFF;
    require(read(nand, 1, 2)[0] == 0, "device retained caller's buffer");
}

void overwrite_rejected() {
    NandFlash nand({1, 2, 8});
    const Bytes original(8, 0xAA);
    expect_error(nand.program_page(0, 0, original), NandError::None);
    // Even a transition that only clears more bits is rejected by this model.
    expect_error(nand.program_page(0, 0, Bytes(8, 0)),
                 NandError::PageAlreadyProgrammed);
    expect_error(nand.program_page(0, 0, Bytes(8, 0xFF)),
                 NandError::PageAlreadyProgrammed);
    require(read(nand, 0, 0) == original, "failed program corrupted page");
}

void all_ff_program_consumes_page() {
    NandFlash nand({1, 1, 8});
    expect_error(nand.program_page(0, 0, Bytes(8, 0xFF)), NandError::None);
    expect_error(nand.program_page(0, 0, Bytes(8, 0x55)),
                 NandError::PageAlreadyProgrammed);
    require(read(nand, 0, 0) == Bytes(8, 0xFF), "all-FF data corrupted");
    expect_error(nand.erase_block(0), NandError::None);
    expect_error(nand.program_page(0, 0, Bytes(8, 0x55)), NandError::None);
}

void invalid_payload_does_not_consume_page() {
    NandFlash nand({1, 1, 8});
    for (const std::size_t size : {std::size_t{0}, std::size_t{7}, std::size_t{9}}) {
        expect_error(nand.program_page(0, 0, Bytes(size, 0xAA)),
                     NandError::DataSizeMismatch);
        require(read(nand, 0, 0) == Bytes(8, 0xFF), "invalid payload changed bytes");
    }
    expect_error(nand.program_page(0, 0, Bytes(8, 0xAA)), NandError::None);
}

void invalid_addresses_preserve_state_and_outputs() {
    NandFlash nand({2, 2, 8});
    const Bytes original(8, 0xAB);
    expect_error(nand.program_page(0, 0, original), NandError::None);
    const auto maximum = std::numeric_limits<std::size_t>::max();
    for (const std::size_t bad_block : {std::size_t{2}, maximum}) {
        Bytes output{0x12, 0x34};
        expect_error(nand.read_page(bad_block, 0, output), NandError::BlockOutOfRange);
        require(output == Bytes({0x12, 0x34}), "failed read changed output");
        expect_error(nand.program_page(bad_block, 0, original),
                     NandError::BlockOutOfRange);
        expect_error(nand.erase_block(bad_block), NandError::BlockOutOfRange);
        std::uint64_t count = 123;
        expect_error(nand.get_erase_count(bad_block, count), NandError::BlockOutOfRange);
        require(count == 123, "failed count query changed output");
    }
    for (const std::size_t bad_page : {std::size_t{2}, maximum}) {
        Bytes output{0x42};
        expect_error(nand.read_page(0, bad_page, output), NandError::PageOutOfRange);
        require(output == Bytes({0x42}), "failed page read changed output");
        expect_error(nand.program_page(0, bad_page, original), NandError::PageOutOfRange);
    }
    require(read(nand, 0, 0) == original, "invalid address corrupted valid page");
    require(read(nand, 1, 1) == Bytes(8, 0xFF), "invalid address crossed boundary");
    require(erase_count(nand, 0) == 0 && erase_count(nand, 1) == 0,
            "invalid erase changed counters");
}

void block_erase_isolated() {
    NandFlash nand({2, 4, 8});
    for (std::size_t page = 0; page < 4; ++page) {
        expect_error(nand.program_page(0, page, Bytes(8, 0x11)), NandError::None);
        expect_error(nand.program_page(1, page, Bytes(8, 0x22)), NandError::None);
    }
    expect_error(nand.erase_block(0), NandError::None);
    for (std::size_t page = 0; page < 4; ++page) {
        require(read(nand, 0, page) == Bytes(8, 0xFF), "erase missed a page");
        require(read(nand, 1, page) == Bytes(8, 0x22), "erase crossed block boundary");
        expect_error(nand.program_page(1, page, Bytes(8, 0x33)),
                     NandError::PageAlreadyProgrammed);
        expect_error(nand.program_page(0, page, Bytes(8, 0x44)), NandError::None);
    }
    require(erase_count(nand, 0) == 1 && erase_count(nand, 1) == 0,
            "wrong erase counts");
}

void erase_counts_track_operations() {
    NandFlash nand({2, 1, 8});
    for (std::uint64_t expected = 1; expected <= 5; ++expected) {
        expect_error(nand.erase_block(1), NandError::None);
        require(erase_count(nand, 1) == expected, "empty erase must be counted");
        expect_error(nand.program_page(1, 0, Bytes(8, 0)), NandError::None);
        require(read(nand, 1, 0) == Bytes(8, 0), "read changed data");
        require(erase_count(nand, 1) == expected, "read/program changed erase count");
    }
    require(erase_count(nand, 0) == 0, "other counter changed");
}

void read_returns_independent_copy() {
    NandFlash nand({1, 1, 8});
    expect_error(nand.program_page(0, 0, Bytes(8, 0xAA)), NandError::None);
    auto result = read(nand, 0, 0);
    result[0] = 0;
    require(read(nand, 0, 0) == Bytes(8, 0xAA), "read exposed internal storage");
}

void invalid_geometry() {
    for (const NandGeometry geometry : {NandGeometry{0, 1, 8},
                                       NandGeometry{1, 0, 8},
                                       NandGeometry{1, 1, 0}}) {
        expect_throw<std::invalid_argument>([geometry] { NandFlash nand(geometry); });
    }
}

void overflowing_geometry() {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    expect_throw<std::overflow_error>([maximum] { NandFlash nand({maximum, 2, 1}); });
    expect_throw<std::overflow_error>([maximum] { NandFlash nand({2, 2, maximum / 4 + 1}); });
    expect_throw<std::length_error>([maximum] { NandFlash nand({1, 1, maximum}); });
}

void deterministic_mixed_workload() {
    // An independent oracle uses nested blocks/pages instead of the model's
    // flat offsets. Check the entire device after EVERY operation, including
    // invalid operations, so address/erase corruption cannot hide elsewhere.
    constexpr std::size_t blocks = 3;
    constexpr std::size_t pages = 4;
    constexpr std::size_t page_bytes = 32;
    for (const std::uint32_t seed : {0x4E414E44U, 42U, 20261009U}) {
        NandFlash nand({blocks, pages, page_bytes});
        std::mt19937 random(seed);
        std::vector<std::vector<Bytes>> expected(
            blocks, std::vector<Bytes>(pages, Bytes(page_bytes, 0xFF)));
        std::vector<std::vector<bool>> used(blocks, std::vector<bool>(pages, false));
        std::vector<std::uint64_t> counts(blocks, 0);

        for (std::size_t step = 0; step < 10000; ++step) {
            try {
                const auto block = static_cast<std::size_t>(random() % (blocks + 1));
                const auto page = static_cast<std::size_t>(random() % (pages + 1));
                const auto operation = random() % 3;
                const bool valid_block = block < blocks;
                const bool valid_page = page < pages;
                const auto address_error = !valid_block ? NandError::BlockOutOfRange
                    : !valid_page ? NandError::PageOutOfRange : NandError::None;

                if (operation == 0) {
                    const auto size = static_cast<std::size_t>(
                        page_bytes - 1 + random() % 3);
                    Bytes data(size);
                    for (auto& byte : data) { byte = static_cast<std::uint8_t>(random() & 0xFFU); }
                    auto expected_error = address_error;
                    if (expected_error == NandError::None) {
                        if (size != page_bytes) { expected_error = NandError::DataSizeMismatch; }
                        else if (used[block][page]) { expected_error = NandError::PageAlreadyProgrammed; }
                    }
                    expect_error(nand.program_page(block, page, data), expected_error);
                    if (expected_error == NandError::None) {
                        expected[block][page] = data;
                        used[block][page] = true;
                    }
                } else if (operation == 1) {
                    Bytes output{0xDE, 0xAD};
                    expect_error(nand.read_page(block, page, output), address_error);
                    require(output == (address_error == NandError::None
                            ? expected[block][page] : Bytes({0xDE, 0xAD})),
                            "read result differs from oracle");
                } else {
                    expect_error(nand.erase_block(block), valid_block
                        ? NandError::None : NandError::BlockOutOfRange);
                    if (valid_block) {
                        for (std::size_t index = 0; index < pages; ++index) {
                            expected[block][index].assign(page_bytes, 0xFF);
                            used[block][index] = false;
                        }
                        ++counts[block];
                    }
                }

                for (std::size_t check_block = 0; check_block < blocks; ++check_block) {
                    require(erase_count(nand, check_block) == counts[check_block],
                            "erase count differs from oracle");
                    for (std::size_t check_page = 0; check_page < pages; ++check_page) {
                        require(read(nand, check_block, check_page) == expected[check_block][check_page],
                                "full-device content differs from oracle");
                    }
                }
            } catch (const std::exception& error) {
                throw std::runtime_error("seed=" + std::to_string(seed) +
                    " step=" + std::to_string(step) + ": " + error.what());
            }
        }
    }
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"initial erased state and capacity", initial_state},
        {"default geometry and last page", default_geometry},
        {"program/read all byte values and caller ownership", program_round_trip},
        {"repeat program rejected without data loss", overwrite_rejected},
        {"all-FF program consumes page until erase", all_ff_program_consumes_page},
        {"invalid payload does not consume page", invalid_payload_does_not_consume_page},
        {"invalid addresses preserve device and outputs", invalid_addresses_preserve_state_and_outputs},
        {"whole-block erase preserves neighbors and enables reuse", block_erase_isolated},
        {"erase counters track successful erase operations", erase_counts_track_operations},
        {"read buffer cannot modify internal storage", read_returns_independent_copy},
        {"zero geometry rejected", invalid_geometry},
        {"size overflow and container limits rejected", overflowing_geometry},
        {"mixed workload: 3 seeds x 10,000 operations", deterministic_mixed_workload},
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
    std::cout << '\n' << tests.size() - failures << '/' << tests.size()
              << " tests passed.\n";
    return failures == 0 ? 0 : 1;
}
