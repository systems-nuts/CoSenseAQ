/*
Authored 2022. Pei Mu.

All rights reserved.

    Redistribution and use in source and binary forms, with or without
		modification, are permitted provided that the following conditions
	are met:

    *	Redistributions of source code must retain the above
	copyright notice, this list of conditions and the following
	disclaimer.

	    *	Redistributions in binary form must reproduce the above
		copyright notice, this list of conditions and the following
		disclaimer in the documentation and/or other materials
	       provided with the distribution.

		   *	Neither the name of the author nor the names of its
		       contributors may be used to endorse or promote products
	    derived from this software without specific prior written
		permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
	"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
	LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
	FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
	    COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
	    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
	    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
	    */

#ifdef __cplusplus
#include "newton-irPass-LLVMIR-rangeAnalysis.h"
#include "newton-irPass-LLVMIR-simplifyControlFlowByRange.h"
#include "newton-irPass-LLVMIR-constantSubstitution.h"
#include "newton-irPass-LLVMIR-shrinkTypeByRange.h"
#include "newton-irPass-LLVMIR-quantization.h"
#include "newton-irPass-LLVMIR-quantDecider.h"
#include "newton-irPass-LLVMIR-optimizeByRange.h"
#include "newton-irPass-LLVMIR-memoryAlignment.h"
#include "newton-irPass-LLVMIR-emitAssume.h"

#endif /* __cplusplus */

#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <set>

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Path.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Verifier.h"

#include "config.h"

#include <unordered_map>
#include <set>
#include <limits>

using namespace llvm;
// #define FRAC_BASE (1 << maxPrecisionBits)
#define FRAC_BASE (1 << MAX_PRECISION_BITS)

namespace
{
bool
isOverflowCheckTarget(const llvm::Value * V)
{
	if (!V)
		return false;

	auto * I = llvm::dyn_cast<llvm::Instruction>(V);
	if (!I)
		return false;

	switch (I->getOpcode())
	{
		case llvm::Instruction::FAdd:
		case llvm::Instruction::FSub:
		case llvm::Instruction::FMul:
		case llvm::Instruction::FDiv:
		case llvm::Instruction::FRem:
		case llvm::Instruction::FNeg:
			return true;
		case llvm::Instruction::Call:
		{
			auto * CI = llvm::dyn_cast<llvm::CallInst>(I);
			if (!CI)
				return false;

			llvm::Function * callee = CI->getCalledFunction();
			if (!callee || !callee->hasName())
				return false;

			llvm::StringRef funcName = callee->getName();
			return funcName == "log" || funcName == "exp" ||
			       funcName == "sqrt" || funcName == "log1p" ||
			       funcName == "scalbn" || funcName == "sin" ||
			       funcName == "cos" ||
			       funcName.startswith("llvm.fabs") ||
			       funcName.startswith("llvm.floor") ||
			       funcName.startswith("llvm.ceil");
		}
		default:
			return false;
	}
}
}  // namespace

