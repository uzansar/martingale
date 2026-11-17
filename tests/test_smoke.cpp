#include <gtest/gtest.h>

#include <bit>
#include <concepts>
#include <cstdint>

namespace martingale::tests {

TEST(ToolchainSmokeTest, Cpp20StandardAndEndianness) {
    static_assert(__cplusplus >= 202002L, "C++20 standard is required");
    static_assert(std::integral<std::uint64_t>, "C++20 concepts support verified");

    EXPECT_GE(__cplusplus, 202002L);
    EXPECT_TRUE(std::integral<std::uint64_t>);
    EXPECT_EQ(std::endian::native, std::endian::little);
}

TEST(ToolchainSmokeTest, TargetArchitectureAssumptions) {
    EXPECT_EQ(sizeof(void*), 8U);
    EXPECT_EQ(sizeof(std::int64_t), 8U);
    EXPECT_EQ(sizeof(std::uint32_t), 4U);
    EXPECT_EQ(sizeof(std::uint8_t), 1U);
}

}  // namespace martingale::tests
