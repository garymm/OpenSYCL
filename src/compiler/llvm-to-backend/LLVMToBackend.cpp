/*
 * This file is part of AdaptiveCpp, an implementation of SYCL and C++ standard
 * parallelism for CPUs and GPUs.
 *
 * Copyright The AdaptiveCpp Contributors
 *
 * AdaptiveCpp is released under the BSD 2-Clause "Simplified" License.
 * See file LICENSE in the project root for full license details.
 */
// SPDX-License-Identifier: BSD-2-Clause
#include "hipSYCL/common/debug.hpp"
#include "hipSYCL/compiler/llvm-to-backend/AddressSpaceInferencePass.hpp"
#include "hipSYCL/compiler/llvm-to-backend/DeadArgumentEliminationPass.hpp"
#include "hipSYCL/compiler/llvm-to-backend/GlobalSizesFitInI32OptPass.hpp"
#include "hipSYCL/compiler/llvm-to-backend/GlobalInliningAttributorPass.hpp"
#include "hipSYCL/compiler/llvm-to-backend/KnownGroupSizeOptPass.hpp"
#include "hipSYCL/compiler/llvm-to-backend/LLVMToBackend.hpp"
#include "hipSYCL/compiler/llvm-to-backend/KnownPtrParamAlignmentOptPass.hpp"
#include "hipSYCL/compiler/llvm-to-backend/ProcessS2ReflectionPass.hpp"
#include "hipSYCL/compiler/llvm-to-backend/Utils.hpp"
#include "hipSYCL/compiler/sscp/IRConstantReplacer.hpp"
#include "hipSYCL/compiler/sscp/KernelOutliningPass.hpp"
#include "hipSYCL/compiler/utils/ProcessFunctionAnnotationsPass.hpp"
#include "hipSYCL/compiler/utils/LLVMUtils.hpp"
#include "hipSYCL/glue/llvm-sscp/jit-reflection/queries.hpp"
#include "hipSYCL/sycl/access.hpp"

#include <cstdint>

#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/ADT/APFloat.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <string>
#include <optional>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