void
checkOverflow(State * N, BoundInfo * boundInfo, int FRAC_Q, llvm::Function * ownerFunction)
{
	if (!ownerFunction || ownerFunction->isDeclaration())
		return;

	if (!ownerFunction->hasFnAttribute("newton.dequantize"))
		return;

	int maxVal, minVal;
	if (BIT_WIDTH == 16)
	{
		maxVal = INT16_MAX;
		minVal = INT16_MIN;
	}
	else if (BIT_WIDTH == 32)
	{
		maxVal = INT32_MAX;
		minVal = INT32_MIN;
	}
	else
	{
		flexprint(N->Fe, N->Fm, N->Fperr, "Unsupported BIT_WIDTH: %d\n", BIT_WIDTH);
		return;
	}

	const double fracBase = std::ldexp(1.0, FRAC_Q);

	for (const auto & entry : boundInfo->virtualRegisterRange)
	{
		if (!isOverflowCheckTarget(entry.first))
			continue;

		if (!std::isfinite(entry.second.first) || !std::isfinite(entry.second.second))
			continue;

		double scaledMin = entry.second.first * fracBase;
		double scaledMax = entry.second.second * fracBase;

		std::string instStr = "unknown";
		if (Instruction * inst = dyn_cast<Instruction>(entry.first))
		{
			instStr = inst->getOpcodeName();
		}

		if ((scaledMin > maxVal && scaledMax > maxVal) || (scaledMin < minVal && scaledMax < minVal))
		{
			flexprint(N->Fe, N->Fm, N->Fperr,
				  "Definite overflow detected: %s range [%f, %f] when scaled by 2^%d is completely outside int%d bounds\n",
				  instStr.c_str(), entry.second.first, entry.second.second,
				  FRAC_Q, BIT_WIDTH);
		}
		else if (scaledMax > maxVal || scaledMin < minVal)
		{
			flexprint(N->Fe, N->Fm, N->Fperr,
				  "Possible overflow detected: %s range [%f, %f] when scaled by 2^%d partially exceeds int%d bounds\n",
				  instStr.c_str(), entry.second.first, entry.second.second,
				  FRAC_Q, BIT_WIDTH);
		}
	}
}

void
autoWhitelistFunctions(Module & Mod, std::set<std::string> & whitelist)
{
	for (Function & F : Mod)
	{
		if (F.isDeclaration())
			continue;

		bool hasSensorParams = false;

		for (auto & Arg : F.args())
		{
			if (Arg.hasName())
			{
				std::string argName = Arg.getName().str();
				if (argName.find("bmx055") != std::string::npos)
				{
					hasSensorParams = true;
					break;
				}
			}
		}

		if (hasSensorParams)
		{
			llvm::errs() << "Auto-Whitelisting function based on sensor params: " << F.getName() << "\n";
			whitelist.insert(F.getName().str());
		}
	}
}

void
eraseUnusedConstant(Module & M)
{
	std::set<std::string> quantizedBaseNames;
	for (auto & GV : M.globals())
	{
		if (GV.getName().endswith("_quantized"))
		{
			std::string baseName = GV.getName().str().substr(0, GV.getName().size() - 10);	// remove "_quantized"
			quantizedBaseNames.insert(baseName);
		}
	}
	std::vector<GlobalVariable *> toDelete;
	for (auto & GV : M.globals())
	{
		std::string name = GV.getName().str();

		if (GV.use_empty() && quantizedBaseNames.count(name))
		{
			toDelete.push_back(&GV);
		}
		if (GV.use_empty() && name.size() > 10 && name.substr(name.size() - 10) == "_quantized")
		{
			toDelete.push_back(&GV);
		}
	}
	for (auto * GV : toDelete)
	{
		GV->eraseFromParent();
	}
}

// Function to save the IR of a module to a file
void
saveModuleIR(llvm::Module & M, const std::string & fileName)
{
	std::error_code	     EC;
	llvm::raw_fd_ostream file(fileName, EC, llvm::sys::fs::OF_Text);
	if (EC)
	{
		llvm::errs() << "Error opening file " << fileName << " for writing: " << EC.message() << "\n";
		return;
	}
	M.print(file, nullptr);
	llvm::errs() << "IR saved to " << fileName << "\n";
	file.close();
}

double
computeResolution(Modality * mod)
{
	return (mod->rangeUpperBound - mod->rangeLowerBound) / (1 << mod->precisionBits);
}

