#include "newton-irPass-LLVMIR-quantDecider.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "newton-irPass-LLVMIR-quantization.h"

using namespace llvm;

namespace
{
struct CostBreakdown {
	double cfp;
	double cint;
	double cq;
	double cdq;
	double ccf;
	int    instructionCount;
	int    fpClusterInstructionCount;
};

struct TargetProfile {
	bool   hasFPU;
	double fpFactor;
	double intFactor;
	double qFactor;
	double dqFactor;
	double marginFactor;
	bool   confident;
};

static bool
isMathRuntimeCall(const CallBase & callBase)
{
	Function * callee = callBase.getCalledFunction();
	if (!callee || !callee->hasName())
		return false;

	StringRef name = callee->getName();
	return name == "log" || name == "logf" ||
	       name == "exp" || name == "expf" ||
	       name == "sqrt" || name == "sqrtf" ||
	       name == "sin" || name == "sinf" ||
	       name == "cos" || name == "cosf" ||
	       name == "log1p" || name == "log1pf" ||
	       name.startswith("llvm.sqrt") ||
	       name.startswith("llvm.log") ||
	       name.startswith("llvm.exp") ||
	       name.startswith("llvm.sin") ||
	       name.startswith("llvm.cos");
}

static bool
usesOrProducesFloatingPoint(const Instruction & instruction)
{
	if (instruction.getType()->isFloatingPointTy())
		return true;

	for (const Value * operand : instruction.operands())
	{
		if (operand->getType()->isFloatingPointTy())
			return true;
		if (operand->getType()->isPointerTy() && operand->getType()->getPointerElementType()->isFloatingPointTy())
			return true;
	}

	if (const auto * callBase = dyn_cast<CallBase>(&instruction))
	{
		if (isMathRuntimeCall(*callBase))
			return true;
		if (callBase->getType()->isFloatingPointTy())
			return true;
		for (const Value * arg : callBase->args())
		{
			if (arg->getType()->isFloatingPointTy())
				return true;
		}
	}

	return false;
}

static bool
isCoreFloatingPointOp(const Instruction & instruction)
{
	switch (instruction.getOpcode())
	{
		case Instruction::FAdd:
		case Instruction::FSub:
		case Instruction::FMul:
		case Instruction::FDiv:
		case Instruction::FRem:
		case Instruction::FNeg:
			return true;
		case Instruction::Call:
		{
			const auto * callBase = dyn_cast<CallBase>(&instruction);
			return callBase && isMathRuntimeCall(*callBase);
		}
		default:
			return false;
	}
}

static double
floatingPointOpCost(const Instruction & instruction)
{
	switch (instruction.getOpcode())
	{
		case Instruction::FAdd:
		case Instruction::FSub:
			return 4.0;
		case Instruction::FMul:
			return 5.0;
		case Instruction::FDiv:
		case Instruction::FRem:
			return 14.0;
		case Instruction::FNeg:
			return 3.0;
		case Instruction::Call:
			return 15.0;
		default:
			return 0.0;
	}
}

static double
integerOpCost(const Instruction & instruction)
{
	switch (instruction.getOpcode())
	{
		case Instruction::Add:
		case Instruction::Sub:
		case Instruction::And:
		case Instruction::Or:
		case Instruction::Xor:
		case Instruction::Shl:
		case Instruction::AShr:
		case Instruction::LShr:
			return 1.0;
		case Instruction::Mul:
			return 2.0;
		case Instruction::SDiv:
		case Instruction::UDiv:
		case Instruction::SRem:
		case Instruction::URem:
			return 8.0;
		default:
			return 0.0;
	}
}

static double
quantizationBoundaryCost(const Instruction & instruction)
{
	switch (instruction.getOpcode())
	{
		case Instruction::FPToSI:
		case Instruction::FPToUI:
			return 6.0;
		default:
			return 0.0;
	}
}

static double
dequantizationBoundaryCost(const Instruction & instruction)
{
	switch (instruction.getOpcode())
	{
		case Instruction::SIToFP:
		case Instruction::UIToFP:
			return 6.0;
		default:
			return 0.0;
	}
}

static double
estimateBasicBlockWeight(const Function & function, const BasicBlock & block,
			 const std::map<const BasicBlock *, int> & blockOrder)
{
	double weight		= 1.0;
	int    predecessorCount = 0;
	for (const BasicBlock * ignored : predecessors(&block))
	{
		(void)ignored;
		predecessorCount++;
	}
	weight += 0.10 * predecessorCount;

	const Instruction * terminator = block.getTerminator();
	if (terminator)
	{
		if (terminator->getOpcode() == Instruction::Br)
		{
			const auto * branchInst = cast<BranchInst>(terminator);
			if (branchInst->isConditional())
				weight += 0.35;
		}
		else if (terminator->getOpcode() == Instruction::Switch)
		{
			const auto * switchInst = cast<SwitchInst>(terminator);
			weight += 0.05 * (switchInst->getNumCases() + 1);
		}
	}

	auto currentIt = blockOrder.find(&block);
	if (currentIt != blockOrder.end())
	{
		int currentIndex = currentIt->second;
		for (const BasicBlock * successor : successors(&block))
		{
			auto succIt = blockOrder.find(successor);
			if (succIt != blockOrder.end() && succIt->second <= currentIndex)
			{
				weight += 0.70;
			}
		}
	}

	if (function.getEntryBlock().getName().equals(block.getName()))
		weight += 0.05;

	return weight;
}

static CostBreakdown
estimateCostBreakdown(const Function & function)
{
	CostBreakdown breakdown = {0.0, 0.0, 0.0, 0.0, 0.0, 0, 0};

	std::map<const BasicBlock *, int> blockOrder;
	int				  index = 0;
	for (const BasicBlock & block : function)
	{
		blockOrder.emplace(&block, index++);
	}

	for (const BasicBlock & block : function)
	{
		double blockWeight = estimateBasicBlockWeight(function, block, blockOrder);
		double blockCfp	   = 0.0;
		double blockCint   = 0.0;
		double blockCq	   = 0.0;
		double blockCdq	   = 0.0;

		for (const Instruction & instruction : block)
		{
			if (isCoreFloatingPointOp(instruction))
			{
				blockCfp += floatingPointOpCost(instruction);
				breakdown.fpClusterInstructionCount++;
			}
			else if (usesOrProducesFloatingPoint(instruction))
			{
				blockCfp += 1.0;
				breakdown.fpClusterInstructionCount++;
			}

			blockCint += integerOpCost(instruction);
			blockCq += quantizationBoundaryCost(instruction);
			blockCdq += dequantizationBoundaryCost(instruction);
			breakdown.instructionCount++;
		}

		double blockControlFlowCost = 0.0;
		if (const Instruction * terminator = block.getTerminator())
		{
			switch (terminator->getOpcode())
			{
				case Instruction::Br:
				{
					const auto * branchInst = cast<BranchInst>(terminator);
					blockControlFlowCost += branchInst->isConditional() ? 2.0 : 0.7;
					break;
				}
				case Instruction::Switch:
				{
					const auto * switchInst = cast<SwitchInst>(terminator);
					blockControlFlowCost += 3.0 + 0.25 * (switchInst->getNumCases() + 1);
					break;
				}
				default:
					break;
			}
		}

		breakdown.cfp += blockWeight * blockCfp;
		breakdown.cint += blockWeight * blockCint;
		breakdown.cq += blockWeight * blockCq;
		breakdown.cdq += blockWeight * blockCdq;
		breakdown.ccf += blockWeight * blockControlFlowCost;
	}

	return breakdown;
}

static std::string
collectTargetFeatures(const Module & module)
{
	std::string merged;
	for (const Function & function : module)
	{
		if (function.hasFnAttribute("target-features"))
		{
			if (!merged.empty())
				merged += ",";
			merged += function.getFnAttribute("target-features").getValueAsString().str();
		}
	}
	return merged;
}

static bool
containsToken(const std::string & haystack, const std::string & token)
{
	return haystack.find(token) != std::string::npos;
}

static TargetProfile
detectTargetProfile(const Module & module)
{
	TargetProfile profile = {false, 1.0, 1.0, 1.0, 1.0, 0.05, false};

	std::string triple   = module.getTargetTriple();
	std::string features = collectTargetFeatures(module);

	bool explicitFPU = containsToken(features, "+vfp") || containsToken(features, "+neon") ||
			   containsToken(features, "+fp-armv8") || containsToken(features, "+fp16") ||
			   containsToken(features, "+vfp4") || containsToken(features, "+fpv4-sp-d16");
	bool softFloat	= containsToken(features, "+soft-float") || containsToken(features, "+softfp");
	bool isAArch64	= containsToken(triple, "aarch64");
	bool isThumb	= containsToken(triple, "thumb");
	bool isARM	= containsToken(triple, "arm") || isThumb || isAArch64;
	bool isMProfile = containsToken(triple, "armv6-m") || containsToken(triple, "armv6m") ||
			  containsToken(triple, "armv7-m") || containsToken(triple, "armv7m") ||
			  containsToken(triple, "armv7e-m") || containsToken(triple, "armv7em") ||
			  containsToken(triple, "armv8-m") || containsToken(triple, "armv8m") ||
			  isThumb;
	bool isAProfile = isAArch64 || containsToken(triple, "armv7-a") || containsToken(triple, "armv8-a");

	if (isAProfile && explicitFPU)
	{
		profile.hasFPU	     = true;
		profile.fpFactor     = 0.85;
		profile.intFactor    = 1.00;
		profile.qFactor	     = 1.15;
		profile.dqFactor     = 1.15;
		profile.marginFactor = 0.10;
		profile.confident    = true;
		return profile;
	}

	if (isMProfile && (!explicitFPU || softFloat))
	{
		profile.hasFPU	     = false;
		profile.fpFactor     = 2.30;
		profile.intFactor    = 0.95;
		profile.qFactor	     = 1.25;
		profile.dqFactor     = 1.30;
		profile.marginFactor = 0.02;
		profile.confident    = true;
		return profile;
	}

	if (isMProfile && explicitFPU)
	{
		profile.hasFPU	     = explicitFPU;
		profile.fpFactor     = explicitFPU ? 1.05 : 1.80;
		profile.intFactor    = 0.95;
		profile.qFactor	     = explicitFPU ? 1.10 : 1.20;
		profile.dqFactor     = explicitFPU ? 1.15 : 1.25;
		profile.marginFactor = explicitFPU ? 0.08 : 0.03;
		profile.confident    = true;
		return profile;
	}

	profile.hasFPU	     = explicitFPU;
	profile.fpFactor     = explicitFPU ? 1.00 : (isARM ? 1.80 : 1.40);
	profile.intFactor    = 1.00;
	profile.qFactor	     = explicitFPU ? 1.15 : (isARM ? 1.20 : 1.10);
	profile.dqFactor     = explicitFPU ? 1.20 : (isARM ? 1.25 : 1.15);
	profile.marginFactor = 0.06;
	profile.confident    = false;
	return profile;
}

static bool
hasFloatingPointCluster(const Function & function)
{
	for (const BasicBlock & block : function)
	{
		for (const Instruction & instruction : block)
		{
			if (isCoreFloatingPointOp(instruction) || usesOrProducesFloatingPoint(instruction))
				return true;
		}
	}
	return false;
}

static int
countFloatingPointOperations(const Function & function)
{
	int operationCount = 0;
	for (const BasicBlock & block : function)
	{
		for (const Instruction & instruction : block)
		{
			if (isCoreFloatingPointOp(instruction))
				operationCount++;
		}
	}
	return operationCount;
}
}  // namespace

