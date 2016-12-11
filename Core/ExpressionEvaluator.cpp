#include "stdafx.h"
#include <climits>
#include <algorithm>
#include "ExpressionEvaluator.h"
#include "Console.h"
#include "Debugger.h"
#include "LabelManager.h"
#include "../Utilities/HexUtilities.h"

std::unordered_map<string, std::vector<int>, StringHasher> ExpressionEvaluator::_outputCache;
SimpleLock ExpressionEvaluator::_cacheLock;

bool ExpressionEvaluator::IsOperator(string token, int &precedence, bool unaryOperator)
{
	if(unaryOperator) {
		for(size_t i = 0, len = _unaryOperators.size(); i < len; i++) {
			if(token.compare(_unaryOperators[i]) == 0) {
				precedence = _unaryPrecedence[i];
				return true;
			}
		}
	} else {
		for(size_t i = 0, len = _binaryOperators.size(); i < len; i++) {
			if(token.compare(_binaryOperators[i]) == 0) {
				precedence = _binaryPrecedence[i];
				return true;
			}
		}
	}
	return false;
}

EvalOperators ExpressionEvaluator::GetOperator(string token, bool unaryOperator)
{
	if(unaryOperator) {
		for(size_t i = 0, len = _unaryOperators.size(); i < len; i++) {
			if(token.compare(_unaryOperators[i]) == 0) {
				return (EvalOperators)(2000000050 + i);
			}
		}
	} else {
		for(size_t i = 0, len = _binaryOperators.size(); i < len; i++) {
			if(token.compare(_binaryOperators[i]) == 0) {
				return (EvalOperators)(2000000000 + i);
			}
		}
	}
	return EvalOperators::Addition;
}

bool ExpressionEvaluator::CheckSpecialTokens(string expression, size_t &pos, string &output)
{
	string token;
	size_t initialPos = pos;
	size_t len = expression.size();
	do {
		char c = std::tolower(expression[pos]);
		if(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_') {
			//Only letters, numbers and underscore are allowed in code labels
			token += c;
			pos++;
		} else {
			break;
		}
	} while(pos < len);

	if(!token.compare("a")) {
		output += std::to_string(EvalValues::RegA);
	} else if(!token.compare("x")) {
		output += std::to_string(EvalValues::RegX);
	} else if(!token.compare("y")) {
		output += std::to_string(EvalValues::RegY);
	} else if(!token.compare("ps")) {
		output += std::to_string(EvalValues::RegPS);
	} else if(!token.compare("sp")) {
		output += std::to_string(EvalValues::RegSP);
	} else if(!token.compare("cycle")) {
		output += std::to_string(EvalValues::PpuCycle);
	} else if(!token.compare("scanline")) {
		output += std::to_string(EvalValues::PpuScanline);
	} else if(!token.compare("irq")) {
		output += std::to_string(EvalValues::Irq);
	} else if(!token.compare("nmi")) {
		output += std::to_string(EvalValues::Nmi);
	} else if(!token.compare("value")) {
		output += std::to_string(EvalValues::Value);
	} else if(!token.compare("address")) {
		output += std::to_string(EvalValues::Address);
	} else if(!token.compare("romaddress")) {
		output += std::to_string(EvalValues::AbsoluteAddress);
	} else {
		string originalExpression = expression.substr(initialPos, pos - initialPos);
		int32_t address = _debugger->GetLabelManager()->GetLabelRelativeAddress(originalExpression);
		if(address >= 0) {
			_containsCustomLabels = true;
			output += std::to_string(address);
		} else {
			return false;
		}
	}

	return true;
}

string ExpressionEvaluator::GetNextToken(string expression, size_t &pos)
{
	string output;
	bool isOperator = false;
	bool isNumber = false;
	bool isHex = false;
	size_t initialPos = pos;
	for(size_t len = expression.size(); pos < len; pos++) {
		char c = std::tolower(expression[pos]);

		if(c == '$' && pos == initialPos) {
			isHex = true;
		} else if((c >= '0' && c <= '9') || (isHex && c >= 'a' && c <= 'f')) {
			if(isNumber || output.empty()) {
				output += c;
				isNumber = true;
			} else {
				//Just hit the start of a number, done reading current token
				break;
			}
		} else if(isNumber) {
			//First non-numeric character, done
			break;
		} else if(c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '-' || c == '+' || c == '~') {
			if(output.empty()) {
				output += c;
				pos++;
			}
			break;
		} else if(c == '!') {
			//Figure out if it's ! or !=
			if(pos < len - 1) {
				if(expression[pos + 1] == '=') {
					output += "!=";
					pos+=2;
				} else {
					output += "!";
					pos++;
				}
			}
			break;
		} else {
			if(c == '$') {
				break;
			} else if((c < 'a' || c > 'z') && c != '_') {
				//Not a number, not a letter, this is an operator
				isOperator = true;
				output += c;
			} else {
				if(isOperator) {
					break;
				} else {
					if(output.empty()) {
						if(CheckSpecialTokens(expression, pos, output)) {
							break;
						}
					}
					output += c;
				}
			}
		}
	}

	if(isHex) {
		output = std::to_string(HexUtilities::FromHex(output));
	}

	return output;
}
	
