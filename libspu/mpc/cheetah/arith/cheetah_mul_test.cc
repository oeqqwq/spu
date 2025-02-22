// Copyright 2022 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "libspu/mpc/cheetah/arith/cheetah_mul.h"

#include "gtest/gtest.h"

#include "libspu/core/type_util.h"
#include "libspu/mpc/utils/ring_ops.h"
#include "libspu/mpc/utils/simulate.h"

namespace spu::mpc::cheetah::test {

class CheetahMulTest
    : public ::testing::TestWithParam<std::tuple<FieldType, size_t>> {};

INSTANTIATE_TEST_SUITE_P(
    Cheetah, CheetahMulTest,
    testing::Combine(testing::Values(FieldType::FM32, FieldType::FM64,
                                     FieldType::FM128),
                     testing::Values(1024, 10000)),
    [](const testing::TestParamInfo<CheetahMulTest::ParamType>& p) {
      return fmt::format("{}x{}", std::get<0>(p.param), std::get<1>(p.param));
    });

TEST_P(CheetahMulTest, Basic) {
  size_t kWorldSize = 2;
  auto field = std::get<0>(GetParam());
  auto n = std::get<1>(GetParam());

  ArrayRef a_bits = ring_rand(field, n);
  ArrayRef b_bits = ring_rand(field, n);

  std::vector<ArrayRef> a_shr(kWorldSize);
  std::vector<ArrayRef> b_shr(kWorldSize);
  a_shr[0] = ring_rand(field, n);
  b_shr[0] = ring_rand(field, n);
  a_shr[1] = ring_sub(a_bits, a_shr[0]);
  b_shr[1] = ring_sub(b_bits, b_shr[0]);

  std::vector<ArrayRef> result(kWorldSize);
  utils::simulate(kWorldSize, [&](std::shared_ptr<yacl::link::Context> lctx) {
    int rank = lctx->Rank();
    // (a0 + a1) * (b0 + b1)
    // a0*b0 + a0*b1 + a1*b0 + a1*b1
    auto mul = std::make_shared<CheetahMul>(lctx);

    ArrayRef cross0, cross1;
    if (rank == 0) {
      cross0 = mul->MulOLE(a_shr[0], true);
      cross1 = mul->MulOLE(b_shr[0], true);
    } else {
      cross0 = mul->MulOLE(b_shr[1], false);
      cross1 = mul->MulOLE(a_shr[1], false);
    }

    result[rank] = ring_mul(a_shr[rank], b_shr[rank]);
    ring_add_(result[rank], cross0);
    ring_add_(result[rank], cross1);
  });

  auto expected = ring_mul(a_bits, b_bits);
  auto computed = ring_add(result[0], result[1]);

  const int64_t kMaxDiff = 0;
  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    auto e = ArrayView<ring2k_t>(expected);
    auto c = ArrayView<ring2k_t>(computed);

    for (auto idx = 0; idx < expected.numel(); idx++) {
      EXPECT_NEAR(e[idx], c[idx], kMaxDiff);
    }
  });
}

TEST_P(CheetahMulTest, BasicBinary) {
  size_t kWorldSize = 2;
  auto field = std::get<0>(GetParam());
  auto n = std::get<1>(GetParam());

  ArrayRef a_bits = ring_rand_range(field, n, 0, 1);
  ArrayRef b_bits = ring_rand_range(field, n, 0, 1);

  std::vector<ArrayRef> a_shr(kWorldSize);
  std::vector<ArrayRef> b_shr(kWorldSize);
  a_shr[0] = ring_rand(field, n);
  b_shr[0] = ring_rand(field, n);
  a_shr[1] = ring_sub(a_bits, a_shr[0]);
  b_shr[1] = ring_sub(b_bits, b_shr[0]);

  std::vector<ArrayRef> result(kWorldSize);
  utils::simulate(kWorldSize, [&](std::shared_ptr<yacl::link::Context> lctx) {
    int rank = lctx->Rank();
    // (a0 + a1) * (b0 + b1)
    // a0*b0 + a0*b1 + a1*b0 + a1*b1
    auto mul = std::make_shared<CheetahMul>(lctx);

    ArrayRef cross0, cross1;
    if (rank == 0) {
      cross0 = mul->MulOLE(a_shr[0], true);
      cross1 = mul->MulOLE(b_shr[0], true);
    } else {
      cross0 = mul->MulOLE(b_shr[1], false);
      cross1 = mul->MulOLE(a_shr[1], false);
    }

    result[rank] = ring_mul(a_shr[rank], b_shr[rank]);
    ring_add_(result[rank], cross0);
    ring_add_(result[rank], cross1);
  });

  auto expected = ring_mul(a_bits, b_bits);
  auto computed = ring_add(result[0], result[1]);

  const int64_t kMaxDiff = 0;
  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    auto e = ArrayView<ring2k_t>(expected);
    auto c = ArrayView<ring2k_t>(computed);

    for (auto idx = 0; idx < expected.numel(); idx++) {
      EXPECT_NEAR(e[idx], c[idx], kMaxDiff);
    }
  });
}

}  // namespace spu::mpc::cheetah::test
