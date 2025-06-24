// Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
// Distributed under the MIT license that can be found in the LICENSE file.
#include "codegen.h"
#include "config.h"
#include "enumerator.h"
#include "parse.h"
#include "slice.h"
#include "util/random.h"
#include "utils.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "hiredis.h"

#include <filesystem>

using namespace std;
using namespace llvm;
using namespace minotaur;

namespace fs = std::filesystem;

namespace {

llvm::cl::opt<unsigned>
    smt_to("minotaur-query-to",
           llvm::cl::desc("minotaur: timeout for SMT queries"),
           llvm::cl::init(60), llvm::cl::value_desc("s"));

llvm::cl::opt<unsigned> slice_to("minotaur-slice-to",
                                 llvm::cl::desc("minotaur: timeout per slice"),
                                 llvm::cl::init(300),
                                 llvm::cl::value_desc("s"));

llvm::cl::opt<bool> smt_verbose("minotaur-smt-verbose",
                                llvm::cl::desc("minotaur: SMT verbose mode"),
                                llvm::cl::init(false));

llvm::cl::opt<bool>
    enable_caching("minotaur-enable-caching",
                   llvm::cl::desc("minotaur: enable result caching"),
                   llvm::cl::init(true));

llvm::cl::opt<bool>
    ignore_mca("minotaur-ignore-machine-cost",
               llvm::cl::desc("minotaur: ignore llvm-mca cost model"),
               llvm::cl::init(false));

llvm::cl::opt<bool>
    debug_enumerator("minotaur-debug-enumerator",
                     llvm::cl::desc("minotaur: enable enumerator debug output"),
                     llvm::cl::init(false));

llvm::cl::opt<bool>
    debug_slicer("minotaur-debug-slicer",
                 llvm::cl::desc("minotaur: enable slicer debug output"),
                 llvm::cl::init(false));

llvm::cl::opt<bool>
    debug_tv("minotaur-debug-tv",
             llvm::cl::desc("minotaur: enable alive2 debug output"),
             llvm::cl::init(false));

llvm::cl::opt<bool>
    debug_codegen("minotaur-debug-codegen",
                  llvm::cl::desc("minotaur: enable alive2 debug output"),
                  llvm::cl::init(false));

llvm::cl::opt<bool>
    debug_parser("minotaur-debug-parser",
                 llvm::cl::desc("minotaur: enable alive2 debug output"),
                 llvm::cl::init(false));

llvm::cl::opt<unsigned> redis_port("minotaur-redis-port",
                                   llvm::cl::desc("redis port number"),
                                   llvm::cl::init(6379));

llvm::cl::opt<bool> no_infer("minotaur-no-infer",
                             llvm::cl::desc("minotaur: do not run synthesizer"),
                             llvm::cl::init(false));

llvm::cl::opt<bool> no_slice("minotaur-no-slice",
                             llvm::cl::desc("minotaur: do not run slicer"),
                             llvm::cl::init(false));

llvm::cl::opt<bool>
    force_infer("minotaur-force-infer",
                llvm::cl::desc("minotaur: force infer even if cache hits"),
                llvm::cl::init(false));

llvm::cl::opt<string> report_dir("minotaur-report-dir",
                                 llvm::cl::desc("Save report to disk"),
                                 llvm::cl::value_desc("directory"));

static bool dom_check(llvm::Value *V, const DominatorTree &DT, llvm::Use &U) {
  if (auto I = dyn_cast<Instruction>(V)) {
    for (auto &op : I->operands()) {
      if (auto opI = dyn_cast<Instruction>(op)) {
        if (!DT.dominates(opI, U))
          return false;
      }
    }
  }
  return true;
}

struct debug {
  template <class T> debug &operator<<(const T &s) {
    if (debug_enumerator || debug_slicer || debug_tv || debug_codegen)
      minotaur::config::dbg() << s;
    return *this;
  }
};

static optional<Rewrite> infer(Function &F, Instruction *I, redisContext *ctx,
                               Enumerator &EN, parse::Parser &P) {
  string bytecode;
  llvm::raw_string_ostream bs(bytecode);
  // WriteBitcodeToFile(*F.getParent(), bs);
  F.getParent()->print(bs, nullptr);
  bs.flush();

  vector<Rewrite> RHSs;

  bool from_cache = false;

  // for caching, minotaur has three modes:
  // 1. no_infer: do not run synthesizer
  // 2. force_infer: force synthesizer even if cache hits
  // 3. normal mode: run synthesizer if cache miss

  // check cache only in normal mode
  if (enable_caching && !force_infer && !no_infer) {
    std::string rewrite;

    if (minotaur::hGet(bytecode.c_str(), bytecode.size(), rewrite, ctx)) {
      if (rewrite == "<no-sol>") {
        debug() << "[online] cache matched, but no solution found in "
                   "previous run, skipping function: "
                << F.getName() << "\n";
        return nullopt;
      } else {
        debug() << "[online] cache matched, using previous solution for "
                   "function: "
                << F.getName() << "\n";
        RHSs = P.parse(F, rewrite);
        if (RHSs.empty()) {
          debug() << "[online] failed to parse cached solution\n";
          return nullopt;
        }
        debug() << *RHSs[0].I << "\n";
        from_cache = true;
      }
    }
  }

  if (no_infer) {
    // in no_infer mode, we write no-sol and return
    if (enable_caching) {
      hSetNoSolution(bytecode.c_str(), bytecode.size(), ctx, F.getName());
    }
    debug() << "[online] skipping synthesizer\n";
    return nullopt;
  } else if (!from_cache) {
    // in force_infer mode, as from_cache is always false, we run synthesizer
    // in normal mode, we run synthesizer only when cache misses
    debug() << "[online] working on function:\n" << F;
    RHSs = EN.solve(F, I);
    if (RHSs.empty()) {
      if (enable_caching)
        hSetNoSolution(bytecode.c_str(), bytecode.size(), ctx, F.getName());
      return nullopt;
    }
  }

  auto R = RHSs[0];
  debug() << "[online] synthesized solution:\n" << *R.I << "\n";

  // write back to cache
  if (!from_cache && enable_caching) {
    debug() << "[online] caching solution\n";
    string rewrite;
    raw_string_ostream rs(rewrite);
    R.I->print(rs);
    rs.flush();
    hSetRewrite(bytecode.c_str(), bytecode.size(), "", 0, rewrite, ctx,
                R.CostAfter, R.CostBefore, F.getName());
  }
  return R;
}

static bool optimize_function(llvm::Function &F, const LoopInfo &LI,
                              const DominatorTree &DT) {
  // set up debug output
  raw_ostream *out_file = &errs();
  if (!report_dir.empty()) {
    fs::create_directories(report_dir.getValue());

    fs::path fname = "minotaur.txt";
    fs::path path = fs::path(report_dir.getValue()) / fname.filename();

    do {
      auto newname = fname.stem();
      if (newname.compare("-") == 0 || newname.compare("<stdin>") == 0)
        newname = "in";
      newname += "_" + util::get_random_str(8) + ".txt";
      path.replace_filename(newname);
    } while (fs::exists(path));

    std::error_code EC;
    out_file = new raw_fd_ostream(path.string(), EC, llvm::sys::fs::OF_None);

    if (EC) {
      cerr << "Alive2: Couldn't open report file!" << endl;
      exit(1);
    }
  }
  config::set_debug(*out_file);

  debug() << "[online] minotaur version " << config::minotaur_version << " "
          << "working on source: " << F.getParent()->getSourceFileName()
          << "\n";

  debug() << "[online] working on function: " << F.getName() << "\n";
  debug() << *F.getParent() << "\n";

  // set alive2 options
  config::ignore_machine_cost = ignore_mca;
  config::debug_enumerator = debug_enumerator;
  config::debug_tv = debug_tv;
  config::debug_slicer = debug_slicer;
  config::debug_codegen = debug_codegen;
  config::debug_parser = debug_parser;
  config::slice_to = slice_to;
  smt::solver_print_queries(smt_verbose);

  smt::set_query_timeout(to_string(smt_to * 1000));

  redisContext *ctx = nullptr;
  if (enable_caching) {
    ctx = redisConnect("127.0.0.1", redis_port);
  }

  bool changed = false;

  if (no_slice) {
    // in this mode we assume only one return point, we do not run slicer,
    // we check if return value can be optimized

    std::unique_ptr<llvm::Module> m;
    ValueToValueMapTy vv;
    auto newM = CloneModule(*F.getParent(), vv);
    newM->dump();

    auto newF = cast<Function>(vv[&F]);

    ReturnInst *ret = nullptr;
    for (auto &BB : *newF) {
      for (auto &I : BB) {
        if (isa<ReturnInst>(I)) {
          ret = cast<ReturnInst>(&I);
          break;
        }
      }
      if (ret) {
        break;
      }
    }
    if (!ret) {
      debug() << "[online] no return instruction found, skipping\n";
      goto final;
    }

    Instruction *retI = dyn_cast<Instruction>(ret->getReturnValue());

    if (!retI) {
      debug() << "[online] return value is not an instruction, skipping\n";
      goto final;
    }

    Enumerator EN;
    parse::Parser P(*newF);
    auto R = infer(*newF, retI, ctx, EN, P);
    if (!R.has_value()) {
      goto final;
    }

    unordered_set<llvm::Function *> IntrinDecls;
    ValueToValueMapTy vmap;
    auto *V = LLVMGen(ret, IntrinDecls).codeGen(R->I, vmap);
    V = llvm::IRBuilder<>(ret).CreateBitCast(V, retI->getType());
    retI->replaceAllUsesWith(V);
    changed = true;
  } else {
    for (auto &BB : F) {
      for (auto &I : make_early_inc_range(BB)) {
        if (I.getType()->isVoidTy())
          continue;

        minotaur::Slice S(F, LI, DT);
        auto NewF = S.extractExpr(I);
        auto m = S.getNewModule();

        if (!NewF.has_value())
          continue;

        Enumerator EN;
        parse::Parser P(NewF->first);
        auto R = infer(NewF->first, NewF->second, ctx, EN, P);

        if (!R.has_value())
          continue;

        unordered_set<llvm::Function *> IntrinDecls;
        Instruction *insertpt = I.getNextNode();
        while (isa<PHINode>(insertpt)) {
          insertpt = insertpt->getNextNode();
        }

        auto *V = LLVMGen(insertpt, IntrinDecls).codeGen(R->I, S.getValueMap());
        V = llvm::IRBuilder<>(insertpt).CreateBitCast(V, I.getType());

        I.replaceUsesWithIf(V, [&changed, &V, &DT](Use &U) {
          if (dom_check(V, DT, U)) {
            changed = true;
            return true;
          }
          return false;
        });
      }
    }
  }

final:
  if (changed) {
    eliminate_dead_code(F);
    F.removeFnAttr("min-legal-vector-width");
  }

  if (enable_caching)
    redisFree(ctx);

  if (changed)
    debug() << "[online] minotaur completed, changed the program\n";
  else {
    debug() << "[online] minotaur completed, no change to the program\n";
  }

  if (out_file != &errs()) {
    out_file->flush();
    delete out_file;
  }

  return changed;
}

struct MinotaurPass : PassInfoMixin<MinotaurPass> {
  PreservedAnalyses run(llvm::Function &F, FunctionAnalysisManager &FAM) {
    if (F.isDeclaration())
      return PreservedAnalyses::all();

    const LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
    const DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    optimize_function(F, LI, DT);
    return PreservedAnalyses::all();
  }
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "Minotaur Superoptimizer", "",
          [](llvm::PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](llvm::StringRef Name, llvm::FunctionPassManager &FPM,
                   llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
                  if (Name != "minotaur")
                    return false;

                  FPM.addPass(MinotaurPass());
                  return true;
                });
          }};
}

} // end anonymous namespace
