#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "branch-profiling"

using namespace llvm;
using namespace std;

// extern void updateBranchInfo(bool taken);
// extern void printOutBranchInfo();

namespace {
struct BranchProfiling : public ModulePass {
    static char ID;
    BranchProfiling() : ModulePass(ID) {}
    
    bool runOnModule(Module &M) override {
        for (auto F = M.begin(); F != M.end(); ++F) {
            runOnFunction(*F);
        }
        Function *mainFunc = M.getFunction("main");
        if (!mainFunc) mainFunc = M.getFunction("MAIN_");

        if (mainFunc) {
            // void printOutBranchInfo();
            FunctionCallee irt = M.getOrInsertFunction("initBranch", Type::getVoidTy(M.getContext()));
            IRBuilder<> Builder(&*(mainFunc->getEntryBlock().getFirstNonPHIOrDbgOrLifetime()));
            Builder.CreateCall(irt);
            FunctionCallee prt = M.getOrInsertFunction("printBranchProfiling", Type::getVoidTy(M.getContext()));
            for (auto B = mainFunc->begin(); B != mainFunc->end(); B++) {
                for (auto I = B->begin(); I != B->end(); I++) {
                    // insert at the end of main function
                    if (isa<ReturnInst>(I)) {
                        IRBuilder<> Builder(&*I);
                        // instrument printOutBranchInfo
                        Builder.CreateCall(prt);
                    }
                }
            }
        } else {
            llvm::outs() << "[main function is not found]" << "\n";
            for (auto F = M.begin(); F != M.end(); ++F) {
                if (F->isIntrinsic()) continue;
                llvm::outs() << F->getName() << "\n";
            }
        }
        return false;
    }

    bool runOnFunction(Function &F) {
        LLVMContext &context = F.getParent()->getContext();

        // void updateCondBranch(intptr_t ins_id, bool taken);
        // void updateUnCondBranch(intptr_t ins_id);
        FunctionCallee cond_func =
            F.getParent()->getOrInsertFunction("updateCondBranch", Type::getVoidTy(context),
                                               Type::getInt32Ty(context), Type::getInt1Ty(context));
        FunctionCallee uncond_func = F.getParent()->getOrInsertFunction(
            "updateUnCondBranch", Type::getVoidTy(context), Type::getInt32Ty(context));

        for (Function::iterator B = F.begin(), BE = F.end(); B != BE; ++B) {
            for (BasicBlock::iterator I = B->begin(), IE = B->end(); I != IE; ++I) {
                if (isa<CallInst>(I)) {
                    // to avoid call bitcast
                    CallInst* CI = dyn_cast<CallInst>(&*I);
                    if (const Function *call_func =
                            dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts()))
                    {
                        if (call_func->isIntrinsic())
                            continue;
                        std::string caller = call_func->getName();
                        if (caller != "exit" && caller != "f90_stop08a" &&
                            caller.find("quit_flag_") == std::string::npos) {
                            continue;
                        }
                        FunctionCallee prt = F.getParent()->getOrInsertFunction(
                            "printBranchProfiling", Type::getVoidTy(context));
                        IRBuilder<> builder_(&*I);
                        builder_.CreateCall(prt);
                        llvm::outs() << "[printInstrReuseDist instrumented] " 
                                    << "Function: "    << F.getName() << ", "
                                    << "Instruction: " << *I << "\n";
                    }
                } else if (isa<BranchInst>(I)) {
                    const BranchInst* br = dyn_cast<BranchInst>(&*I);
                    IRBuilder<> Builder(&*I);
                    vector<Value *> args;
                    Value *ins_id = ConstantInt::get(
                        Type::getInt32Ty(context),
                        reinterpret_cast<std::intptr_t>(dyn_cast<Instruction>(&*I)));
                    args.push_back(ins_id);
                    if (br->isConditional()) {
                        args.push_back(br->getCondition());
                        Builder.CreateCall(cond_func, args);
                    }
                    else {
                        Builder.CreateCall(uncond_func, args);
                    }
                }
            }
        }
        return false;
    }
}; // end of struct BranchProfiling
} // end of anonymous namespace

char BranchProfiling::ID = 0;
static RegisterPass<BranchProfiling> X(DEBUG_TYPE, "Profiling Branch Bias", false /* Only looks at CFG */,
                                       false /* Analysis Pass */);

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<filename>.bc"), cl::init(""));
static cl::opt<std::string> OutputputFilename(cl::Positional,
                                              cl::desc("<filename>-instrumented.bc"), cl::init(""));

#if LLVM_VERSION_MAJOR >= 4
static ManagedStatic<LLVMContext> GlobalContext;
static LLVMContext &getGlobalContext() { return *GlobalContext; }
#endif

int main(int argc, const char *argv[]) {
    LLVMContext &Context = getGlobalContext();
    // static LLVMContext Context;
    SMDiagnostic Err;
    // Parse the command line to read the Inputfilename
    cl::ParseCommandLineOptions(argc, argv, "LLVM branch profiling...\n");

    // load the input module
    std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
    if (!M) {
        Err.print(argv[0], errs());
        return -1;
    }

    llvm::legacy::PassManager Passes;

    /// Transform it to SSA
    // Passes.add(llvm::createPromoteMemoryToRegisterPass());
    // Passes.add(new LoopInfoWrapperPass());

    Passes.add(new BranchProfiling());
    Passes.run(*M.get());

    // Write back the instrumentation info into LLVM IR
    std::error_code EC;
    std::unique_ptr<ToolOutputFile> Out(new ToolOutputFile(OutputputFilename, EC, sys::fs::F_None));
    WriteBitcodeToFile(*M.get(), Out->os());
    Out->keep();

    return 0;
}