namespace hipsycl {
namespace compiler {

namespace {

template<class T>
std::optional<T> getEnvironmentVariable(const std::string& Name) {
  std::string EnvName = Name;
  std::transform(EnvName.begin(), EnvName.end(), EnvName.begin(), ::toupper);

  if(const char* EnvVal = std::getenv(("ACPP_S2_"+EnvName).c_str())) {
    T val;
    std::stringstream sstr{std::string{EnvVal}};
    sstr >> val;
    if (!sstr.fail() && !sstr.bad()) {
      return val;
    }
  }
  return {};
}

template<class T>
T getEnvironmentVariableOrDefault(const std::string& Name,
                                      const T& Default) {
  std::optional<T> v = getEnvironmentVariable<T>(Name);
  if(v.has_value()) {
    return v.value();
  }
  return Default;
}

void printModuleToFile(llvm::Module& M, const std::string& File,
                      const std::string& Header){

  // Desired behavior is to truncate files for each application run,
  // but append content in the dump file within one application run.
  static std::unordered_set<std::string> UsedFiles;
  auto OpenFlag = llvm::sys::fs::OpenFlags::OF_Append;
  if(UsedFiles.find(File) == UsedFiles.end()) {
    OpenFlag = llvm::sys::fs::OpenFlags::OF_None;
    UsedFiles.insert(File);
  }

  std::error_code EC;
  llvm::raw_fd_ostream Out{File, EC, OpenFlag};
  Out << ";---------------- Begin AdaptiveCpp IR dump --------------\n";
  Out << Header;
  M.print(Out, nullptr);
  Out << ";----------------- End AdaptiveCpp IR dump ---------------\n";
}

void enableModuleStateDumping(llvm::Module &M, const std::string &PipelineStage,
                              const std::string &Kernels) {
  std::string Filter =
      getEnvironmentVariableOrDefault<std::string>("DUMP_IR_FILTER", "");

  std::string FallbackFileName = M.getSourceFileName()+".ll";
  std::string FileName =
      getEnvironmentVariableOrDefault<std::string>("DUMP_IR_" + PipelineStage, "");

  if(FileName == "1")
    FileName = FallbackFileName;
  
  std::string Header =
      "; AdaptiveCpp SSCP S2 IR dump; Compiling kernels: " + Kernels + ", stage: " + PipelineStage + "\n";

  if(FileName.length() != 0) {
    if(Kernels == Filter || Filter.empty())
      printModuleToFile(M, FileName, Header);
  }

  std::string AllFileName =
      getEnvironmentVariableOrDefault<std::string>("DUMP_IR_ALL", "");
  if(AllFileName == "1")
    AllFileName = FallbackFileName;

  if(AllFileName.length() != 0 && AllFileName != FileName) {
    if(Kernels == Filter || Filter.empty())
      printModuleToFile(M, AllFileName, Header);
  }
}

bool linkBitcode(llvm::Module &M, std::unique_ptr<llvm::Module> OtherM,
                   const std::string &ForcedTriple = "",
                   const std::string &ForcedDataLayout = "",
                   llvm::Linker::Flags Flags = llvm::Linker::Flags::LinkOnlyNeeded) {
  if(!ForcedTriple.empty())
    OtherM->setTargetTriple(ForcedTriple);
  if(!ForcedDataLayout.empty())
    OtherM->setDataLayout(ForcedDataLayout);

  // Returns true on error
  if (llvm::Linker::linkModules(M, std::move(OtherM), Flags)) {
    return false;
  }
  return true;
}

void setFastMathFunctionAttribs(llvm::Module& M) {
  auto forceAttr = [&](llvm::Function& F, llvm::StringRef Key, llvm::StringRef Value) {
    if(F.hasFnAttribute(Key)) {
      if(F.getFnAttribute(Key).getValueAsString() != Value)
        F.removeFnAttr(Key);
    }
    F.addFnAttr(Key, Value);
  };

  for(auto& F : M) {
    if(!F.isIntrinsic()) {
      forceAttr(F, "approx-func-fp-math","true");
      forceAttr(F, "denormal-fp-math","preserve-sign,preserve-sign");
      forceAttr(F, "no-infs-fp-math","true");
      forceAttr(F, "no-nans-fp-math","true");
      forceAttr(F, "no-signed-zeros-fp-math","true");
      forceAttr(F, "no-trapping-math","true");
      forceAttr(F, "unsafe-fp-math","true");
    }
  }
}


class InstructionCleanupPass : public llvm::PassInfoMixin<InstructionCleanupPass> {
public:

  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM) {
    
    llvm::SmallVector<llvm::CallBase*> CallsToRemove;
    for(auto& F : M) {
      for(auto& BB : F) {
        for(auto& I : BB) {
          if(llvm::CallBase* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
            // these instructions can sometimes appear as a byproduct of some transformations
            // even without dynamic allocas, but they are generally unsupported on device
            // backends.
            if (llvmutils::starts_with(CB->getCalledFunction()->getName(), "llvm.stacksave") ||
                llvmutils::starts_with(CB->getCalledFunction()->getName(), "llvm.stackrestore"))
              CallsToRemove.push_back(CB);
          }
        }
      }
    }

    for(auto* C : CallsToRemove) {
      C->replaceAllUsesWith(llvm::UndefValue::get(C->getType()));
      C->eraseFromParent();
    }
    return llvm::PreservedAnalyses::none();
  }
};


}

LLVMToBackendTranslator::LLVMToBackendTranslator(int S2IRConstantCurrentBackendId,
                                                 const std::vector<std::string> &OutliningEPs,
                                                 const std::vector<std::string> &KernelNames)
    : S2IRConstantBackendId(S2IRConstantCurrentBackendId),
      OutliningEntrypoints{OutliningEPs}, Kernels{KernelNames} {}

bool LLVMToBackendTranslator::setBuildFlag(const std::string &Flag) {
  HIPSYCL_DEBUG_INFO << "LLVMToBackend: Using build flag: " << Flag << "\n";

  if(Flag == "global-sizes-fit-in-int") {
    GlobalSizesFitInInt = true;
    return true;
  } else if(Flag == "fast-math") {
    IsFastMath = true;
    return true;
  }

  return applyBuildFlag(Flag);
}

bool LLVMToBackendTranslator::setBuildOption(const std::string &Option, const std::string &Value) {
  HIPSYCL_DEBUG_INFO << "LLVMToBackend: Using build option: " << Option << "=" << Value << "\n";

  if(Option == "known-group-size-x") {
    KnownGroupSizeX = std::stoi(Value);
    return true;
  } else if (Option == "known-group-size-y") {
    KnownGroupSizeY = std::stoi(Value);
    return true;
  } else if (Option == "known-group-size-z") {
    KnownGroupSizeZ = std::stoi(Value);
    return true;
  } else if (Option == "known-local-mem-size") {
    KnownLocalMemSize = std::stoi(Value);
  }

  return applyBuildOption(Option, Value);
}
bool LLVMToBackendTranslator::setBuildToolArguments(const std::string &ToolName,
                                    const std::vector<std::string> &Args) {
  HIPSYCL_DEBUG_INFO << "LLVMToBackend: Using tool arguments for tool " << ToolName << ":\n";
  for(const auto& A : Args) {
    HIPSYCL_DEBUG_INFO << "   " << A << "\n";
  }
  return applyBuildToolArguments(ToolName, Args);
}

bool LLVMToBackendTranslator::partialTransformation(const std::string &LLVMIR, std::string &Out) {
  llvm::LLVMContext ctx;
  std::unique_ptr<llvm::Module> M;
  auto err = loadModuleFromString(LLVMIR, ctx, M);

  if (err) {
    this->registerError("LLVMToBackend: Could not load LLVM module");
    llvm::handleAllErrors(std::move(err), [&](llvm::ErrorInfoBase &EIB) {
      this->registerError(EIB.message());
    });
    return false;
  }

  assert(M);
  if (!prepareIR(*M)) {
    setFailedIR(*M);
    return false;
  }

  llvm::raw_string_ostream OutputStream{Out};
  llvm::WriteBitcodeToFile(*M, OutputStream);

  return true;
}

bool LLVMToBackendTranslator::fullTransformation(const std::string &LLVMIR, std::string &out) {
  llvm::LLVMContext ctx;
  std::unique_ptr<llvm::Module> M;
  auto err = loadModuleFromString(LLVMIR, ctx, M);

  if (err) {
    this->registerError("LLVMToBackend: Could not load LLVM module");
    llvm::handleAllErrors(std::move(err), [&](llvm::ErrorInfoBase &EIB) {
      this->registerError(EIB.message());
    });
    return false;
  }

  assert(M);
  if (!prepareIR(*M)) {
    setFailedIR(*M);
    return false;
  }
  if (!translatePreparedIR(*M, out)) {
    setFailedIR(*M);
    return false;
  }

  return true;
}

bool LLVMToBackendTranslator::prepareIR(llvm::Module &M) {
  enableModuleStateDumping(M, "input", getCompilationIdentifier());

  HIPSYCL_DEBUG_INFO << "LLVMToBackend: Preparing backend flavoring...\n";

  return withPassBuilderAndMAM([&](llvm::PassBuilder &PB, llvm::ModuleAnalysisManager &MAM) {
    PassHandler PH {&PB, &MAM};

    // Do an initial outlining to simplify the code, particularly to reduce
    // linking complexity if --acpp-export-all is used
    HIPSYCL_DEBUG_INFO << "LLVMToBackend: Reoutlining kernels...\n";
    // Function call specializations are only handled at a later stage,
    // so if the user has requested any, ensure that we don't throw them away
    // since these functions will not yet appear in the call graph.
    std::vector<std::string> InitialOutliningEntrypoints = OutliningEntrypoints;
    for(const auto& FName : FunctionCallSpecializationOutliningEntrypoints)
      InitialOutliningEntrypoints.push_back(FName);
    KernelOutliningPass InitialOutlining{InitialOutliningEntrypoints};
    InitialOutlining.run(M, MAM);
    enableModuleStateDumping(M, "initial_outlining", getCompilationIdentifier());
    // We need to resolve symbols now instead of after optimization, because we
    // may have to reoutline if the code that is linked in after symbol resolution
    // depends on IR constants.
    // This also means that we cannot error yet if we cannot resolve all symbols :(
    resolveExternalSymbols(M);

    if(!this->prepareBackendFlavor(M))
      return false;

    HIPSYCL_DEBUG_INFO << "LLVMToBackend: Applying specializations and S2 IR constants...\n";
    for(auto& A : SpecializationApplicators) {
      HIPSYCL_DEBUG_INFO << "LLVMToBackend: Processing specialization " << A.first << "\n";
      A.second(M);
    }
    // Return error in case applying specializations has caused error list to be populated
    if(!Errors.empty())
      return false;
    
    enableModuleStateDumping(M, "specialization", getCompilationIdentifier());

    // Process stage 2 reflection calls
    ReflectionFields["compiler_backend"] = this->getBackendId();
    for(const auto& Fields : ReflectionFields) {
      HIPSYCL_DEBUG_INFO << "LLVMToBackend: Setting up reflection fields: " << Fields.first << " = "
                         << Fields.second << "\n";
    }
    ProcessS2ReflectionPass S2RP{ReflectionFields};
    S2RP.run(M, MAM);

    enableModuleStateDumping(M, "reflection", getCompilationIdentifier());

    // Optimize away unnecessary branches due to backend-specific S2IR constants
    // This is what allows us to specialize code for different backends.
    HIPSYCL_DEBUG_INFO << "LLVMToBackend: Optimizing branches post S2 IR constant application...\n";
    IRConstant::optimizeCodeAfterConstantModification(M, MAM);

    // Rerun kernel outlining pass so that we don't include unneeded functions
    // that are specific to other backends.
    HIPSYCL_DEBUG_INFO << "LLVMToBackend: Reoutlining kernels...\n";
    KernelOutliningPass KP{OutliningEntrypoints};
    KP.run(M, MAM);

    for(auto& P : NoAliasParameters) {
      auto* F = M.getFunction(P.first);
      if(F) {
        for(int i : P.second) {
          HIPSYCL_DEBUG_INFO << "LLVMToBackend: Attaching noalias attribute to parameter " << i
                             << " of kernel " << P.first << "\n";
          if(i < F->getFunctionType()->getNumParams())
            if(!F->hasParamAttribute(i, llvm::Attribute::AttrKind::NoAlias))
              F->addParamAttr(i, llvm::Attribute::AttrKind::NoAlias);
        }
      }
    }

    // These optimizations should be run before __acpp_sscp_* builtins
    // are resolved, so before backend bitcode libraries are linked. We thus
    // run them prior to flavoring.
    KnownGroupSizeOptPass GroupSizeOptPass{KnownGroupSizeX, KnownGroupSizeY, KnownGroupSizeZ};
    GlobalSizesFitInI32OptPass SizesAsIntOptPass{GlobalSizesFitInInt, KnownGroupSizeX,
                                                 KnownGroupSizeY, KnownGroupSizeZ};

    GroupSizeOptPass.run(M, MAM);
    SizesAsIntOptPass.run(M, MAM);

    KnownPtrParamAlignmentOptPass KnownAlignmentOptPass{KnownPtrParamAlignments};
    KnownAlignmentOptPass.run(M, MAM);

    // Before optimizing, make sure everything has internal linkage to
    // help inlining. All linking should have occured by now, except
    // for backend builtin libraries like libdevice etc

    // First inling stage is prior to backend flavoring. This helps
    // for some backends which introduces call conventions that complicate inlining
    // (e.g. spir_func)
    GlobalInliningAttributorPass InliningPass{Kernels};
    InliningPass.run(M, MAM);
    MAM.clear();
    llvm::AlwaysInlinerPass AIP;
    AIP.run(M, MAM);

    InstructionCleanupPass ICP;
    ICP.run(M, MAM);

    enableModuleStateDumping(M, "jit_optimizations", getCompilationIdentifier());

    HIPSYCL_DEBUG_INFO << "LLVMToBackend: Adding backend-specific flavor to IR...\n";
    if(!this->toBackendFlavor(M, PH)) {
      HIPSYCL_DEBUG_INFO << "LLVMToBackend: Flavoring failed\n";
      return false;
    }

    enableModuleStateDumping(M, "backend_flavoring", getCompilationIdentifier());
    // Run again to resolve reflection inside builtins
    S2RP.run(M, MAM);
    enableModuleStateDumping(M, "builtin_reflection", getCompilationIdentifier());

    // Inline again to handle builtin definitions pulled in by backend flavors
    InliningPass.run(M, MAM);

    // Run optimizations
    HIPSYCL_DEBUG_INFO << "LLVMToBackend: Optimizing flavored IR...\n";

    if(IsFastMath)
      setFastMathFunctionAttribs(M);

    // Remove argument_used hints, which are no longer needed once we enter optimization stage.
    // This is primarily needed for dynamic functions.
    utils::ProcessFunctionAnnotationPass PFA({"argument_used"});
    PFA.run(M, MAM);

    MAM.clear(); 

    if(!optimizeFlavoredIR(M, PH)) {
      this->registerError("LLVMToBackend: Optimization failed");
      return false;
    }

    for(const auto& Entry : FunctionsForDeadArgumentElimination) {
      if(auto* F = M.getFunction(Entry.first)) {
        if(isKernelAfterFlavoring(*F)) {
          runKernelDeadArgumentElimination(M, F, PH, *Entry.second);
        }
      }
    }
    llvm::AlwaysInlinerPass{}.run(M, MAM);

    enableModuleStateDumping(M, "full_optimizations", getCompilationIdentifier());
    
    enableModuleStateDumping(M, "final", getCompilationIdentifier());

    bool ContainsUnsetIRConstants = false;
    S2IRConstant::forEachS2IRConstant(M, [&](S2IRConstant C) {
      if (C.isValid()) {
        if (!C.isInitialized()) {
          ContainsUnsetIRConstants = true;
          this->registerError("LLVMToBackend: AdaptiveCpp S2IR constant was not set: " +
                              C.getGlobalVariable()->getName().str());
        }
      }
    });
    if(ContainsUnsetIRConstants)
      return false;

    return true;
  });
}

bool LLVMToBackendTranslator::translatePreparedIR(llvm::Module &FlavoredModule, std::string &out) {
  HIPSYCL_DEBUG_INFO << "LLVMToBackend: Invoking translation to backend-specific format\n";
  return this->translateToBackendFormat(FlavoredModule, out);
}

bool LLVMToBackendTranslator::optimizeFlavoredIR(llvm::Module& M, PassHandler& PH) {
  assert(PH.PassBuilder);
  assert(PH.ModuleAnalysisManager);

  // silence optimization remarks,..
  M.getContext().setDiagnosticHandlerCallBack(
#if LLVM_VERSION_MAJOR >= 19
      [](const llvm::DiagnosticInfo *DI, void *Context) {
        llvm::DiagnosticPrinterRawOStream DP(llvm::errs());
        if (DI->getSeverity() == llvm::DS_Error) {
          llvm::errs() << "LLVMToBackend: Error: ";
          DI->print(DP);
          llvm::errs() << "\n";
        }
      });
#else
      [](const llvm::DiagnosticInfo &DI, void *Context) {
        llvm::DiagnosticPrinterRawOStream DP(llvm::errs());
        if (DI.getSeverity() == llvm::DS_Error) {
          llvm::errs() << "LLVMToBackend: Error: ";
          DI.print(DP);
          llvm::errs() << "\n";
        }
      });
#endif