void
dumpIR(State * N, std::string fileSuffix, const std::unique_ptr<Module> & Mod)
{
	StringRef   filePath(N->llvmIR);
	std::string dirPath	= std::string(sys::path::parent_path(filePath)) + "/";
	std::string fileName	= std::string(sys::path::stem(filePath)) + "_" + fileSuffix + ".bc";
	std::string filePathStr = dirPath + fileName;
	std::string tmpPathStr	= filePathStr + ".tmp";
	filePath		= StringRef(filePathStr);

	flexprint(N->Fe, N->Fm, N->Fpinfo, "Dump IR of: %s\n", filePath.str().c_str());
	std::error_code errorCode;
	raw_fd_ostream	dumpedFile(tmpPathStr, errorCode, llvm::sys::fs::OF_None);
	if (errorCode)
	{
		flexprint(N->Fe, N->Fm, N->Fperr, "Error: failed to open %s for bitcode output: %s\n",
			  tmpPathStr.c_str(), errorCode.message().c_str());
		return;
	}

	WriteBitcodeToFile(*Mod, dumpedFile);
	if (dumpedFile.has_error())
	{
		flexprint(N->Fe, N->Fm, N->Fperr, "Error: failed to write bitcode to %s\n",
			  filePath.str().c_str());
		dumpedFile.clear_error();
	}
	dumpedFile.flush();
	dumpedFile.close();

	errorCode = llvm::sys::fs::rename(tmpPathStr, filePathStr);
	if (errorCode)
	{
		flexprint(N->Fe, N->Fm, N->Fperr, "Error: failed to finalize bitcode file %s: %s\n",
			  filePath.str().c_str(), errorCode.message().c_str());
	}
}

void
mergeBoundInfo(BoundInfo * dst, const BoundInfo * src)
{
	dst->virtualRegisterRange.insert(src->virtualRegisterRange.begin(),
					 src->virtualRegisterRange.end());
	return;
}

void
collectCalleeInfo(std::vector<std::string> &	       calleeNames,
		  std::map<std::string, BoundInfo *> & funcBoundInfo,
		  const BoundInfo *		       boundInfo)
{
	for (auto & calleeInfo : boundInfo->calleeBound)
	{
		calleeNames.emplace_back(calleeInfo.first);
		funcBoundInfo.emplace(calleeInfo.first, calleeInfo.second);
		collectCalleeInfo(calleeNames, funcBoundInfo, calleeInfo.second);
	}
	return;
}

class FunctionNode {
	mutable AssertingVH<Function>	 F;
	FunctionComparator::FunctionHash Hash;

	public:
	// Note the hash is recalculated potentially multiple times, but it is cheap.
	FunctionNode(Function * F)
	    : F(F), Hash(FunctionComparator::functionHash(*F)) {}

	Function *
	getFunc() const
	{
		return F;
	}

	FunctionComparator::FunctionHash
	getHash() const
	{
		return Hash;
	}
};

GlobalNumberState GlobalNumbers;

class FunctionNodeCmp {
	public:
	bool
	operator()(const FunctionNode & LHS, const FunctionNode & RHS) const
	{
		// Order first by hashes, then full function comparison.
		if (LHS.getHash() != RHS.getHash())
			return LHS.getHash() < RHS.getHash();
		FunctionComparator FCmp(LHS.getFunc(), RHS.getFunc(), &GlobalNumbers);
		return FCmp.compare() == -1;
	}
};

using hashFuncSet = std::set<FunctionNode, FunctionNodeCmp>;

void
cleanFunctionMap(const std::unique_ptr<Module> & Mod, std::map<std::string, CallInst *> & callerMap)
{
	for (auto itFunc = callerMap.begin(); itFunc != callerMap.end();)
	{
		if (nullptr == Mod->getFunction(itFunc->first))
			itFunc = callerMap.erase(itFunc);
		else
			++itFunc;
	}
}