bool ExpressionEvaluator::ProcessSpecialOperator(EvalOperators evalOp, std::stack<EvalOperators> &opStack, vector<int> &outputQueue)
{
	if(opStack.empty()) {
		return false;
	}
	while(opStack.top() != evalOp) {
		outputQueue.push_back(opStack.top());
		opStack.pop();

		if(opStack.empty()) {
			return false;
		}
	}
	if(evalOp != EvalOperators::Parenthesis) {
		outputQueue.push_back(opStack.top());
	}
	opStack.pop();

	return true;
}

bool ExpressionEvaluator::ToRpn(string expression, vector<int> &outputQueue)
{
	std::stack<EvalOperators> opStack = std::stack<EvalOperators>();	
	std::stack<int> precedenceStack;

	size_t position = 0;
	int parenthesisCount = 0;
	int bracketCount = 0;
	int braceCount = 0;

	bool previousTokenIsOp = true;
	while(true) {
		string token = GetNextToken(expression, position);
		if(token.empty()) {
			break;
		}

		bool unaryOperator = previousTokenIsOp;
		previousTokenIsOp = false;

		int precedence = 0;
		if(IsOperator(token, precedence, unaryOperator)) {
			EvalOperators op = GetOperator(token, unaryOperator);
			if(!opStack.empty()) {
				EvalOperators topOp = opStack.top();
				if((unaryOperator && precedence < precedenceStack.top()) || (!unaryOperator && precedence <= precedenceStack.top())) {
					opStack.pop();
					precedenceStack.pop();
					outputQueue.push_back(topOp);
				}
			}
			opStack.push(op);
			precedenceStack.push(precedence);
			
			previousTokenIsOp = true;
		} else if(token[0] == '(') {
			parenthesisCount++;
			opStack.push(EvalOperators::Parenthesis);
			precedenceStack.push(0);
			previousTokenIsOp = true;
		} else if(token[0] == ')') {
			parenthesisCount--;
			if(!ProcessSpecialOperator(EvalOperators::Parenthesis, opStack, outputQueue)) {
				return false;
			}
		} else if(token[0] == '[') {
			bracketCount++;
			opStack.push(EvalOperators::Bracket);
			precedenceStack.push(0);
		} else if(token[0] == ']') {
			bracketCount--;
			if(!ProcessSpecialOperator(EvalOperators::Bracket, opStack, outputQueue)) {
				return false;
			}
		} else if(token[0] == '{') {
			braceCount++;
			opStack.push(EvalOperators::Braces);
			precedenceStack.push(0);
		} else if(token[0] == '}') {
			braceCount--;
			if(!ProcessSpecialOperator(EvalOperators::Braces, opStack, outputQueue)){
				return false;
			}
		} else {
			outputQueue.push_back(std::stoi(token));
		}
	}

	if(braceCount || bracketCount || parenthesisCount) {
		//Mismatching number of brackets/braces/parenthesis
		return false;
	}

	while(!opStack.empty()) {
		outputQueue.push_back(opStack.top());
		opStack.pop();
	}

	return true;
}

