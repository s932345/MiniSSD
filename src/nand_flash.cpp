#include "minissd/nand_flash.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace minissd {
namespace {

std::size_t checked_product(std::size_t left, std::size_t right) {
    if (left > std::numeric_limits<std::size_t>::max() / right) {
        throw std::overflow_error("NAND geometry exceeds the addressable size");
    }
    return left * right;
}

}  // namespace

std::string_view describe(NandError error) noexcept {
    switch (error) {
        case NandError::None: return "success";
        case NandError::BlockOutOfRange: return "block out of range";
        case NandError::PageOutOfRange: return "page out of range";
        case NandError::DataSizeMismatch: return "data must fill exactly one page";
        case NandError::PageAlreadyProgrammed:
            return "page already programmed; erase its block before reuse";
        case NandError::EraseCountOverflow: return "erase counter exhausted";
        case NandError::InjectedReadFailure: return "injected read failure";
        case NandError::InjectedProgramFailure: return "injected program failure";
        case NandError::InjectedEraseFailure: return "injected erase failure";
    }
    return "unknown NAND error";
}

NandFlash::NandFlash(NandGeometry geometry) : geometry_(geometry) {
    if (geometry.block_count == 0 || geometry.pages_per_block == 0 ||
        geometry.bytes_per_page == 0) {
        throw std::invalid_argument("All NAND geometry dimensions must be positive");
    }

    const auto total_pages = checked_product(
        geometry.block_count, geometry.pages_per_block);
    const auto total_bytes = checked_product(total_pages, geometry.bytes_per_page);

    // Check before allocating, so invalid configurations fail predictably.
    if (total_bytes > storage_.max_size() ||
        total_pages > programmed_.max_size() ||
        geometry.block_count > erase_counts_.max_size()) {
        throw std::length_error("NAND geometry exceeds container limits");
    }

    storage_.assign(total_bytes, erased_value);
    programmed_.assign(total_pages, 0);
    erase_counts_.assign(geometry.block_count, 0);
}

const NandGeometry& NandFlash::geometry() const noexcept { return geometry_; }

std::size_t NandFlash::capacity_bytes() const noexcept { return storage_.size(); }

NandError NandFlash::validate_address(
    std::size_t block, std::size_t page) const noexcept {
    if (block >= geometry_.block_count) {
        return NandError::BlockOutOfRange;
    }
    if (page >= geometry_.pages_per_block) {
        return NandError::PageOutOfRange;
    }
    return NandError::None;
}

std::size_t NandFlash::page_index(
    std::size_t block, std::size_t page) const noexcept {
    return block * geometry_.pages_per_block + page;
}

NandError NandFlash::read_page(
    std::size_t block, std::size_t page,
    std::vector<std::uint8_t>& output) const {
    const auto error = validate_address(block, page);
    if (error != NandError::None) {
        return error;
    }

    if (should_fail(NandOperation::Read)) {
        return NandError::InjectedReadFailure;
    }

    const auto offset = page_index(block, page) * geometry_.bytes_per_page;
    output.resize(geometry_.bytes_per_page);
    std::copy_n(storage_.data() + offset, geometry_.bytes_per_page, output.data());
    return NandError::None;
}

NandError NandFlash::program_page(
    std::size_t block, std::size_t page,
    const std::vector<std::uint8_t>& data) {
    const auto error = validate_address(block, page);
    if (error != NandError::None) {
        return error;
    }
    if (data.size() != geometry_.bytes_per_page) {
        return NandError::DataSizeMismatch;
    }

    const auto index = page_index(block, page);
    if (programmed_[index] != 0) {
        return NandError::PageAlreadyProgrammed;
    }
    if (should_fail(NandOperation::Program)) {
        return NandError::InjectedProgramFailure;
    }

    // Every writable page was initialized/erased to 0xFF. With one program
    // per erase, copying the payload cannot create a 0-to-1 transition.
    const auto offset = index * geometry_.bytes_per_page;
    std::copy(data.begin(), data.end(), storage_.data() + offset);
    programmed_[index] = 1;
    return NandError::None;
}

NandError NandFlash::erase_block(std::size_t block) {
    if (block >= geometry_.block_count) {
        return NandError::BlockOutOfRange;
    }
    if (erase_counts_[block] == std::numeric_limits<std::uint64_t>::max()) {
        return NandError::EraseCountOverflow;
    }
    if (should_fail(NandOperation::Erase)) {
        return NandError::InjectedEraseFailure;
    }

    const auto first_page = page_index(block, 0);
    const auto byte_offset = first_page * geometry_.bytes_per_page;
    const auto block_bytes = geometry_.pages_per_block * geometry_.bytes_per_page;
    std::fill_n(storage_.data() + byte_offset, block_bytes, erased_value);
    std::fill_n(programmed_.data() + first_page, geometry_.pages_per_block, 0);
    ++erase_counts_[block];
    return NandError::None;
}

NandError NandFlash::get_erase_count(
    std::size_t block, std::uint64_t& output) const noexcept {
    if (block >= geometry_.block_count) {
        return NandError::BlockOutOfRange;
    }
    output = erase_counts_[block];
    return NandError::None;
}

void NandFlash::inject_failure(NandOperation operation, std::size_t skip) {
    const auto index = static_cast<std::size_t>(operation);
    if (index >= failures_.size()) {
        throw std::invalid_argument("Unknown NAND operation");
    }
    failures_[index] = skip;
}

void NandFlash::clear_failures() noexcept {
    for (auto& failure : failures_) { failure.reset(); }
}

bool NandFlash::should_fail(NandOperation operation) const noexcept {
    auto& remaining = failures_[static_cast<std::size_t>(operation)];
    if (!remaining) { return false; }
    if (*remaining != 0) { --*remaining; return false; }
    remaining.reset();
    return true;
}

}  // namespace minissd