void
overloadFunc(std::unique_ptr<Module> & Mod, std::map<std::string, CallInst *> & callerMap)
{
	/*
	 * compare the functions and remove the redundant one
	 * */
	hashFuncSet baseFuncs;
	auto	    baseFuncNum = baseFuncs.size();
	for (auto itFunc = Mod->getFunctionList().rbegin(); itFunc != Mod->getFunctionList().rend(); itFunc++)
	{
		if (!itFunc->hasName() || itFunc->getName().empty())
			continue;
		if (itFunc->getName().startswith("llvm.dbg.value") ||
		    itFunc->getName().startswith("llvm.dbg.declare"))
			continue;
		if (itFunc->isDeclaration())
			continue;
		baseFuncs.emplace(FunctionNode(&(*itFunc)));
		/*
		 * find the function with the same implementation and change the callInst
		 * */
		if (baseFuncNum == baseFuncs.size())
		{
			auto callerIt = callerMap.find(itFunc->getName().str());
			assert(callerIt != callerMap.end());
			auto		  currentCallerInst = callerIt->second;
			auto		  currentFuncNode   = FunctionNode(&(*itFunc));
			GlobalNumberState cmpGlobalNumbers;
			auto		  sameImplIt = std::find_if(baseFuncs.begin(), baseFuncs.end(),
								    [currentFuncNode, &cmpGlobalNumbers](const FunctionNode & func) {
								    FunctionComparator FCmp(func.getFunc(), currentFuncNode.getFunc(), &cmpGlobalNumbers);
								    return func.getHash() == currentFuncNode.getHash() && FCmp.compare() == 0;
							    });
			assert(sameImplIt != baseFuncs.end());
			currentCallerInst->setCalledFunction(sameImplIt->getFunc());
		}
		else
			baseFuncNum = baseFuncs.size();
	}

	legacy::PassManager passManager;
	passManager.add(createGlobalDCEPass());
	passManager.run(*Mod);
}

