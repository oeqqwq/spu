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

#include "libspu/mpc/semi2k/conversion.h"

#include "libspu/core/trace.h"
#include "libspu/core/vectorize.h"
#include "libspu/mpc/common/ab_api.h"
#include "libspu/mpc/common/ab_kernels.h"
#include "libspu/mpc/common/communicator.h"
#include "libspu/mpc/common/prg_state.h"
#include "libspu/mpc/semi2k/state.h"
#include "libspu/mpc/semi2k/type.h"
#include "libspu/mpc/utils/ring_ops.h"

namespace spu::mpc::semi2k {

ArrayRef A2B::proc(KernelEvalContext* ctx, const ArrayRef& x) const {
  SPU_TRACE_MPC_LEAF(ctx, x);

  const auto field = x.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();

  std::vector<ArrayRef> bshrs;
  const auto bty = makeType<BShrTy>(field);
  for (size_t idx = 0; idx < comm->getWorldSize(); idx++) {
    auto b = zero_b(ctx->caller(), x.numel());
    if (idx == comm->getRank()) {
      ring_xor_(b, x);
    }
    bshrs.push_back(b.as(bty));
  }

  ArrayRef res = vectorizedReduce(bshrs.begin(), bshrs.end(),
                                  [&](const ArrayRef& xx, const ArrayRef& yy) {
                                    return add_bb(ctx->caller(), xx, yy);
                                  });
  return res.as(makeType<BShrTy>(field));
}

ArrayRef B2A::proc(KernelEvalContext* ctx, const ArrayRef& x) const {
  SPU_TRACE_MPC_LEAF(ctx, x);

  const auto field = x.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();
  auto* prg_state = ctx->getState<PrgState>();

  auto r_v = prg_state->genPriv(field, x.numel());
  auto r_a = r_v.as(makeType<AShrTy>(field));

  // convert r to boolean share.
  auto r_b = a2b(ctx->caller(), r_a);

  // evaluate adder circuit on x & r, and reveal x+r
  auto x_plus_r =
      comm->allReduce(ReduceOp::XOR, add_bb(ctx->caller(), x, r_b), kBindName);

  // compute -r + (x+r)
  ring_neg_(r_a);
  if (comm->getRank() == 0) {
    ring_add_(r_a, x_plus_r);
  }
  return r_a;
}

ArrayRef B2A_Randbit::proc(KernelEvalContext* ctx, const ArrayRef& x) const {
  SPU_TRACE_MPC_LEAF(ctx, x);

  const auto field = x.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();
  auto* beaver = ctx->getState<Semi2kState>()->beaver();

  const size_t nbits = x.eltype().as<BShare>()->nbits();
  SPU_ENFORCE(nbits <= SizeOf(field) * 8, "invalid nbits={}", nbits);
  if (nbits == 0) {
    // special case, it's known to be zero.
    return ring_zeros(field, x.numel()).as(makeType<AShrTy>(field));
  }

  auto randbits = beaver->RandBit(field, x.numel() * nbits);
  auto res = ArrayRef(makeType<AShrTy>(field), x.numel());

  DISPATCH_ALL_FIELDS(field, kBindName, [&]() {
    using U = ring2k_t;

    auto _x = ArrayView<U>(x);
    auto _r = ArrayView<U>(randbits);

    // algorithm begins.
    // Ref: III.D @ https://eprint.iacr.org/2019/599.pdf (SPDZ-2K primitives)
    std::vector<U> x_xor_r(x.numel(), 0);
    pforeach(0, _x.numel(), [&](int64_t idx) {
      // use _r[i*nbits, (i+1)*nbits) to construct rb[i]
      U mask = 0;
      for (size_t bit = 0; bit < nbits; bit++) {
        mask += (_r[idx * nbits + bit] & 0x1) << bit;
      }
      x_xor_r[idx] = _x[idx] ^ mask;
    });

    // open c = x ^ r
    x_xor_r = comm->allReduce<U, std::bit_xor>(x_xor_r, "open(x^r)");

    auto _res = ArrayView<U>(res);
    pforeach(0, _x.numel(), [&](int64_t idx) {
      _res[idx] = 0;
      for (size_t bit = 0; bit < nbits; bit++) {
        auto c_i = (x_xor_r[idx] >> bit) & 0x1;
        if (comm->getRank() == 0) {
          _res[idx] += (c_i + (1 - c_i * 2) * _r[idx * nbits + bit]) << bit;
        } else {
          _res[idx] += ((1 - c_i * 2) * _r[idx * nbits + bit]) << bit;
        }
      }
    });
  });

  return res;
}

ArrayRef MsbA2B::proc(KernelEvalContext* ctx, const ArrayRef& in) const {
  SPU_TRACE_MPC_LEAF(ctx, in);

  const auto field = in.eltype().as<AShrTy>()->field();
  auto* comm = ctx->getState<Communicator>();

  // For if k > 2 parties does not collude with each other, then we can
  // construct two additive share and use carray out circuit directly.
  SPU_ENFORCE(comm->getWorldSize() == 2, "only support for 2PC, got={}",
              comm->getWorldSize());

  std::vector<ArrayRef> bshrs;
  const auto bty = makeType<BShrTy>(field);
  for (size_t idx = 0; idx < comm->getWorldSize(); idx++) {
    auto b = zero_b(ctx->caller(), in.numel());
    if (idx == comm->getRank()) {
      ring_xor_(b, in);
    }
    bshrs.push_back(b.as(bty));
  }

  // Compute the k-1'th carry bit.
  size_t k = SizeOf(field) * 8 - 1;
  if (in.numel() == 0) {
    k = 0;  // Empty matrix
  }

  auto* obj = ctx->caller();
  ArrayRef carry = common::carry_out(obj, bshrs[0], bshrs[1], k);
  return xor_bb(obj, rshift_b(obj, xor_bb(obj, bshrs[0], bshrs[1]), k), carry);
}

}  // namespace spu::mpc::semi2k