  llvm::ModulePassManager MPM =
      PH.PassBuilder->buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  MPM.run(M, *PH.ModuleAnalysisManager);

  return true;
}

bool LLVMToBackendTranslator::linkBitcodeString(llvm::Module &M, const std::string &Bitcode,
                                                const std::string &ForcedTriple,
                                                const std::string &ForcedDataLayout,
                                                bool LinkOnlyNeeded) {
  std::unique_ptr<llvm::Module> OtherModule;
  auto err = loadModuleFromString(Bitcode, M.getContext(), OtherModule);

  if (err) {
    this->registerError("LLVMToBackend: Could not load LLVM module");
    llvm::handleAllErrors(std::move(err), [&](llvm::ErrorInfoBase &EIB) {
      this->registerError(EIB.message());
    });
    return false;
  }

  llvm::Linker::Flags F = llvm::Linker::None;
  if(LinkOnlyNeeded)
    F = llvm::Linker::LinkOnlyNeeded;

  if(!linkBitcode(M, std::move(OtherModule), ForcedTriple, ForcedDataLayout, F)) {
    this->registerError("LLVMToBackend: Linking module failed");
    return false;
  }

  return true;
}

bool LLVMToBackendTranslator::linkBitcodeFile(llvm::Module &M, const std::string &BitcodeFile,
                                              const std::string &ForcedTriple,
                                              const std::string &ForcedDataLayout,
                                              bool LinkOnlyNeeded) {
  auto F = llvm::MemoryBuffer::getFile(BitcodeFile);
  if(auto Err = F.getError()) {
    this->registerError("LLVMToBackend: Could not open file " + BitcodeFile);
    return false;
  }
  HIPSYCL_DEBUG_INFO << "LLVMToBackend: Linking with bitcode file: " << BitcodeFile << "\n";
  return linkBitcodeString(M, std::string{F.get()->getBuffer()}, ForcedTriple, ForcedDataLayout,
                           LinkOnlyNeeded);
}