void
irPassLLVMIROptimizeByRange(State * N, bool enableQuantization, bool enableOverload,
			    bool enableBuiltinAssume, bool enableQuantDecider)
{
	llvm::errs() << "Entering irPassLLVMIROptimizeByRange\n";
	if (N->llvmIR == nullptr)
	{
		flexprint(N->Fe, N->Fm, N->Fperr, "Please specify the LLVM IR input file\n");
		llvm::errs() << "Error: llvmIR is nullptr\n";
		fatal(N, Esanity);
	}

	SMDiagnostic		Err;
	LLVMContext		Context;
	std::unique_ptr<Module> Mod(parseIRFile(N->llvmIR, Err, Context));
	if (!Mod)
	{
		flexprint(N->Fe, N->Fm, N->Fperr, "Error: Couldn't parse IR file.");
		llvm::errs() << "Error: Couldn't parse IR file: " << N->llvmIR << "\n";
		fatal(N, Esanity);
	}
	llvm::errs() << "Module successfully parsed: " << N->llvmIR << "\n";
	auto				   globalBoundInfo = new BoundInfo();
	std::map<std::string, BoundInfo *> funcBoundInfo;

	/*
	 * get sensor info, we only concern the id and range here
	 * */
	std::map<std::string, std::pair<double, double>> typeRange;
	if (N->sensorList != NULL)
	{
		for (Modality * currentModality = N->sensorList->modalityList; currentModality != NULL; currentModality = currentModality->next)
		{
			flexprint(N->Fe, N->Fm, N->Fpinfo, "\tModality: %s\n", currentModality->identifier);
			flexprint(N->Fe, N->Fm, N->Fpinfo, "\t\trangeLowerBound: %f\n", currentModality->rangeLowerBound);
			flexprint(N->Fe, N->Fm, N->Fpinfo, "\t\trangeUpperBound: %f\n", currentModality->rangeUpperBound);
			typeRange.emplace(currentModality->identifier, std::make_pair(currentModality->rangeLowerBound, currentModality->rangeUpperBound));
			double resolution = computeResolution(currentModality);
			flexprint(N->Fe, N->Fm, N->Fpinfo, "\t\tresolution: %.10f\n", resolution);
		}
	}

	/*
	 *TODO  get sensor info, we only concern the id and precisionBits here
	 * */
	std::map<std::string, int> typePrecisionBits;
	if (N->sensorList != NULL)
	{
		for (Modality * currentModality = N->sensorList->modalityList; currentModality != NULL; currentModality = currentModality->next)
		{
			flexprint(N->Fe, N->Fm, N->Fpinfo, "\tModality: %s\n", currentModality->identifier);
			flexprint(N->Fe, N->Fm, N->Fpinfo, "\t\tprecisionBits: %d\n", currentModality->precisionBits);
			typePrecisionBits.emplace(currentModality->identifier, currentModality->precisionBits);

			// resolution
		}
	}

	//	int maxPrecisionBits = 0;
	//	int maxPrecisionBits = MAX_PRECISION_BITS;
	//	for (auto & typePrecisionBit : typePrecisionBits)
	//	{
	//		if (typePrecisionBit.second > maxPrecisionBits)
	//		{
	//			maxPrecisionBits = typePrecisionBit.second;
	//		}
	//	}
	//

	//	int maxPrecisionBits = 0;
	//	for (auto & typePrecisionBit : typePrecisionBits)
	//	{
	//		if (typePrecisionBit.second > maxPrecisionBits)
	//		{
	//			maxPrecisionBits = typePrecisionBit.second;
	//		}
	//	}
	//	int MAX_PRECISION_BITS = maxPrecisionBits;

	int maxPrecisionBits = MAX_PRECISION_BITS;

	flexprint(N->Fe, N->Fm, N->Fpinfo, "Precision Analysis");

	double minResolution = 0.0;
	bool   isFirstSensor = true;

	if (N->sensorList != NULL)
	{
		for (Modality * currentModality = N->sensorList->modalityList; currentModality != NULL;
		     currentModality		= currentModality->next)
		{
			// Calculate resolution
			double resolution = (currentModality->rangeUpperBound - currentModality->rangeLowerBound) /
					    (1 << currentModality->precisionBits);

			// Store and print
			currentModality->resolution = resolution;

			// Initialize or compare for minimum
			if (isFirstSensor)
			{
				minResolution = resolution;
				isFirstSensor = false;
			}
			else if (resolution < minResolution)
			{
				minResolution = resolution;
			}
		}
	}

	if (!isFirstSensor && std::isfinite(minResolution) && minResolution > 0.0)
	{
		double fracQ_exact = -log2(minResolution) - 1.0;
		int    fracQ	   = (int)ceil(fracQ_exact);

		const int maxSafeFracQ = (BIT_WIDTH > 1) ? (BIT_WIDTH - 1) : 0;
		if (fracQ < 0)
			fracQ = 0;
		if (fracQ > maxSafeFracQ)
			fracQ = maxSafeFracQ;

		maxPrecisionBits = fracQ;

		maxPrecisionBits = 16;



		flexprint(N->Fe, N->Fm, N->Fpinfo, "Minimum resolution across all sensors: %f\n", minResolution);
		flexprint(N->Fe, N->Fm, N->Fpinfo,
			  "Required FRAC_Q: ceil(-log2(minResolution) - 1) = ceil(%f) = %d\n",
			  fracQ_exact, fracQ);
		flexprint(N->Fe, N->Fm, N->Fpinfo,
			  "Using computed precision bits (maxPrecisionBits/FRAC_Q): %d\n",
			  maxPrecisionBits);
	}
	else
	{
		flexprint(N->Fe, N->Fm, N->Fpinfo,
			  "Precision analysis unavailable; fallback to build-time MAX_PRECISION_BITS=%d\n",
			  maxPrecisionBits);
	}

	/**
	 * Config
	 */
	//	int BIT_WIDTH = 32;
	//	maxPrecisionBits = 16;

	/*
	 * get const global variables
	 * */
	std::map<llvm::Value *, std::vector<std::pair<double, double>>> virtualRegisterVectorRange;
	for (auto & globalVar : Mod->getGlobalList())
	{
		if (!globalVar.hasInitializer())
		{
			llvm::errs() << "Global variable " << globalVar.getName() << " has no initializer\n";
			continue;
		}
		llvm::errs() << "Processing global variable: " << globalVar.getName() << "\n";
		auto constValue = globalVar.getInitializer();
		if (ConstantFP * constFp = llvm::dyn_cast<llvm::ConstantFP>(constValue))
		{
			if (constValue->getType()->isFloatTy())
			{
				float constValue = constFp->getValueAPF().convertToFloat();
				globalBoundInfo->virtualRegisterRange.emplace(&globalVar, std::make_pair(constValue, constValue));
			}
			else if (constValue->getType()->isDoubleTy())
			{
				double constValue = constFp->getValueAPF().convertToDouble();
				globalBoundInfo->virtualRegisterRange.emplace(&globalVar, std::make_pair(constValue, constValue));
			}
		}
		else if (ConstantInt * constInt = llvm::dyn_cast<llvm::ConstantInt>(constValue))
		{
			auto constValue = constInt->getSExtValue();
			globalBoundInfo->virtualRegisterRange.emplace(&globalVar, std::make_pair(static_cast<double>(constValue),
												 static_cast<double>(constValue)));
		}
		else if (ConstantDataArray * constArr = llvm::dyn_cast<llvm::ConstantDataArray>(constValue))
		{
			auto arrType = constArr->getElementType();
			if (arrType->isDoubleTy())
			{
				for (size_t idx = 0; idx < constArr->getNumElements(); idx++)
				{
					double dbValue = constArr->getElementAsDouble(idx);
					virtualRegisterVectorRange[&globalVar].emplace_back(std::make_pair(dbValue, dbValue));
				}
			}
			else if (arrType->isFloatTy())
			{
				for (size_t idx = 0; idx < constArr->getNumElements(); idx++)
				{
					double ftValue = constArr->getElementAsFloat(idx);
					virtualRegisterVectorRange[&globalVar].emplace_back(std::make_pair(ftValue, ftValue));
				}
			}
			else if (arrType->isIntegerTy())
			{
				for (size_t idx = 0; idx < constArr->getNumElements(); idx++)
				{
					uint64_t intValue = constArr->getElementAsInteger(idx);
					virtualRegisterVectorRange[&globalVar].emplace_back(std::make_pair(intValue, intValue));
				}
			}
			else if (arrType->isPointerTy())
			{
				flexprint(N->Fe, N->Fm, N->Fperr, "\t\tTODO: Didn't support const pointer!\n");
			}
			else
			{
				flexprint(N->Fe, N->Fm, N->Fperr, "\t\tUnknown constant type!\n");
			}
		}
		else
		{
			flexprint(N->Fe, N->Fm, N->Fperr, "\t\tUnknown type!\n");
		}
	}

	/*
	 * analyze the range of all local variables in each function
	 * */
	flexprint(N->Fe, N->Fm, N->Fpinfo, "infer bound\n");
	std::map<std::string, CallInst *> callerMap;
	callerMap.clear();
	funcBoundInfo.clear();
	bool useOverLoad = false;
	for (auto & mi : *Mod)
	{
		auto boundInfo = new BoundInfo();
		mergeBoundInfo(boundInfo, globalBoundInfo);
		rangeAnalysis(N, mi, boundInfo, callerMap, typeRange, virtualRegisterVectorRange, useOverLoad);
		funcBoundInfo.emplace(mi.getName().str(), boundInfo);
		std::vector<std::string> calleeNames;
		collectCalleeInfo(calleeNames, funcBoundInfo, boundInfo);
	}

	/**
	 * Check for potential overflows
	 */
	flexprint(N->Fe, N->Fm, N->Fpinfo, "checking for potential overflows\n");
	for (auto & funcPair : funcBoundInfo)
	{
		checkOverflow(N, funcPair.second, maxPrecisionBits, Mod->getFunction(funcPair.first));
	}

	flexprint(N->Fe, N->Fm, N->Fpinfo, "shrink data type by range\n");
	for (auto & mi : *Mod)
	{
		auto boundInfoIt = funcBoundInfo.find(mi.getName().str());
		if (boundInfoIt != funcBoundInfo.end())
		{
			shrinkType(N, boundInfoIt->second, mi);
		}
		//            else
		//            {
		//	            memoryAlignment
		//	        }
	}

	if (enableQuantization)
	{
		flexprint(N->Fe, N->Fm, N->Fpinfo, "auto quantization\n");
		llvm::errs() << "Auto quantization enabled\n";
		std::map<std::string, QuantDeciderResult> decisionMap;
		if (enableQuantDecider)
		{
			irPassLLVMIRQuantDecider(N, *Mod, maxPrecisionBits, decisionMap);
		}
		std::vector<llvm::Function *> functionsToInsert;
		for (auto & mi : *Mod)
		{
			if (enableQuantDecider)
			{
				auto decisionIt = decisionMap.find(mi.getName().str());
				if (decisionIt != decisionMap.end() && !decisionIt->second.shouldQuantize)
				{
					llvm::errs() << "QuantDecider: skipping function " << mi.getName()
						     << " (origCost=" << decisionIt->second.originalCost
						     << ", quantCost=" << decisionIt->second.quantizedCost
						     << ")\n";
					continue;
				}
			}

			llvm::errs() << "Quantizing function: " << mi.getName() << "\n";

			irPassLLVMIRAutoQuantization(N, mi, functionsToInsert, maxPrecisionBits);
		}
		for (auto mi : functionsToInsert)
		{
			Mod->getFunctionList().remove(mi);
			Mod->getFunctionList().push_front(mi);
		}
	}

	/**
	 * Check for potential overflows
	 */
	flexprint(N->Fe, N->Fm, N->Fpinfo, "checking for potential overflows\n");
	for (auto & funcPair : funcBoundInfo)
	{
		checkOverflow(N, funcPair.second, maxPrecisionBits, Mod->getFunction(funcPair.first));
	}

	flexprint(N->Fe, N->Fm, N->Fpinfo, "memory alignment\n");
	for (auto & mi : *Mod)
	{
		auto boundInfoIt = funcBoundInfo.find(mi.getName().str());
		if (boundInfoIt != funcBoundInfo.end())
		{
			memoryAlignment(N, boundInfoIt->second, mi);
		}
		//        else
		//        {
		//            assert(false);
		//        }
	}

	//	/*
	//	 * remove the functions that are optimized by passes.
	//	 * */
	//			if (useOverLoad)
	//				cleanFunctionMap(Mod, callerMap);
	//
	//			if (useOverLoad)
	//				overloadFunc(Mod, callerMap);
	//
	//			callerMap.clear();
	//			funcBoundInfo.clear();
	//			useOverLoad = true;
	//			for (auto & mi : *Mod)
	//			{
	//				auto boundInfo = new BoundInfo();
	//				mergeBoundInfo(boundInfo, globalBoundInfo);
	//				rangeAnalysis(N, mi, boundInfo, callerMap, typeRange, virtualRegisterVectorRange, useOverLoad);
	//				funcBoundInfo.emplace(mi.getName().str(), boundInfo);
	//				std::vector<std::string> calleeNames;
	//				collectCalleeInfo(calleeNames, funcBoundInfo, boundInfo);
	//			}

	/*
	 * simplify the condition of each branch
	 * */
	flexprint(N->Fe, N->Fm, N->Fpinfo, "simplify control flow by range\n");
	for (auto & mi : *Mod)
	{
		auto boundInfoIt = funcBoundInfo.find(mi.getName().str());
		if (boundInfoIt != funcBoundInfo.end())
		{
			simplifyControlFlow(N, boundInfoIt->second, mi);
		}
		//		else
		//		{
		//			assert(false);
		//		}
	}

	legacy::PassManager passManager;
	passManager.add(createCFGSimplificationPass());
	passManager.add(createInstSimplifyLegacyPass());
	passManager.add(createGlobalDCEPass());
	passManager.run(*Mod);

	//		/*
	//		 * remove the functions that are optimized by passes.
	//		 * */
	//		if (useOverLoad)
	//			cleanFunctionMap(Mod, callerMap);
	//
	//		if (useOverLoad)
	//			overloadFunc(Mod, callerMap);
	//
	flexprint(N->Fe, N->Fm, N->Fpinfo, "infer bound\n");
	callerMap.clear();
	funcBoundInfo.clear();
	useOverLoad = false;
	for (auto & mi : *Mod)
	{
		auto boundInfo = new BoundInfo();
		mergeBoundInfo(boundInfo, globalBoundInfo);
		rangeAnalysis(N, mi, boundInfo, callerMap, typeRange, virtualRegisterVectorRange, useOverLoad);
		funcBoundInfo.emplace(mi.getName().str(), boundInfo);
		std::vector<std::string> calleeNames;
		collectCalleeInfo(calleeNames, funcBoundInfo, boundInfo);
	}
	//
	flexprint(N->Fe, N->Fm, N->Fpinfo, "constant substitution\n");
	for (auto & mi : *Mod)
	{
		auto boundInfoIt = funcBoundInfo.find(mi.getName().str());
		if (boundInfoIt != funcBoundInfo.end())
		{
			constantSubstitution(N, boundInfoIt->second, mi);
		}
		//		else
		//		{
		//			assert(false);
		//		}
	}
	//
	//			/*
	//			 * remove the functions that are optimized by passes.
	//			 * */
	//			if (useOverLoad)
	//				cleanFunctionMap(Mod, callerMap);
	//
	//			if (useOverLoad)
	//				overloadFunc(Mod, callerMap);

	// Finally, erase old functions

	//	eraseOldFunctions();

	// eraseOldGlobals();
	eraseUnusedConstant(*Mod);

	//		eraseOldFunctions();
	//	eraseOldFunctions();

	irPassLLVMIRApplyDequantization(*Mod, maxPrecisionBits);

	//	eraseOldFunctions();
	eraseOldFunctions(*Mod);

	StringRef   inputPath(N->llvmIR);
	std::string outDir  = std::string(sys::path::parent_path(inputPath));
	std::string outStem = std::string(sys::path::stem(inputPath)) + "_output.ll";
	std::string outPath = outDir + "/" + outStem;
	saveModuleIR(*Mod, outPath);
	if (verifyModule(*Mod, &llvm::errs()))
	{
		llvm::errs() << "Warning: in-memory module verification failed before sanitized bitcode emission.\n";
	}

	LLVMContext		dumpContext;
	SMDiagnostic		dumpErr;
	std::unique_ptr<Module> sanitizedOutputMod = parseIRFile(outPath, dumpErr, dumpContext);
	if (!sanitizedOutputMod)
	{
		llvm::errs() << "Warning: failed to parse serialized LLVM IR for sanitized bitcode output: "
			     << outPath << "\n";
		llvm::errs() << "Falling back to in-memory module for bitcode emission.\n";
	}
	else if (verifyModule(*sanitizedOutputMod, &llvm::errs()))
	{
		llvm::errs() << "Warning: sanitized output module verification failed; falling back to in-memory module.\n";
		sanitizedOutputMod.reset();
	}

	if (sanitizedOutputMod)
		dumpIR(N, "output", sanitizedOutputMod);
	else
		dumpIR(N, "output", Mod);
	llvm::errs() << "Exiting irPassLLVMIROptimizeByRange\n";
}