int32_t ExpressionEvaluator::EvaluateExpression(vector<int> *outputQueue, DebugState &state, EvalResultType &resultType, int16_t memoryValue, uint32_t memoryAddr)
{
	int pos = 0;
	int right = 0;
	int left = 0;
	int operandStack[1000];
	resultType = EvalResultType::Numeric;

	for(size_t i = 0, len = outputQueue->size(); i < len; i++) {
		int token = (*outputQueue)[i];

		if(token >= EvalValues::RegA) {
			//Replace value with a special value
			switch(token) {
				case EvalValues::RegA: token = state.CPU.A; break;
				case EvalValues::RegX: token = state.CPU.X; break;
				case EvalValues::RegY: token = state.CPU.Y; break;
				case EvalValues::RegPS: token = state.CPU.PS; break;
				case EvalValues::RegSP: token = state.CPU.SP; break;
				case EvalValues::Irq: token = state.CPU.IRQFlag; break;
				case EvalValues::Nmi: token = state.CPU.NMIFlag; break;
				case EvalValues::PpuCycle: token = state.PPU.Cycle; break;
				case EvalValues::PpuScanline: token = state.PPU.Scanline; break;
				case EvalValues::Value: token = memoryValue; break;
				case EvalValues::Address: token = memoryAddr; break;
				case EvalValues::AbsoluteAddress: token = _debugger->GetAbsoluteAddress(memoryAddr); break;
			}
		} else if(token >= EvalOperators::Multiplication) {
			right = operandStack[--pos];
			if(pos > 0 && token <= EvalOperators::LogicalOr) {
				//Only do this for binary operators
				left = operandStack[--pos];
			}

			resultType = EvalResultType::Numeric;
			switch(token) {
				case EvalOperators::Multiplication: token = left * right; break;
				case EvalOperators::Division: token = left / right; break;
				case EvalOperators::Modulo: token = left % right; break;
				case EvalOperators::Addition: token = left + right; break;
				case EvalOperators::Substration: token = left - right; break;
				case EvalOperators::ShiftLeft: token = left << right; break;
				case EvalOperators::ShiftRight: token = left >> right; break;
				case EvalOperators::SmallerThan: token = left < right; resultType = EvalResultType::Boolean; break;
				case EvalOperators::SmallerOrEqual: token = left <= right; resultType = EvalResultType::Boolean; break;
				case EvalOperators::GreaterThan: token = left > right; resultType = EvalResultType::Boolean; break;
				case EvalOperators::GreaterOrEqual: token = left >= right; resultType = EvalResultType::Boolean; break;
				case EvalOperators::Equal: token = left == right; resultType = EvalResultType::Boolean; break;
				case EvalOperators::NotEqual: token = left != right; resultType = EvalResultType::Boolean; break;
				case EvalOperators::BinaryAnd: token = left & right; break;
				case EvalOperators::BinaryXor: token = left | right; break;
				case EvalOperators::BinaryOr: token = left ^ right; break;
				case EvalOperators::LogicalAnd: token = left && right; resultType = EvalResultType::Boolean; break;
				case EvalOperators::LogicalOr: token = left || right; resultType = EvalResultType::Boolean; break;

				//Unary operators
				case EvalOperators::Bracket: token = _debugger->GetMemoryValue(right); break;
				case EvalOperators::Braces: token = _debugger->GetMemoryValue(right) | (_debugger->GetMemoryValue(right+1) << 8); break;
				case EvalOperators::Plus: token = right; break;
				case EvalOperators::Minus: token = -right; break;
				case EvalOperators::BinaryNot: token = ~right; break;
				case EvalOperators::LogicalNot: token = !right; break;
				default: throw std::runtime_error("Invalid operator");
			}
		}
		operandStack[pos++] = token;
	}
	return operandStack[0];
}

ExpressionEvaluator::ExpressionEvaluator(Debugger* debugger)
{
	_debugger = debugger;
}

int32_t ExpressionEvaluator::PrivateEvaluate(string expression, DebugState &state, EvalResultType &resultType, int16_t memoryValue, uint32_t memoryAddr, bool& success)
{
	success = true;
	vector<int> output;
	vector<int> *outputQueue = nullptr;

	{
		LockHandler lock = _cacheLock.AcquireSafe();

		auto cacheOutputQueue = _outputCache.find(expression);
		if(cacheOutputQueue != _outputCache.end()) {
			outputQueue = &(cacheOutputQueue->second);
		}
	}
	
	if(outputQueue == nullptr && output.empty()) {
		string fixedExp = expression;
		fixedExp.erase(std::remove(fixedExp.begin(), fixedExp.end(), ' '), fixedExp.end());
		success = ToRpn(fixedExp, output);

		if(success) {
			if(_containsCustomLabels) {
				outputQueue = &output;
			} else {
				LockHandler lock = _cacheLock.AcquireSafe();
				_outputCache[expression] = output;
				outputQueue = &_outputCache[expression];
			}
		} else {
			return 0;
		}
	}

	if(outputQueue) {
		return EvaluateExpression(outputQueue, state, resultType, memoryValue, memoryAddr);
	} else {
		return EvaluateExpression(&output, state, resultType, memoryValue, memoryAddr);
	}
}

int32_t ExpressionEvaluator::Evaluate(string expression, DebugState &state, int16_t memoryValue, uint32_t memoryAddr)
{
	EvalResultType resultType;
	return Evaluate(expression, state, resultType, memoryValue, memoryAddr);
}

int32_t ExpressionEvaluator::Evaluate(string expression, DebugState &state, EvalResultType &resultType, int16_t memoryValue, uint32_t memoryAddr)
{
	try {
		bool success;
		int32_t result = PrivateEvaluate(expression, state, resultType, memoryValue, memoryAddr, success);
		if(success) {
			return result;
		}
	} catch(std::exception e) {
	}
	resultType = EvalResultType::Invalid;
	return 0;

}

bool ExpressionEvaluator::Validate(string expression)
{
	try {
		DebugState state;
		EvalResultType type;
		bool success;
		PrivateEvaluate(expression, state, type, 0, 0, success);
		return success;
	} catch(std::exception e) {
		return false;
	}
}