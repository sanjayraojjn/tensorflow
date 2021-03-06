/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/tf2xla/xla_compilation_device.h"

#include <functional>
#include <memory>

#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/tf2xla/xla_context.h"
#include "tensorflow/compiler/tf2xla/xla_helpers.h"
#include "tensorflow/core/common_runtime/local_device.h"
#include "tensorflow/core/framework/device_base.h"
#include "tensorflow/core/platform/mem.h"

namespace tensorflow {

// The XlaCompilationAllocator doesn't actually back any Tensors with storage
// buffers of values: instead for each Tensor it stores a
// XlaExpression which corresponds to the XLA computation
// represented by the Tensor.
class XlaCompilationAllocator : public Allocator {
 public:
  XlaCompilationAllocator() {}
  ~XlaCompilationAllocator() override {}

  string Name() override { return "xla_compilation"; }

  void* AllocateRaw(size_t alignment, size_t num_bytes) override {
    // Regardless of the size requested, always allocates an XlaExpression.
    // Respects the aligment request because there is alignment checking even
    // for Tensors whose data is never accessed.
    void* p = port::AlignedMalloc(sizeof(XlaExpression), alignment);
    XlaExpression* expression = reinterpret_cast<XlaExpression*>(p);
    new (expression) XlaExpression();
    return expression;
  }

  void DeallocateRaw(void* ptr) override {
    XlaExpression* expression = reinterpret_cast<XlaExpression*>(ptr);
    expression->~XlaExpression();
    port::AlignedFree(ptr);
  }

  // Make sure that even tensors with 0 elements have allocated
  // buffers, so they get ids to track.
  bool ShouldAllocateEmptyTensors() override { return true; }

  void GetStats(AllocatorStats* stats) override { stats->Clear(); }

