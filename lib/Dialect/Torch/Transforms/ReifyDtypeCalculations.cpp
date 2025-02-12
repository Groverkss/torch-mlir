//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"

#include "ReifyAbstractInterpCalculationsUtils.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Transforms/DialectConversion.h"
#include "torch-mlir/Dialect/Torch/IR/TorchOps.h"
#include "torch-mlir/Dialect/Torch/Transforms/Passes.h"

using namespace mlir;
using namespace mlir::torch;
using namespace mlir::torch::Torch;

static bool isTensorTypeOrWrappedTensorType(Type type) {
  // Allowing tuples as arguments to dtype calculation functions can cause
  // issues. For example, if an argument is a tuple of tensors and ints, there
  // would be no way of differentiating the original ints from the ints created
  // to represent the dtype and rank of the tensors. Therefore, to avoid this
  // and keep things simple, the tuple type is not allowed. This works well in
  // practice, since PyTorch op signatures don't seem to take tuples as inputs.
  assert(!type.isa<Torch::TupleType>() &&
         "dtype calculation functions are expected to not have tuples of "
         "tensors as arguments");

  if (type.isa<Torch::BaseTensorType>())
    return true;

  if (auto optionalType = type.dyn_cast<Torch::OptionalType>()) {
    return isTensorTypeOrWrappedTensorType(optionalType.getContainedType());
  } else if (auto listType = type.dyn_cast<Torch::ListType>()) {
    return isTensorTypeOrWrappedTensorType(listType.getContainedType());
  } else {
    return false;
  }
}

// Massage the op operands to match the dtype function signature.
// The dtype function generally takes the same operands as the op, with a few
// systematic modifications, such as replacing tensors with a rank and dtype
// argument.
static FailureOr<SmallVector<Value>>
dtypeFunctionArgsBuilder(OpBuilder &b, Location loc,
                         ValueRange originalOperands, func::FuncOp dtypeFunc) {
  // Turns a tensor operand into an operand representing the rank of the tensor
  auto rankArgAdjuster = [](OpBuilder &b, Location loc, Value operand,
                            Type desiredType) -> Value {
    if (desiredType.isa<Torch::IntType>() &&
        operand.getType().isa<Torch::BaseTensorType>()) {
      auto sizeListType =
          Torch::ListType::get(Torch::IntType::get(b.getContext()));
      Value size = b.create<AtenSizeOp>(loc, sizeListType, operand);
      return b.create<AtenLenTOp>(loc, desiredType, size);
    }
    return operand;
  };

  // Turns a tensor operand into an operand representing the dtype of the tensor
  auto dtypeArgAdjuster = [](OpBuilder &b, Location loc, Value operand,
                             Type desiredType) -> Value {
    if (desiredType.isa<Torch::IntType>() &&
        operand.getType().isa<Torch::BaseTensorType>()) {
      return b.create<PrimDtypeOp>(loc, desiredType, operand);
    }
    return operand;
  };

  SmallVector<Value> dtypeFuncArgs;
  ArrayRef<Type> desiredTypes = dtypeFunc.getArgumentTypes();
  for (auto operand : originalOperands) {
    assert(!desiredTypes.empty() &&
           "`dtypeFunc` should have at least one argument for each argument in "
           "`originalOperands`");
    Type desiredType = desiredTypes.front();
    if (isTensorTypeOrWrappedTensorType(operand.getType())) {
      assert(desiredTypes.size() >= 2 &&
             "`dtypeFunc` should have two arguments for each tensor argument "
             "in `originalOperands`");
      FailureOr<Value> rankArg, dtypeArg;
      if (failed(rankArg = adjustFunctionArg(b, loc, operand, desiredType,
                                             rankArgAdjuster)))
        return failure();
      desiredTypes = desiredTypes.drop_front();
      desiredType = desiredTypes.front();
      if (failed(dtypeArg = adjustFunctionArg(b, loc, operand, desiredType,
                                              dtypeArgAdjuster)))
        return failure();
      dtypeFuncArgs.append({*rankArg, *dtypeArg});
    } else {
      FailureOr<Value> otherArg;
      if (failed(otherArg = adjustFunctionArg(b, loc, operand, desiredType)))
        return failure();
      dtypeFuncArgs.push_back(*otherArg);
    }
    desiredTypes = desiredTypes.drop_front();
  }

  return dtypeFuncArgs;
}

namespace {
class ReifyDtypeCalculationsPass
    : public ReifyDtypeCalculationsBase<ReifyDtypeCalculationsPass> {
  void runOnOperation() override {
    MLIRContext *context = &getContext();
    ModuleOp module = getOperation();
    OwningOpRef<ModuleOp> library =
        parseSourceString<ModuleOp>(getAbstractInterpLibrary(), context);

    // Walk all the operations, and if we have a dtype function, wrap the op
    // in a `torch.dtype.calculate` op.
    SmallVector<std::string> functionsNeeded;
    WalkResult walkResult = module.walk([&](Operation *op) -> WalkResult {
      return wrapWithCalculateOpIfLibraryFunctionAvailable(
          op, *library, LibraryFunctionKind::DtypeFunction, functionsNeeded,
          dtypeFunctionArgsBuilder);
    });

    if (walkResult.wasInterrupted())
      return signalPassFailure();
    importLibraryFunctions(module, *library, std::move(functionsNeeded));
  }
};
} // namespace

std::unique_ptr<OperationPass<ModuleOp>>
Torch::createReifyDtypeCalculationsPass() {
  return std::make_unique<ReifyDtypeCalculationsPass>();
}
