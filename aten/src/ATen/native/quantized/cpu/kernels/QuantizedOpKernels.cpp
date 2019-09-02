#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cpu/Loops.h>
#include <ATen/native/quantized/cpu/quantized_ops.h>

namespace at {
namespace native {
namespace {

// ****** HEY YOU! YES YOU! Read this! ************
//
// This file is compiled multiple times for different CPU vector instruction
// sets (e.g. AVX, AVX2). The purpose of putting code in this file is to make
// sure we can generate the optimal code for a given processor's vector
// capabilities. Much of this is done via preprocessor guards in vec256_qint.h.
// Although the code in this file may look generic, the instantiations of
// the Vec256 methods can be wildly different!
//
// The considerations for code written in this file include:
//  - Keep code in this file to a minimum, since we're compiling it several
//    times.
//  - All code in this file should go through the DECLARE_DISPATCH,
//    DEFINE_DISPATCH, and REGISTER_DISPATCH mechanism to ensure the correct
//    runtime dispatch occurs.
//  - THE CODE MUST RESIDE IN THE ANONYMOUS NAMESPACE. FAILURE TO ENSURE THIS
//    IS THE CASE CAN LEAD TO HARD-TO-DEBUG ODR VIOLATIONS.
//  - **Make sure different variants of the code (AVX, AVX2) are tested!**
//    There are build variants that do things like have NO AVX and NO AVX2 in
//    CI. Make sure they work!

void qrelu_kernel(const Tensor& qx, Tensor& qy) {
  const auto zero_point = qx.q_zero_point();
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qrelu", [&]() {
    qy = at::_empty_affine_quantized(
        qx.sizes(),
        at::device(kCPU).dtype(SCALAR_TYPE),
        qx.q_scale(),
        qx.q_zero_point(),
        qx.suggest_memory_format());
    using Vec = Vec256<scalar_t>;
    auto zero_point_vec = Vec(scalar_t(zero_point));
    auto iter = TensorIterator::unary_op(qy, qx);
    cpu_kernel_vec(
        iter,
        [&](scalar_t value) -> scalar_t {
          return scalar_t(std::max<underlying_t>(value.val_, zero_point));
        },
        [&](Vec value) -> Vec { return value.relu(zero_point_vec); });
  });
}

void qrelu6_kernel(const Tensor& qx, Tensor& qy) {
  const auto zero_point = qx.q_zero_point();
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qrelu6", [&]() {
    qy = at::_empty_affine_quantized(
        qx.sizes(),
        at::device(kCPU).dtype(SCALAR_TYPE),
        qx.q_scale(),
        qx.q_zero_point(),
        qx.suggest_memory_format());
    using Vec = Vec256<scalar_t>;
    auto iter = TensorIterator::unary_op(qy, qx);
    scalar_t six =
        at::quantize_val<scalar_t>(qx.q_scale(), qx.q_zero_point(), 6.0);
    auto zero_point_vec = Vec(scalar_t(zero_point));
    auto six_vec = Vec(six);
    cpu_kernel_vec(
        iter,
        [&](scalar_t value) -> scalar_t {
          underlying_t relu_val =
              std::max<underlying_t>(value.val_, zero_point);
          return scalar_t(std::min<underlying_t>(relu_val, six.val_));
        },
        [&](Vec val) -> Vec { return val.relu6(zero_point_vec, six_vec); });
  });
}

// Note: out is assumed to be the same size as self and other.
// Note: Addition is only supported when self, other, out are of the same dtype.
template <bool ReLUFused = false>
void qadd_kernel(Tensor& out, const Tensor& self, const Tensor& other) {
  int64_t zero_point = out.q_zero_point();
  double scale = out.q_scale();
  int64_t self_zero_point = self.q_zero_point();
  double self_scale = self.q_scale();
  int64_t other_zero_point = other.q_zero_point();
  double other_scale = other.q_scale();

  // Broadcast out the parameters here to amortize out that cost across
  // loop iterations.
  // TODO: we can optimize dequantization by doing a premultiplication
  // of the zero point by scale and doing FMA on scale*x_q - (scale*zero_point)
  auto self_zero_point_vec = Vec256<float>((float)self_zero_point);
  auto self_scale_vec = Vec256<float>(self_scale);
  auto other_zero_point_vec = Vec256<float>((float)other_zero_point);
  auto other_scale_vec = Vec256<float>(other_scale);

  auto iter = TensorIterator::binary_op(out, self, other);

  AT_DISPATCH_QINT_TYPES(out.scalar_type(), "qadd", [&]() {
    using Vec = Vec256<scalar_t>;
    cpu_kernel_vec(
        iter,
        [&](scalar_t a, scalar_t b) -> scalar_t {
          const auto da = at::dequantize_val(self_scale, self_zero_point, a);
          const auto db = at::dequantize_val(other_scale, other_zero_point, b);
          float c = da + db;
          if (ReLUFused) {
            c = std::max<float>(c, 0.0);
          }
          return at::quantize_val<scalar_t>(scale, zero_point, c);
        },
        [&](Vec a, Vec b) -> Vec {
          const auto da = a.dequantize(self_scale_vec, self_zero_point_vec);
          const auto db = b.dequantize(other_scale_vec, other_zero_point_vec);
          Vec::float_vec_return_type retvals;
          for (int i = 0; i < Vec::float_num_vecs(); ++i) {
            auto c = da[i] + db[i];
            if (ReLUFused) {
              c = vec256::maximum(c, Vec256<float>(0.0f));
            }
            retvals[i] = c;
          }
          // TODO: fbgemm::Quantize doesn't support taking in the
          // pre-broadcasted parameters. We might be able to save some cycles by
          // enabling that in the API.
          // TODO: specialize fbgemm::Quantize for a single vector and make it
          // inlineable. This could help with interleaving as suggested by the
          // TensorIterator implementations
          auto rv = Vec::quantize(retvals, scale, zero_point);
          return rv;
        });
  });
}

} // namespace

REGISTER_DISPATCH(qrelu_stub, &qrelu_kernel);
REGISTER_DISPATCH(qrelu6_stub, &qrelu6_kernel);
REGISTER_DISPATCH(qadd_relu_stub, &qadd_kernel<true>);
REGISTER_DISPATCH(qadd_stub, &qadd_kernel<false>);

} // namespace native
} // namespace at