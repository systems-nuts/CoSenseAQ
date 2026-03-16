#ifndef NEWTON_IR_PASS_LLVM_IR_QUANT_DECIDER
#define NEWTON_IR_PASS_LLVM_IR_QUANT_DECIDER

#include <map>
#include <string>

#ifdef __cplusplus
namespace llvm
{
class Module;
class Function;
}  // namespace llvm
extern "C" {
#endif

typedef struct QuantDeciderResult {
	bool   shouldQuantize;
	double originalCost;
	double quantizedCost;
	double cfpCost;
	double cintCost;
	double cqCost;
	double cdqCost;
	double controlFlowCost;
	double decisionMargin;
	int    originalInstructionCount;
	int    quantizedInstructionCount;
	int    fpClusterInstructionCount;
	bool   targetHasFPU;
} QuantDeciderResult;

QuantDeciderResult
irPassLLVMIRQuantDecideFunction(void * N, llvm::Module & module, llvm::Function & llvmIrFunction, int maxPrecisionBits);

void
irPassLLVMIRQuantDecider(void * N, llvm::Module & module, int maxPrecisionBits,
			 std::map<std::string, QuantDeciderResult> & decisionMap);

#ifdef __cplusplus
}
#endif

#endif
