//*****************************************************************************
// Copyright 2017-2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include <climits>
#include <random>

#include "gtest/gtest.h"

#include "ngraph/runtime/aligned_buffer.hpp"
#include "ngraph/type/float16.hpp"
#include "util/float_util.hpp"

using namespace std;
using namespace ngraph;

TEST(float16, conversions)
{
    float16 f16;
    const char* source_string;
    std::string f16_string;

    // 1.f
    source_string = "0  01111  00 0000 0000";
    f16 = test::bits_to_float16(source_string);
    EXPECT_EQ(f16, float16(1.0));
    f16_string = test::float16_to_bits(f16);
    EXPECT_STREQ(source_string, f16_string.c_str());
    EXPECT_EQ(static_cast<float>(f16), 1.0);

    // -1.f
    source_string = "1  01111  00 0000 0000";
    f16 = test::bits_to_float16(source_string);
    EXPECT_EQ(f16, float16(-1.0));
    f16_string = test::float16_to_bits(f16);
    EXPECT_STREQ(source_string, f16_string.c_str());
    EXPECT_EQ(static_cast<float>(f16), -1.0);

    // 0.f
    source_string = "0  00000  00 0000 0000";
    f16 = test::bits_to_float16(source_string);
    EXPECT_EQ(f16, float16(0.0));
    f16_string = test::float16_to_bits(f16);
    EXPECT_STREQ(source_string, f16_string.c_str());
    EXPECT_EQ(static_cast<float>(f16), 0.0);

    // 1.5f
    source_string = "0  01111  10 0000 0000";
    f16 = test::bits_to_float16(source_string);
    EXPECT_EQ(f16, float16(1.5));
    f16_string = test::float16_to_bits(f16);
    EXPECT_STREQ(source_string, f16_string.c_str());
    EXPECT_EQ(static_cast<float>(f16), 1.5);
}