void LLVMToBackendTranslator::specializeKernelArgument(const std::string &KernelName, int ParamIndex,
                                const void *ValueBuffer) {
  std::string Id = KernelName+"__specialized_kernel_argument_"+std::to_string(ParamIndex);
  SpecializationApplicators[Id] = [=](llvm::Module& M) {
    if(auto* F = M.getFunction(KernelName)) {
      if(F->getFunctionType()->getNumParams() > ParamIndex && !F->isDeclaration()) {
        
        llvm::Type* ParamType = F->getFunctionType()->getParamType(ParamIndex);
        if (ParamType->isIntegerTy() || ParamType->isPointerTy() || ParamType->isFloatTy() ||
            ParamType->isDoubleTy()) {
          std::string GetterName = "__specialization_getter_"+Id;
          llvm::Function *GetConstant = llvm::dyn_cast<llvm::Function>(
              M.getOrInsertFunction(GetterName, ParamType).getCallee());
          
          if(!GetConstant)
            return;
          GetConstant->addFnAttr(llvm::Attribute::AlwaysInline);

          llvm::Constant *ReturnedValue = nullptr;
          std::size_t ParamByteSize = M.getDataLayout().getTypeSizeInBits(ParamType) / CHAR_BIT;

          if(ParamType->isIntegerTy()) {
            uint64_t Value = 0;
            std::memcpy(&Value, ValueBuffer, ParamByteSize);
            ReturnedValue = llvm::ConstantInt::get(
              M.getContext(), llvm::APInt(ParamType->getIntegerBitWidth(), Value));
          } else if(ParamType->isFloatTy()) {
            float Value = 0.0f;
            std::memcpy(&Value, ValueBuffer, ParamByteSize);
            ReturnedValue = llvm::ConstantFP::get(M.getContext(), llvm::APFloat(Value));
          } else if(ParamType->isDoubleTy()) {
            double Value = 0.0;
            std::memcpy(&Value, ValueBuffer, ParamByteSize);
            ReturnedValue = llvm::ConstantFP::get(M.getContext(), llvm::APFloat(Value));
          } else if(ParamType->isPointerTy()) {
            uint64_t Value = 0;
            std::memcpy(&Value, ValueBuffer, ParamByteSize);
            auto* IntPtr = llvm::ConstantInt::get(
              M.getContext(), llvm::APInt(ParamByteSize * CHAR_BIT, Value));
            ReturnedValue = llvm::ConstantExpr::getIntToPtr(
                IntPtr, ParamType);
          }
          if(!ReturnedValue) {
            HIPSYCL_DEBUG_WARNING << "LLVMToBackend: Could not specialize kernel argument " << Id
                                  << " due to unsupported parameter type\n";
            return;
          }

          llvm::BasicBlock *BB =
              llvm::BasicBlock::Create(M.getContext(), "", GetConstant);

          llvm::ReturnInst::Create(M.getContext(), ReturnedValue, BB);

          llvm::Instruction* InsertionPt = &(*F->getEntryBlock().getFirstInsertionPt());
          auto* FnCall = llvm::CallInst::Create(llvm::FunctionCallee(GetConstant),
                                  llvm::ArrayRef<llvm::Value *>{}, "", InsertionPt);
          F->getArg(ParamIndex)->replaceNonMetadataUsesWith(FnCall);
        }
      }
    }
  };
}

