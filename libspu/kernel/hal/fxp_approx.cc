// Copyright 2021 Ant Group Co., Ltd.
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

#include "libspu/kernel/hal/fxp_approx.h"

#include <algorithm>
#include <cmath>
#include <future>

#include "libspu/kernel/hal/constants.h"
#include "libspu/kernel/hal/fxp_base.h"
#include "libspu/kernel/hal/fxp_cleartext.h"
#include "libspu/kernel/hal/ring.h"

namespace spu::kernel::hal {

namespace {

// simple convenient function.
Value f_constant(HalContext* ctx, const PtBufferView& init,
                 absl::Span<int64_t const> shape) {
  return constant(ctx, init, DT_FXP, shape);
}

}  // namespace

namespace detail {

// Pade approximation fo x belongs to [0.5, 1]:
//
// p2524(x) = -0.205466671951 * 10
//          + x * -0.88626599391 * 10
//          + x^2 * 0.610585199015 * 10
//          + x^3 * 0.481147460989 * 10
// q2524(x) = 0.353553425277
//          + x * 0.454517087629 * 10
//          + x^2 * 0.642784209029 * 10
//          + x^3 * 0.1 *10
// log2(x) = p2524(x) / q2524(x)
//
Value log2_pade_approx_for_normalized(HalContext* ctx, const Value& x) {
  const auto x2 = f_square(ctx, x);
  const auto x3 = f_mul(ctx, x2, x);

  const auto p0 = f_constant(ctx, -0.205466671951 * 10, x.shape());
  const auto p1 = f_constant(ctx, -0.88626599391 * 10, x.shape());
  const auto p2 = f_constant(ctx, 0.610585199015 * 10, x.shape());
  const auto p3 = f_constant(ctx, 0.481147460989 * 10, x.shape());

  const auto q0 = f_constant(ctx, 0.353553425277, x.shape());
  const auto q1 = f_constant(ctx, 0.454517087629 * 10, x.shape());
  const auto q2 = f_constant(ctx, 0.642784209029 * 10, x.shape());
  const auto q3 = f_constant(ctx, 0.1 * 10, x.shape());

  auto p2524 = _mul(ctx, x, p1);
  p2524 = _add(ctx, p2524, _mul(ctx, x2, p2));
  p2524 = _add(ctx, p2524, _mul(ctx, x3, p3));
  p2524 = _add(ctx, _trunc(ctx, p2524), p0).asFxp();

  auto q2524 = _mul(ctx, x, q1);
  q2524 = _add(ctx, q2524, _mul(ctx, x2, q2));
  q2524 = _add(ctx, q2524, _mul(ctx, x3, q3));
  q2524 = _add(ctx, _trunc(ctx, q2524), q0).asFxp();

  return detail::div_goldschmidt(ctx, p2524, q2524);
}

// Refer to
// Chapter 5 Exponentiation and Logarithms
// Benchmarking Privacy Preserving Scientific Operations
// https://www.esat.kuleuven.be/cosic/publications/article-3013.pdf
Value log2_pade_approx(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_DISP(ctx, x);

  const size_t bit_width = SizeOf(ctx->rt_config().field()) * 8;
  auto k = _popcount(ctx, _prefix_or(ctx, x), bit_width);

  const size_t num_fxp_bits = ctx->getFxpBits();

  // let x = x_norm * factor, where x in [0.5, 1.0)
  auto msb = detail::highestOneBit(ctx, x);
  auto factor = _bitrev(ctx, msb, 0, 2 * num_fxp_bits).asFxp();
  detail::hintNumberOfBits(factor, 2 * num_fxp_bits);
  auto norm = f_mul(ctx, x, factor);

  // log2(x) = log2(x_norm * factor)
  //         = log2(x_norm) + log2(factor)
  //         = log2(x_norm) + (k-fxp_bits)
  return _add(
             ctx, log2_pade_approx_for_normalized(ctx, norm),
             _lshift(ctx, _sub(ctx, k, _constant(ctx, num_fxp_bits, x.shape())),
                     num_fxp_bits))
      .asFxp();
}

// See P11, A.2.4 Logarithm and Exponent,
// https://lvdmaaten.github.io/publications/papers/crypten.pdf
// https://github.com/facebookresearch/CrypTen/blob/master/crypten/common/functions/approximations.py#L55-L104
// Approximates the natural logarithm using 8th order modified
// Householder iterations. This approximation is accurate within 2% relative
// error on [0.0001, 250].
Value log_householder_approx(HalContext* ctx, const Value& x) {
  Value term_1 = f_div(ctx, x, f_constant(ctx, 120.0, x.shape()));
  Value term_2 = f_mul(
      ctx,
      f_exp(ctx,
            f_negate(ctx,
                     f_add(ctx, f_mul(ctx, x, f_constant(ctx, 2.0, x.shape())),
                           f_constant(ctx, 1.0, x.shape())))),
      f_constant(ctx, 20.0, x.shape()));
  Value y =
      f_add(ctx, f_sub(ctx, term_1, term_2), f_constant(ctx, 3.0, x.shape()));

  std::vector<Value> coeffs;
  const size_t fxp_log_orders = ctx->rt_config().fxp_log_orders();
  SPU_ENFORCE(fxp_log_orders != 0, "fxp_log_orders should not be {}",
              fxp_log_orders);
  for (size_t i = 0; i < fxp_log_orders; i++) {
    coeffs.emplace_back(f_constant(ctx, 1.0 / (1.0 + i), x.shape()));
  }

  const size_t num_iters = ctx->rt_config().fxp_log_iters();
  SPU_ENFORCE(num_iters != 0, "fxp_log_iters should not be {}", num_iters);
  for (size_t i = 0; i < num_iters; i++) {
    Value h = f_sub(ctx, f_constant(ctx, 1.0, x.shape()),
                    f_mul(ctx, x, f_exp(ctx, f_negate(ctx, y))));
    y = f_sub(ctx, y, detail::f_polynomial(ctx, h, coeffs));
  }

  return y;
}

// see https://lvdmaaten.github.io/publications/papers/crypten.pdf
//   exp(x) = (1 + x / n) ^ n, when n is infinite large.
Value exp_taylor_series(HalContext* ctx, const Value& x) {
  const size_t fxp_exp_iters = ctx->rt_config().fxp_exp_iters();
  SPU_ENFORCE(fxp_exp_iters != 0, "fxp_exp_iters should not be {}",
              fxp_exp_iters);

  Value res = f_add(ctx, _trunc(ctx, x, fxp_exp_iters).asFxp(),
                    f_constant(ctx, 1.0, x.shape()));

  for (size_t i = 0; i < fxp_exp_iters; i++) {
    res = f_square(ctx, res);
  }

  return res;
}

// Pade approximation of exp2(x), x is in [0, 1].
// p1015(x) = 0.100000007744302 * 10
//             + x * 0.693147180426163
//             + x^2 * 0.240226510710170
//             + x^3 * 0.555040686204663 / 10
//             + x^4 * 0.961834122588046 / 100
//             + x^5 * 0.133273035928143 / 100
Value exp2_pade_approx_for_positive_pure_decimal(HalContext* ctx,
                                                 const Value& x) {
  auto x2 = f_mul(ctx, x, x);
  auto x3 = f_mul(ctx, x, x2);
  auto x4 = f_mul(ctx, x, x3);
  auto x5 = f_mul(ctx, x, x4);

  const auto p0 = f_constant(ctx, 0.100000007744302 * 10, x.shape());
  const auto p1 = f_constant(ctx, 0.693147180426163, x.shape());
  const auto p2 = f_constant(ctx, 0.240226510710170, x.shape());
  const auto p3 = f_constant(ctx, 0.555040686204663 / 10, x.shape());
  const auto p4 = f_constant(ctx, 0.961834122588046 / 100, x.shape());
  const auto p5 = f_constant(ctx, 0.133273035928143 / 100, x.shape());

  auto res = _mul(ctx, x, p1);
  res = _add(ctx, res, _mul(ctx, x2, p2));
  res = _add(ctx, res, _mul(ctx, x3, p3));
  res = _add(ctx, res, _mul(ctx, x4, p4));
  res = _add(ctx, res, _mul(ctx, x5, p5));

  return _add(ctx, _trunc(ctx, res), p0).asFxp();
}

// Refer to
// Chapter 5 Exponentiation and Logarithms
// Benchmarking Privacy Preserving Scientific Operations
// https://www.esat.kuleuven.be/cosic/publications/article-3013.pdf
// NOTE(junfeng): The valid integer bits of x is 5. Otherwise, the output is
// incorrect.
Value exp2_pade_approx(HalContext* ctx, const Value& x) {
  const size_t fbits = ctx->getFxpBits();
  const auto k1 = _constant(ctx, 1U, x.shape());
  // TODO(junfeng): Make int_bits configurable.
  const size_t int_bits = 5;
  const size_t bit_width = SizeOf(ctx->getField()) * 8;

  const auto x_bshare = _prefer_b(ctx, x);
  const auto x_msb = _rshift(ctx, x_bshare, bit_width - 1);
  auto x_integer = _rshift(ctx, x_bshare, fbits);
  auto x_fraction = _sub(ctx, x, _lshift(ctx, x_integer, fbits)).asFxp();
  auto ret = exp2_pade_approx_for_positive_pure_decimal(ctx, x_fraction);

  for (size_t idx = 0; idx < int_bits; idx++) {
    auto a = _and(ctx, _rshift(ctx, x_integer, idx), k1);
    detail::hintNumberOfBits(a, 1);
    a = _prefer_a(ctx, a);
    const auto K = 1U << std::min(1UL << idx, bit_width - 2);
    ret = _mul(ctx, ret,
               _add(ctx, _mul(ctx, a, _constant(ctx, K, x.shape())),
                    _sub(ctx, k1, a)))
              .asFxp();
  }

  // If we could ensure the integer bits of x is 5.
  // we have x, -x, -x_hat. x_hat is 2's complement of -x.
  // Then,
  //             x + (x_hat) = 32
  //            (x_hat) - 32 = -x
  //  exp2(x_hat) / exp2(32) = exp(-x)
  //  so exp(-x) = exp2(x_hat) / exp2(32)
  auto ret_reciprocal = _trunc(ctx, ret, std::pow(2, int_bits)).asFxp();

  // ret + msb * (reciprocal - ret)
  return f_add(ctx, ret,
               _mul(ctx, x_msb, f_sub(ctx, ret_reciprocal, ret)).asFxp());
}

Value exp_pade_approx(HalContext* ctx, const Value& x) {
  return f_exp2(
      ctx, f_mul(ctx, x, f_constant(ctx, std::log2(std::exp(1)), x.shape())));
}

// Refer to
// https://www.wolframalpha.com/input?i=Pade+approximation+tanh%28x%29+order+5%2C5.
// tanh(x) = (x + x^3 / 9.0 + x^5 /945.0) /
//           (1 + 4 * x^2 / 9.0 + x^4 / 63.0)
Value tanh_pade_approx(HalContext* ctx, const Value& x) {
  const auto x_2 = f_square(ctx, x);
  const auto x_3 = f_mul(ctx, x_2, x);
  const auto x_4 = f_square(ctx, x_2);
  const auto x_5 = f_mul(ctx, x_2, x_3);

  const auto dividend =
      f_add(ctx, x,
            f_add(ctx, f_div(ctx, x_3, f_constant(ctx, 9.0, x.shape())),
                  f_div(ctx, x_5, f_constant(ctx, 945.0, x.shape()))));

  const auto divisor =
      f_add(ctx, f_constant(ctx, 1.0, x.shape()),
            f_add(ctx, f_div(ctx, x_2, f_constant(ctx, 9.0 / 4.0, x.shape())),
                  f_div(ctx, x_4, f_constant(ctx, 63.0, x.shape()))));

  return f_div(ctx, dividend, divisor);
}

}  // namespace detail

Value f_exp(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  SPU_ENFORCE(x.isFxp());

  if (x.isPublic()) {
    return f_exp_p(ctx, x);
  }

  switch (ctx->rt_config().fxp_exp_mode()) {
    case RuntimeConfig::EXP_DEFAULT:
    case RuntimeConfig::EXP_TAYLOR:
      return detail::exp_taylor_series(ctx, x);
    case RuntimeConfig::EXP_PADE: {
      // The valid input for exp_pade_approx is [-kInputLimit, kInputLimit].
      // TODO(junfeng): should merge clamp into exp_pade_approx to save msb ops.
      const float kInputLimit = 32 / std::log2(std::exp(1));
      const auto clampped_x =
          _clamp(ctx, x, f_constant(ctx, -kInputLimit, x.shape()),
                 f_constant(ctx, kInputLimit, x.shape()))
              .setDtype(x.dtype());
      return detail::exp_pade_approx(ctx, clampped_x);
    }
    default:
      SPU_THROW("unexpected exp approxmation method {}",
                ctx->rt_config().fxp_exp_mode());
  }
}

Value f_log(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  SPU_ENFORCE(x.isFxp());

  if (x.isPublic()) {
    return f_log_p(ctx, x);
  }

  switch (ctx->rt_config().fxp_log_mode()) {
    case RuntimeConfig::LOG_DEFAULT:
    case RuntimeConfig::LOG_PADE:
      return f_mul(ctx, f_constant(ctx, std::log(2.0), x.shape()),
                   f_log2(ctx, x));
    case RuntimeConfig::LOG_NEWTON:
      return detail::log_householder_approx(ctx, x);
    default:
      SPU_THROW("unlogected log approxmation method {}",
                ctx->rt_config().fxp_log_mode());
  }
}

Value f_log1p(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  SPU_ENFORCE(x.isFxp());

  return f_log(ctx, f_add(ctx, f_constant(ctx, 1.0, x.shape()), x));
}

Value f_log2(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  SPU_ENFORCE(x.isFxp());

  return detail::log2_pade_approx(ctx, x).asFxp();
}

Value f_exp2(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  return detail::exp2_pade_approx(ctx, x);
}

Value f_tanh(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  return detail::tanh_pade_approx(ctx, x);
}

static Value rsqrt_init_guess(HalContext* ctx, const Value& x, const Value& z) {
  SPU_TRACE_HAL_LEAF(ctx, x, z);
  const size_t f = ctx->getFxpBits();

  // let u in [0.25, 0.5)
  auto z_rev = _bitrev(ctx, z, 0, 2 * f);
  detail::hintNumberOfBits(z_rev, 2 * f);

  auto u = _trunc(ctx, _mul(ctx, x, z_rev)).asFxp();

  // let rsqrt(u) = 26.02942339 * u^4 - 49.86605845 * u^3 + 38.4714796 * u^2
  // - 15.47994394 * u + 4.14285016
  spu::Value r;
  if (!ctx->rt_config().enable_lower_accuracy_rsqrt()) {
    std::vector<Value> coeffs = {f_constant(ctx, -15.47994394, x.shape()),
                                 f_constant(ctx, 38.4714796, x.shape()),
                                 f_constant(ctx, -49.86605845, x.shape()),
                                 f_constant(ctx, 26.02942339, x.shape())};
    r = f_add(ctx, detail::f_polynomial(ctx, u, coeffs),
              f_constant(ctx, 4.14285016, x.shape()));
  } else {
    std::vector<Value> coeffs = {f_constant(ctx, -5.9417, x.shape()),
                                 f_constant(ctx, 4.7979, x.shape())};
    r = f_add(ctx, detail::f_polynomial(ctx, u, coeffs),
              f_constant(ctx, 3.1855, x.shape()));
  }

  return r;
}

static Value rsqrt_comp(HalContext* ctx, const Value& x, const Value& z) {
  SPU_TRACE_HAL_LEAF(ctx, x, z);

  const size_t k = SizeOf(ctx->getField()) * 8;
  const size_t f = ctx->getFxpBits();

  // let a = 2^((e+f)/2), that is a[i] = 1 for i = (e+f)/2 else 0
  // let b = lsb(e+f)
  Value a;
  Value b;
  {
    auto z_sep = _bitdeintl(ctx, z);
    auto lo_mask =
        _constant(ctx, (static_cast<uint128_t>(1) << (k / 2)) - 1, x.shape());
    auto z_even = _and(ctx, z_sep, lo_mask);
    auto z_odd = _and(ctx, _rshift(ctx, z_sep, k / 2), lo_mask);

    // a[i] = z[2*i] ^ z[2*i+1]
    a = _xor(ctx, z_odd, z_even);
    // b ^= z[2*i]
    b = _bit_parity(ctx, z_even, k / 2);
    detail::hintNumberOfBits(b, 1);
  }

  auto a_rev = _bitrev(ctx, a, 0, (f / 2) * 2);
  detail::hintNumberOfBits(a_rev, (f / 2) * 2);

  // do compensation
  // Note:
  //   https://arxiv.org/pdf/2107.00501.pdf
  // - the magic number c0 & c1 seems to be wrong.
  // - the LSB algorithm is correct and used in this implementation.
  //
  // The following constant is deduced exactly from:
  //   https://dl.acm.org/doi/10.1145/3411501.3419427
  Value c0;
  Value c1;
  if (f % 2 == 1) {
    c0 = _constant(ctx, 1 << ((f + 3) / 2), x.shape());  // f+e even
    c1 = _constant(ctx, (1 << (f / 2 + 1)) * std::sqrt(2),
                   x.shape());  // f+e odd
  } else {
    c0 = _constant(ctx, (1 << (f / 2)) * std::sqrt(2),
                   x.shape());                     // f+e even
    c1 = _constant(ctx, 1 << (f / 2), x.shape());  // f+e odd
  }

  // let comp = 2^(-(e-1)/2) = mux(b, c1, c0) * a_rev
  return _mul(ctx, _mux(ctx, b, c0, c1), a_rev);
}

static Value rsqrt_np2(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  // let e = NP2(x), z = 2^(e+f)
  return _lshift(ctx, detail::highestOneBit(ctx, x), 1);
}

// Reference:
//  1. https://dl.acm.org/doi/10.1145/3411501.3419427
// Main idea:
//  1. convert x to u * 2^(e + 1) while u belongs to [0.25, 0.5).
//  2. get a nice approximation for u part.
//  3. get the compensation for 2^(e + 1) part.
//  4. multiple two parts and get the result.
Value f_rsqrt(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  // let e = NP2(x) , z = 2^(e+f)
  auto z = rsqrt_np2(ctx, x);

  // TODO: we should avoid fork context in hal layer, it will make global
  // scheduling harder and also make profiling harder.
  if (ctx->rt_config().experimental_enable_intra_op_par()) {
    auto sub_ctx = ctx->fork();
    auto r = std::async(rsqrt_init_guess, sub_ctx.get(), x, z);
    auto comp = rsqrt_comp(ctx, x, z);
    return _trunc(ctx, _mul(ctx, r.get(), comp)).asFxp();
  } else {
    auto r = rsqrt_init_guess(ctx, x, z);
    auto comp = rsqrt_comp(ctx, x, z);
    return _trunc(ctx, _mul(ctx, r, comp)).asFxp();
  }
}

// Reference:
// 1. https://eprint.iacr.org/2012/405.pdf, section 6.1
// 2.
// https://github.com/tf-encrypted/tf-encrypted/blob/3b0f14d26e900caf12a92a9ea2284ccd4d58e683/tf_encrypted/protocol/aby3/fp.py#L35-L52
// Goldschmidt iteration, needs an initial approximation of sqrt_inv(x).
// In the end, g is an approximation of sqrt(x) while h is an approximation of
// 1 / (2 * sqrt(x)).
Value f_sqrt(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  const auto c0 = f_constant(ctx, 0.5, x.shape());
  const auto c1 = f_constant(ctx, 1.5, x.shape());

  Value y0 = f_rsqrt(ctx, x);
  Value g = f_mul(ctx, x, y0);
  Value h = f_mul(ctx, y0, c0);

  // iterations of 1 is enough.
  const int iterations = 1;

  for (int i = 0; i < iterations; i++) {
    const auto r = f_sub(ctx, c1, f_mul(ctx, g, h));
    g = f_mul(ctx, g, r);
    h = f_mul(ctx, h, r);
  }

  return g;
}

namespace {

Value sigmiod_real(HalContext* ctx, const Value& x) {
  // f(x) = 1/(1+exp(-x))
  const auto c1 = f_constant(ctx, 1.0F, x.shape());
  return f_reciprocal(ctx, f_add(ctx, c1, f_exp(ctx, f_negate(ctx, x))));
}

Value sigmiod_mm1(HalContext* ctx, const Value& x) {
  // SigmoidMM1: f(x) = 0.5 + 0.125 * x
  const auto c1 = f_constant(ctx, 0.5, x.shape());
  const auto c2 = f_constant(ctx, 0.125, x.shape());
  return f_add(ctx, c1, f_mul(ctx, c2, x));
}

Value sigmiod_seg3(HalContext* ctx, const Value& x) {
  // f(x) = 0.5 + 0.125x if -4 <= x <= 4
  //        1            if       x > 4
  //        0            if  -4 > x
  // Rounds = Gt + Mux*2 = 4 + Log(K)
  auto upper = f_constant(ctx, 1.0F, x.shape());
  auto lower = f_constant(ctx, 0.0F, x.shape());
  auto middle = sigmiod_mm1(ctx, x);

  auto upper_bound = f_constant(ctx, 4.0F, x.shape());
  auto lower_bound = f_constant(ctx, -4.0F, x.shape());

  auto ret = _mux(ctx, f_less(ctx, upper_bound, x), upper, middle);
  return _mux(ctx, f_less(ctx, x, lower_bound), lower, ret).asFxp();
}

}  // namespace

Value f_sigmoid(HalContext* ctx, const Value& x) {
  SPU_TRACE_HAL_DISP(ctx, x);

  SPU_ENFORCE(x.isFxp());

  switch (ctx->rt_config().sigmoid_mode()) {
    case RuntimeConfig::SIGMOID_DEFAULT:
    case RuntimeConfig::SIGMOID_MM1: {
      return sigmiod_mm1(ctx, x);
    }
    case RuntimeConfig::SIGMOID_SEG3: {
      return sigmiod_seg3(ctx, x);
    }
    case RuntimeConfig::SIGMOID_REAL: {
      return sigmiod_real(ctx, x);
    }
    default: {
      SPU_THROW("Should not hit");
    }
  }
}

}  // namespace spu::kernel::hal
