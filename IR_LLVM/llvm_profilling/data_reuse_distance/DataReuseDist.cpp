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
#include "llvm/IR/CallSite.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>

using namespace llvm;
using namespace std;

#define DEBUG_TYPE "data-reuse-dist"

namespace {
struct DataReuseDist : public ModulePass {
    static char ID;

    DataReuseDist() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
        for (auto F = M.begin(); F != M.end(); ++F) {
            runOnFunction(*F);
        }

        FunctionCallee init_cache = M.getOrInsertFunction("initLRUDataCache", Type::getVoidTy(M.getContext()));

        Function *mainFunc = M.getFunction("main");
        if (!mainFunc) mainFunc = M.getFunction("MAIN_");
        if (mainFunc) {
            IRBuilder<> Builder(&*(mainFunc->getEntryBlock().getFirstNonPHIOrDbgOrLifetime()));
            Builder.CreateCall(init_cache);
            FunctionCallee print_res = M.getOrInsertFunction("printDataReuseDist",
                                                       Type::getVoidTy(M.getContext()));
            for (auto B = mainFunc->begin(); B != mainFunc->end(); B++)
            {
                for (auto I = B->begin(); I != B->end(); I++) {
                    // insert at the end of main function
                    if (!isa<ReturnInst>(I)) continue;
                    IRBuilder<> Builder(&*I);
                    Builder.CreateCall(print_res);
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
        // skip instrumentations before global variables get initialized
        std::string func_name = F.getName();
        if (func_name == "__cxx_global_var_init" || func_name.find("_GLOBAL__sub_I_") != func_name.npos) {
            llvm::outs() << "skip function " << F.getName() << "\n";
            return false;
        }

        FunctionCallee insert_Int1Ptr = F.getParent()->getOrInsertFunction(
            "insertLRUDataCache", Type::getVoidTy(F.getParent()->getContext()),
            Type::getInt1PtrTy(F.getParent()->getContext()), Type::getInt32Ty(F.getParent()->getContext()));

        for (Function::iterator B = F.begin(); B != F.end(); ++B) {
            for (BasicBlock::iterator I = B->getFirstInsertionPt(); I != B->end(); ++I) {
                Instruction& inst = *I;
                if (isa<CallInst>(I)) {
                    // to avoid call bitcast
                    CallInst* CI = dyn_cast<CallInst>(&*I);
                    if (const Function *call_func = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts())) {
                        if (call_func->isIntrinsic())
                            continue;
                        std::string caller = call_func->getName();
                        if (caller != "exit" && caller != "f90_stop08a" &&
                            caller.find("quit_flag_") == std::string::npos) {
                            continue;
                        }
                        FunctionCallee print_res = F.getParent()->getOrInsertFunction("printDataReuseDist",
                                                       Type::getVoidTy(F.getParent()->getContext()));
                        IRBuilder<> builder_(&*I);
                        builder_.CreateCall(print_res);
                    }
                } else if (isa<PHINode>(I) || isa<InvokeInst>(I)) {
                    // skip instrument on `phi` or `invoke` instructions
                    continue;
                }
                // Get the operand (addr/ptr)
                Value *opnd;
                // only track the operands of Load and Store Instruction
                if (isa<LoadInst>(I)) {
                    opnd = inst.getOperand(0);
                }
                else if (isa<StoreInst>(I)) {
                    opnd = inst.getOperand(1);
                }
                else {
                    continue;
                }
                // Get the type of the operand, only callback on ptr
                if (opnd->getType()->isPointerTy()) {
                    IRBuilder<> Builder(&*I);
                    opnd = Builder.CreateBitCast(opnd, Type::getInt1PtrTy(F.getParent()->getContext()));
                    std::vector<Value *> args;
                    args.push_back(opnd); 
		    args.push_back(ConstantInt::get(Type::getInt32Ty(F.getParent()->getContext()), I->getOpcode()));
		    Builder.CreateCall(insert_Int1Ptr, args);
		    //Builder.CreateCall(insert_Int1Ptr, opnd);
                }
                else {
                    llvm::outs() << "Something ERROR..... " << inst << "\n";
                    // for debug
                }
                // llvm::outs() << inst;
                // std::cout << std::endl;
            }
        } // end for each basic block
        return false;
    }
}; // end of struct DataReuseDist
} // end of anonymous namespace

char DataReuseDist::ID = 0;
static RegisterPass<DataReuseDist> X(DEBUG_TYPE, "Data Reuse Distance profiling analysis");

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<filename>.ll"), cl::init(""));
static cl::opt<std::string> OutputputFilename(cl::Positional,
                                              cl::desc("<filename>-instrumented.bc"), cl::init(""));

#if LLVM_VERSION_MAJOR >= 4
static ManagedStatic<LLVMContext> GlobalContext;
static LLVMContext &getGlobalContext() { return *GlobalContext; }
#endif

int main(int argc, const char *argv[]) {
    // std::cout << "<<<<< main start\n";
    LLVMContext &Context = getGlobalContext();
    // std::cout << ">>>>> get context done\n";
    // static LLVMContext Context;
    SMDiagnostic Err;
    // Parse the command line to read the Inputfilename
    // std::cout << ">>>> Parse the command line to read the Inputfilename\n";
    cl::ParseCommandLineOptions(argc, argv, "Dynamic instructions profiling analysis...\n");
    // std::cout << ">>>> Parse the command line to read the Inputfilename done\n";

    // load the input module
    // std::cout << ">>>> load the input module\n";
    std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
    // std::cout << ">>>> load the input module done\n";
    if (!M) {
        // std::cout << ">>>> in if\n";
        Err.print(argv[0], errs());
        return -1;
    }
    // std::cout << ">>>> if load the input module done\n";

    llvm::legacy::PassManager Passes;

    /// Transform it to SSA
    // Passes.add(llvm::createPromoteMemoryToRegisterPass());
    // Passes.add(new LoopInfoWrapperPass());

    Passes.add(new DataReuseDist());
    Passes.run(*M.get());

    // Write back the instrumentation info into LLVM IR
    std::error_code EC;
    std::unique_ptr<ToolOutputFile> Out(new ToolOutputFile(OutputputFilename, EC, sys::fs::F_None));
    WriteBitcodeToFile(*M.get(), Out->os());
    Out->keep();

    return 0;
}