extern "C" {

QuantDeciderResult
irPassLLVMIRQuantDecideFunction(void * N, Module & module, Function & llvmIrFunction, int maxPrecisionBits)
{
	QuantDeciderResult result = {false, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0, 0, 0, false};

	if (llvmIrFunction.isDeclaration())
		return result;

	if (!hasFloatingPointCluster(llvmIrFunction))
		return result;

	TargetProfile profile = detectTargetProfile(module);
	result.targetHasFPU   = profile.hasFPU;

	CostBreakdown originalBreakdown	 = estimateCostBreakdown(llvmIrFunction);
	result.originalInstructionCount	 = originalBreakdown.instructionCount;
	result.fpClusterInstructionCount = originalBreakdown.fpClusterInstructionCount;

	std::unique_ptr<Module> clonedModule = CloneModule(module);
	if (!clonedModule)
		return result;

	Function * clonedFunction = clonedModule->getFunction(llvmIrFunction.getName());
	if (!clonedFunction)
		return result;

	std::vector<Function *> functionsToInsert;
	irPassLLVMIRAutoQuantization(reinterpret_cast<State *>(N), *clonedFunction, functionsToInsert, maxPrecisionBits);

	CostBreakdown quantizedBreakdown = estimateCostBreakdown(*clonedFunction);
	result.quantizedInstructionCount = quantizedBreakdown.instructionCount;

	result.cfpCost	       = profile.fpFactor * originalBreakdown.cfp;
	result.cintCost	       = profile.intFactor * quantizedBreakdown.cint;
	result.cqCost	       = profile.qFactor * quantizedBreakdown.cq;
	result.cdqCost	       = profile.dqFactor * quantizedBreakdown.cdq;
	result.controlFlowCost = 0.5 * (originalBreakdown.ccf + quantizedBreakdown.ccf);

	result.originalCost  = result.cfpCost + originalBreakdown.ccf;
	result.quantizedCost = result.cintCost + result.cqCost + result.cdqCost + quantizedBreakdown.ccf;

	double effectiveCfp   = result.cfpCost + result.controlFlowCost;
	double effectiveQuant = result.cintCost + result.cqCost + result.cdqCost + result.controlFlowCost;
	result.decisionMargin = effectiveCfp * profile.marginFactor;
	if (!profile.confident)
		result.decisionMargin = (std::max)(result.decisionMargin, 4.0);

	result.shouldQuantize = effectiveCfp > (effectiveQuant + result.decisionMargin);

	errs() << "[quant-decider] function=" << llvmIrFunction.getName()
	       << " targetHasFPU=" << (result.targetHasFPU ? "true" : "false")
	       << " Cfp=" << result.cfpCost
	       << " Cint=" << result.cintCost
	       << " Cq=" << result.cqCost
	       << " Cdq=" << result.cdqCost
	       << " Ccf=" << result.controlFlowCost
	       << " margin=" << result.decisionMargin
	       << " shouldQuantize=" << (result.shouldQuantize ? "true" : "false") << "\n";

	return result;
}

void
irPassLLVMIRQuantDecider(void * N, Module & module, int maxPrecisionBits,
			 std::map<std::string, QuantDeciderResult> & decisionMap)
{
	decisionMap.clear();

	for (Function & function : module)
	{
		if (function.isDeclaration())
			continue;

		QuantDeciderResult result = irPassLLVMIRQuantDecideFunction(N, module, function, maxPrecisionBits);
		decisionMap.emplace(function.getName().str(), result);
	}

	for (const auto & entry : decisionMap)
	{
		Function * function = module.getFunction(entry.first);
		if (!function)
			continue;

		int fpOperationCount = countFloatingPointOperations(*function);
		if (fpOperationCount > 0)
		{
			errs() << "Function: " << entry.first
			       << " - Floating-point operations: " << fpOperationCount << "\n";
		}
	}
}
}