void LLVMToBackendTranslator::specializeFunctionCalls(
    const std::string &FuncName, const std::vector<std::string> &ReplacementCalls,
    bool OverrideOnlyUndefined) {

  for(const auto& FName : ReplacementCalls) {
    this->FunctionCallSpecializationOutliningEntrypoints.push_back(FName);
  }

  std::string Id = "__specialized_function_call_"+FuncName;
  SpecializationApplicators[Id] = [=](llvm::Module &M) {
    HIPSYCL_DEBUG_INFO << "LLVMToBackend: Specializing function calls to " << FuncName << " to:\n";
    for(const auto& s : ReplacementCalls)
      HIPSYCL_DEBUG_INFO << "LLVMToBackend:   " << s << "\n";
    if(auto* F = M.getFunction(FuncName)) {
      if((!OverrideOnlyUndefined || F->isDeclaration()) && !ReplacementCalls.empty()) {
        llvm::Value* ReplacementValue;
        if(ReplacementCalls.size() == 1){
          llvm::Function* ReplacementF = M.getFunction(ReplacementCalls[0]);
          ReplacementValue = ReplacementF;

          if(!ReplacementValue) {
            registerError("LLVMToBackend: Could not find function call specialization target " +
                        ReplacementCalls[0] + ", was the function emitted to device code?");
            return;
          }
          if(ReplacementF->getFunctionType() != F->getFunctionType()) {
            registerError("LLVMToBackend: Specialization function " + ReplacementCalls[0] +
                          " has incompatible type for specialization of " + FuncName);
            return;
          }
        } else {
          llvm::SmallVector<llvm::Function*, 16> ReplacementFs;

          if (!F->getReturnType()->isVoidTy()) {
            registerError("LLVMToBackend: Specialization of function calls using a function call "
                          "list is only possible if the original and all replacement functions "
                          "have void return type.");
            return;
          }
          for(const auto& FName : ReplacementCalls) {
            auto* RetrievedF = M.getFunction(FName);
            if(!RetrievedF) {
              registerError("LLVMToBackend: Could not find function call specialization target " +
                            FName + ", was the function emitted to device code?");
              return;
            }
            if(RetrievedF->getFunctionType() != F->getFunctionType()) {
              registerError("LLVMToBackend: Specialization function " + FName +
                          " has incompatible type for specialization of " + FuncName);
              return;
            }
            ReplacementFs.push_back(RetrievedF);
          }
          auto ReplacementWrapperFuncCallee = M.getOrInsertFunction(Id, F->getFunctionType(), F->getAttributes());
          if (auto *ReplacementWrapperF =
                  static_cast<llvm::Function *>(ReplacementWrapperFuncCallee.getCallee())) {
            auto BB = llvm::BasicBlock::Create(M.getContext(), "entry",
                                               ReplacementWrapperF);
            for(auto* F : ReplacementFs) {
              llvm::SmallVector<llvm::Value*> Args;
              for(int i = 0; i < F->getFunctionType()->getNumParams(); ++i)
                Args.push_back(ReplacementWrapperF->getArg(i));

              llvm::CallInst::Create(llvm::FunctionCallee{F},
                                     llvm::ArrayRef<llvm::Value *>{Args}, "", BB);
            }                                            
            llvm::ReturnInst::Create(M.getContext(), BB);
          }
          ReplacementValue  = ReplacementWrapperFuncCallee.getCallee();
        }

        F->replaceUsesWithIf(ReplacementValue, [=](llvm::Use& U) {
          return llvm::isa<llvm::CallBase>(U.getUser());
        });
      }
    }
  };
}