 private:
  // Don't run any constructors or destructors for complex objects,
  // since there is no backing store for the tensor to run them
  // on. strings are the only complex objects currently stored in
  // Tensors. If others are added, this set of overrides must be
  // extended to include them.
  void RunStringCtor(string* p, size_t n) override {}
  void RunStringDtor(string* p, size_t n) override {}
  void RunResourceCtor(ResourceHandle* p, size_t n) override {}
  void RunResourceDtor(ResourceHandle* p, size_t n) override {}
};

XlaCompilationDevice::XlaCompilationDevice(const SessionOptions& options,
                                           DeviceType type)
    : LocalDevice(
          options,
          Device::BuildDeviceAttributes(
              "", type, Bytes(256 << 20), DeviceLocality(),
              strings::StrCat("device: XLA compilation device ", type.type()))),
      allocator_(new XlaCompilationAllocator()) {}

XlaCompilationDevice::~XlaCompilationDevice() {}

Allocator* XlaCompilationDevice::GetAllocator(AllocatorAttributes attr) {
  return allocator_.get();
}

void XlaCompilationDevice::Compute(OpKernel* op_kernel,
                                   OpKernelContext* context) {
  VLOG(4) << "XlaCompilationDevice::Compute "
          << SummarizeNodeDef(op_kernel->def());
  auto* b = XlaContext::Get(context).builder();
  xla::OpMetadata metadata;
  metadata.set_op_type(op_kernel->type_string());
  metadata.set_op_name(op_kernel->name());
  b->SetOpMetadata(metadata);

  DeviceNameUtils::ParsedName parsed;
  /* FIXME: For some reason, this fails during tfcompile:
   * 2017-11-14 17:24:17.169663: F external/org_tensorflow/tensorflow/compiler/aot/tfcompile_main.cc:140] Non-OK-status: status status: Internal: Unable to parse device name: XLA_CPU_JIT
   *
  OP_REQUIRES(
      context,
      DeviceNameUtils::ParseFullName(op_kernel->requested_device(), &parsed),
      errors::Internal("Unable to parse device name: ",
                       op_kernel->requested_device()));
  */
  xla::OpDeviceAssignment assignment;
  // If no device ID assignment is found, XLA is free to use whatever device it
  // wants. In practice this usually has the effect of placing things on
  // device 0.
  if (parsed.has_id) {
    assignment.set_has_device(true);
    assignment.set_device(parsed.id);
  }
  b->SetDeviceAssignment(assignment);

  op_kernel->Compute(context);

  b->ClearOpMetadata();
  b->ClearDeviceAssignment();
  VLOG(4) << "Done";
}

Status XlaCompilationDevice::Sync() { return Status::OK(); }

Status XlaCompilationDevice::MakeTensorFromProto(
    const TensorProto& tensor_proto, const AllocatorAttributes alloc_attrs,
    Tensor* tensor) {
  return errors::InvalidArgument(
      "XLACompilationDevice::MakeTensorFromProto should not be called");
}

XlaExpression::XlaExpression() = default;

void XlaExpression::set_handle(const xla::ComputationDataHandle& h) {
  handle_ = h;
}

void XlaExpression::set_constant_value(Tensor value) {
  has_constant_value_ = true;
  constant_value_ = std::move(value);
}

Status XlaResource::GetXlaShape(xla::ComputationBuilder* builder,
                                xla::Shape* shape) const {
  auto shape_or_status = builder->GetShape(value);
  if (!shape_or_status.ok()) {
    return shape_or_status.status();
  }
  *shape = *shape_or_status.ValueOrDie();
  return Status::OK();
}

Status XlaResource::GetShape(xla::ComputationBuilder* builder,
                             TensorShape* shape) const {
  xla::Shape xla_shape;
  TF_RETURN_IF_ERROR(GetXlaShape(builder, &xla_shape));
  TF_RETURN_IF_ERROR(XLAShapeToTensorShape(xla_shape, shape));
  return Status::OK();
}

Status XlaResource::GetOrCreateTensorArrayGradient(
    const string& source, xla::ComputationBuilder* builder,
    XlaResource** gradient_out) {
  VLOG(2) << "Gradient lookup for resource: " << name
          << " gradient: " << source;
  TF_RET_CHECK(kind == kTensorArray);
  std::unique_ptr<XlaResource>& gradient = tensor_array_gradients[source];
  if (!gradient) {
    gradient.reset(new XlaResource);
    gradient->kind = XlaResource::kTensorArray;
    gradient->name = strings::StrCat("TensorArrayGrad: ", name);
    gradient->type = type;
    gradient->tensor_array_size = tensor_array_size;

    TensorShape ta_shape;
    TF_RETURN_IF_ERROR(GetShape(builder, &ta_shape));
    gradient->value = builder->Broadcast(XlaHelpers::Zero(builder, type),
                                         ta_shape.dim_sizes());
    gradient->initial_value = gradient->value;
  }
  *gradient_out = gradient.get();
  return Status::OK();
}

Status XlaResource::PackedShape(xla::ComputationBuilder* builder,
                                xla::Shape* packed_shape) const {
  if (tensor_array_gradients.empty()) {
    return GetXlaShape(builder, packed_shape);
  }
  TF_RET_CHECK(kind == kTensorArray);
  std::vector<xla::Shape> elem_shapes(1 + tensor_array_gradients.size());
  int pos = 0;
  TF_RETURN_IF_ERROR(GetXlaShape(builder, &elem_shapes[pos++]));
  for (const auto& gradient : tensor_array_gradients) {
    TF_RETURN_IF_ERROR(
        gradient.second->GetXlaShape(builder, &elem_shapes[pos++]));
  }
  *packed_shape = xla::ShapeUtil::MakeTupleShape(elem_shapes);
  return Status::OK();
}

Status XlaResource::Pack(xla::ComputationDataHandle* pack,
                         xla::ComputationBuilder* builder) const {
  if (tensor_array_gradients.empty()) {
    *pack = value;
  } else {
    TF_RET_CHECK(kind == kTensorArray);
    std::vector<xla::ComputationDataHandle> elems;
    elems.push_back(value);
    for (const auto& gradient : tensor_array_gradients) {
      elems.push_back(gradient.second->value);
    }
    *pack = builder->Tuple(elems);
  }
  return Status::OK();
}

Status XlaResource::SetFromPack(const std::set<string>& gradient_sources,
                                const xla::ComputationDataHandle& pack,
                                xla::ComputationBuilder* builder) {
  if (gradient_sources.empty()) {
    value = pack;
  } else {
    TF_RET_CHECK(kind == kTensorArray);
    int pos = 0;
    value = builder->GetTupleElement(pack, pos++);
    for (const auto& source : gradient_sources) {
      XlaResource* gradient;
      TF_RETURN_IF_ERROR(
          GetOrCreateTensorArrayGradient(source, builder, &gradient));
      gradient->value = builder->GetTupleElement(pack, pos++);
    }
  }
  return Status::OK();
}

}  // namespace tensorflow
