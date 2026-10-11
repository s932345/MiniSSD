#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace minissd {

// These are simulator settings, not the specification of a real NAND chip.
struct NandGeometry {
    std::size_t block_count = 16;
    std::size_t pages_per_block = 16;
    std::size_t bytes_per_page = 4096;
};

enum class NandError {
    None,
    BlockOutOfRange,
    PageOutOfRange,
    DataSizeMismatch,
    PageAlreadyProgrammed,
    EraseCountOverflow,
    InjectedReadFailure,
    InjectedProgramFailure,
    InjectedEraseFailure,
};

enum class NandOperation { Read, Program, Erase };

[[nodiscard]] std::string_view describe(NandError error) noexcept;

// Synchronous, in-memory NAND model. Each page may be programmed once per
// erase cycle. This class owns its bytes; it is not thread-safe.
class NandFlash {
public:
    static constexpr std::uint8_t erased_value = 0xFF;

    // Zero dimensions throw invalid_argument; size overflow throws
    // overflow_error. Allocation failures use the standard vector exceptions.
    explicit NandFlash(NandGeometry geometry = {});

    // A simulated device has one owner and a stable identity. Later the FTL
    // can hold a reference without accidental device copies or moves.
    NandFlash(const NandFlash&) = delete;
    NandFlash& operator=(const NandFlash&) = delete;
    NandFlash(NandFlash&&) = delete;
    NandFlash& operator=(NandFlash&&) = delete;

    [[nodiscard]] const NandGeometry& geometry() const noexcept;
    [[nodiscard]] std::size_t capacity_bytes() const noexcept;

    // Reads copy a whole page. On an address error, output is unchanged.
    [[nodiscard]] NandError read_page(
        std::size_t block, std::size_t page,
        std::vector<std::uint8_t>& output) const;

    // The payload must contain exactly bytes_per_page bytes.
    // Any returned error leaves the NAND state unchanged.
    [[nodiscard]] NandError program_page(
        std::size_t block, std::size_t page,
        const std::vector<std::uint8_t>& data);

    // Erases every page in one block and increments that block's erase count.
    [[nodiscard]] NandError erase_block(std::size_t block);

    // On an invalid block, output is unchanged.
    [[nodiscard]] NandError get_erase_count(
        std::size_t block, std::uint64_t& output) const noexcept;

    // Test hook: fail one otherwise-valid operation before changing bytes.
    // Skip the specified number of valid operations first. One slot per kind.
    void inject_failure(NandOperation operation, std::size_t skip = 0);
    void clear_failures() noexcept;

private:
    [[nodiscard]] NandError validate_address(
        std::size_t block, std::size_t page) const noexcept;
    [[nodiscard]] std::size_t page_index(
        std::size_t block, std::size_t page) const noexcept;
    [[nodiscard]] bool should_fail(NandOperation operation) const noexcept;

    NandGeometry geometry_;
    std::vector<std::uint8_t> storage_;
    // Explicit metadata: an all-0xFF program still consumes the page.
    std::vector<std::uint8_t> programmed_;
    std::vector<std::uint64_t> erase_counts_;
    mutable std::array<std::optional<std::size_t>, 3> failures_{};
};

}  // namespace minissd