void LLVMToBackendTranslator::setNoAliasKernelParam(const std::string &KernelName, int ParamIndex) {
  NoAliasParameters[KernelName].push_back(ParamIndex);
}

void LLVMToBackendTranslator::provideExternalSymbolResolver(ExternalSymbolResolver Resolver) {
  this->SymbolResolver = Resolver;
  this->HasExternalSymbolResolver = true;
}

void LLVMToBackendTranslator::resolveExternalSymbols(llvm::Module& M) {

  if(HasExternalSymbolResolver) {

    // TODO We can not rely on LinkedIRIds being reliable, since
    // we only link needed symbols. Therefore, just because we have linked one module once
    // we may have to do it again.
    llvm::SmallSet<std::string, 32> AllAttemptedSymbolResolutions;
    llvm::SmallSet<std::string, 16> UnresolvedSymbolsSet;

    // Find out which unresolved symbols are in this IR
    for(auto SymbolName : SymbolResolver.getImportedSymbols()) {
      HIPSYCL_DEBUG_INFO << "LLVMToBackend: Attempting to resolve primary symbol " << SymbolName
                         << "\n";
      UnresolvedSymbolsSet.insert(SymbolName);
    }

    for(;;) {
      std::vector<std::string> Symbols;
      for(auto S : UnresolvedSymbolsSet) {
        Symbols.push_back(S);
        AllAttemptedSymbolResolutions.insert(S);
      }

      std::vector<ExternalSymbolResolver::LLVMModuleId> IRs = SymbolResolver.mapSymbolsToModuleIds(Symbols);
      HIPSYCL_DEBUG_INFO << "LLVMToBackend: Attempting to link against " << IRs.size()
                        << " external bitcode modules to resolve " << UnresolvedSymbolsSet.size()
                        << " symbols\n";

      // It can happen that the IR we have just linked needs new, external
      // symbol definitions to work. So we need to try to resolve the new
      // stuff in the next iteration.
      llvm::SmallSet<std::string, 16> NewUnresolvedSymbolsSet;

      for(const auto& IRID : IRs) {

        SymbolListType NewUndefinedSymbolsFromIR;

        if (!this->linkBitcodeString(
                M, SymbolResolver.retrieveBitcode(IRID, NewUndefinedSymbolsFromIR))) {
          HIPSYCL_DEBUG_WARNING
              << "LLVMToBackend: Linking against bitcode to resolve symbols failed\n";
        }

        for(const auto& S : NewUndefinedSymbolsFromIR) {
          if(!AllAttemptedSymbolResolutions.contains(S)) {
            NewUnresolvedSymbolsSet.insert(S);
            HIPSYCL_DEBUG_INFO << "LLVMToBackend: Attemping resolve symbol " << S
                                << " as a dependency\n";
          }
        }

      }

      if(NewUnresolvedSymbolsSet.empty()) {
        return;
      }

      UnresolvedSymbolsSet = NewUnresolvedSymbolsSet;
    }
  }
}

void LLVMToBackendTranslator::enableDeadArgumentElminiation(
    const std::string &FunctionName, std::vector<int> *RetainedArgumentIndices) {
  this->FunctionsForDeadArgumentElimination.push_back(
      std::make_pair(FunctionName, RetainedArgumentIndices));
}

const std::vector<std::pair<std::string, std::vector<int> *>> &
LLVMToBackendTranslator::getDeadArgumentEliminationConfig() const {
  return FunctionsForDeadArgumentElimination;
}

void LLVMToBackendTranslator::setFailedIR(llvm::Module& M) {
  llvm::raw_string_ostream Stream{ErroringCode};
  llvm::WriteBitcodeToFile(M, Stream);
}

void LLVMToBackendTranslator::runKernelDeadArgumentElimination(
    llvm::Module &M, llvm::Function *F, PassHandler &PH, std::vector<int> &RetainedIndicesOut) {
  std::string FName = F->getName().str();

  llvm::SmallVector<int> RetainedArgumentIndices;
  std::function<void(llvm::Function *, llvm::Function *)> KernelMigrationHandler =
      [&, this](llvm::Function *Old, llvm::Function *New) {
        this->migrateKernelProperties(Old, New);
      };
  DeadArgumentEliminationPass DAE{F, &RetainedArgumentIndices, &KernelMigrationHandler};
  DAE.run(M, *PH.ModuleAnalysisManager);

  auto *DAEOutput = &RetainedIndicesOut;
  if (DAEOutput) {
    DAEOutput->resize(RetainedArgumentIndices.size());
    std::copy(RetainedArgumentIndices.begin(), RetainedArgumentIndices.end(), DAEOutput->begin());

    std::string RetainedArgsStr;
    for (int i = 0; i < DAEOutput->size(); ++i) {
      RetainedArgsStr += std::to_string(DAEOutput->at(i)) + " ";
    }

    HIPSYCL_DEBUG_INFO << "LLVMToBackend: Dead argument elimination for " << FName
                       << " has resulted in these arguments being retained: " << RetainedArgsStr
                       << "\n";
  }
}

void LLVMToBackendTranslator::setKnownPtrParamAlignment(const std::string &FunctionName,
                                                        int ParamIndex, int Alignment) {
  for (auto &Entry : KnownPtrParamAlignments[FunctionName]) {
    if (Entry.first == ParamIndex) {
      Entry.second = Alignment;
      return;
    }
  }
  KnownPtrParamAlignments[FunctionName].push_back(std::make_pair(ParamIndex, Alignment));
}

void LLVMToBackendTranslator::setReflectionField(const std::string &str, uint64_t value) {
  ReflectionFields[str] = value;
}

std::string LLVMToBackendTranslator::getCompilationIdentifier() const {
  std::string Result;
  for(const auto& K : Kernels) {
    Result += "<Kernel:"+K+">";
  }
  if(Result.empty())
    return "<no-kernels>";
  return Result;
}

}
}

