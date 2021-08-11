/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Optimiser component that removes stores to memory and storage slots that are not used
 * or overwritten later on.
 */

#include <libyul/optimiser/RedundantStoreEliminator.h>

#include <libyul/optimiser/Semantics.h>
#include <libyul/optimiser/OptimizerUtilities.h>
#include <libyul/optimiser/Semantics.h>
#include <libyul/optimiser/SSAValueTracker.h>
#include <libyul/optimiser/DataFlowAnalyzer.h>
#include <libyul/optimiser/KnowledgeBase.h>
#include <libyul/AST.h>

#include <libsolutil/CommonData.h>

#include <libevmasm/Instruction.h>
#include <libevmasm/SemanticInformation.h>

#include <range/v3/algorithm/all_of.hpp>

using namespace std;
using namespace solidity;
using namespace solidity::yul;

/// Variable names for special constants that can never appear in actual Yul code.
static string const zero{"@ 0"};
static string const one{"@ 1"};
static string const thirtyTwo{"@ 32"};


void RedundantStoreEliminator::run(OptimiserStepContext& _context, Block& _ast)
{
	map<YulString, SideEffects> functionSideEffects = SideEffectsPropagator::sideEffects(
		_context.dialect,
		CallGraphGenerator::callGraph(_ast)
	);
	SSAValueTracker ssaValues;
	ssaValues(_ast);
	map<YulString, AssignedValue> values;
	for (auto const& [name, expression]: ssaValues.values())
		values[name] = AssignedValue{expression, {}};
	Expression const zeroLiteral{Literal{{}, LiteralKind::Number, YulString{"0"}, {}}};
	Expression const oneLiteral{Literal{{}, LiteralKind::Number, YulString{"1"}, {}}};
	Expression const thirtyTwoLiteral{Literal{{}, LiteralKind::Number, YulString{"32"}, {}}};
	values[YulString{zero}] = AssignedValue{&zeroLiteral, {}};
	values[YulString{one}] = AssignedValue{&oneLiteral, {}};
	values[YulString{thirtyTwo}] = AssignedValue{&thirtyTwoLiteral, {}};

	bool const ignoreMemory = MSizeFinder::containsMSize(_context.dialect, _ast);
	RedundantStoreEliminator rse{_context.dialect, functionSideEffects, values, ignoreMemory};
	rse(_ast);
	rse.changeUndecidedTo(State::Unused, Location::Memory);
	rse.changeUndecidedTo(State::Used, Location::Storage);
	rse.scheduleUnusedForDeletion();

	StatementRemover remover(rse.m_pendingRemovals);
	remover(_ast);
}

void RedundantStoreEliminator::operator()(FunctionCall const& _functionCall)
{
	RedundantStoreBase::operator()(_functionCall);

	for (Operation const& op: operationsFromFunctionCall(_functionCall))
		applyOperation(op);

	// TODO handle reverts of user-defined functions

	if (BuiltinFunction const* f = m_dialect.builtin(_functionCall.functionName.name))
		if (f->controlFlowSideEffects.terminates)
		{
			changeUndecidedTo(State::Unused, Location::Memory);
			changeUndecidedTo(
				f->controlFlowSideEffects.reverts ?
				State::Unused :
				State::Used,
				Location::Storage
			);
		}
}

void RedundantStoreEliminator::operator()(FunctionDefinition const& _functionDefinition)
{
	ScopedSaveAndRestore storeOperations(m_storeOperations, {});
	RedundantStoreBase::operator()(_functionDefinition);
}


void RedundantStoreEliminator::operator()(Leave const&)
{
	changeUndecidedTo(State::Used);
}

void RedundantStoreEliminator::visit(Statement const& _statement)
{
	RedundantStoreBase::visit(_statement);

	auto const* exprStatement = get_if<ExpressionStatement>(&_statement);
	if (!exprStatement)
		return;

	FunctionCall const* funCall = get_if<FunctionCall>(&exprStatement->expression);
	if (!funCall)
		return;
	optional<evmasm::Instruction> instruction = toEVMInstruction(m_dialect, funCall->functionName.name);
	if (!instruction)
		return;

	if (!ranges::all_of(funCall->arguments, [](Expression const& _expr) -> bool {
		return get_if<Identifier>(&_expr) || get_if<Literal>(&_expr);
	}))
		return;

	if (
		*instruction == evmasm::Instruction::SSTORE ||
		(!m_ignoreMemory && (
			*instruction == evmasm::Instruction::EXTCODECOPY ||
			*instruction == evmasm::Instruction::CODECOPY ||
			*instruction == evmasm::Instruction::CALLDATACOPY ||
			*instruction == evmasm::Instruction::RETURNDATACOPY ||
			*instruction == evmasm::Instruction::MSTORE ||
			*instruction == evmasm::Instruction::MSTORE8
		))
	)
	{
		m_stores[YulString{}].insert({&_statement, State::Undecided});
		vector<Operation> operations = operationsFromFunctionCall(*funCall);
		yulAssert(operations.size() == 1, "");
		m_storeOperations[&_statement] = move(operations.front());
	}
}

vector<RedundantStoreEliminator::Operation> RedundantStoreEliminator::operationsFromFunctionCall(
	FunctionCall const& _functionCall
) const
{
	using evmasm::Instruction;

	YulString functionName = _functionCall.functionName.name;
	SideEffects sideEffects;
	if (BuiltinFunction const* f = m_dialect.builtin(functionName))
		sideEffects = f->sideEffects;
	else
		sideEffects = m_functionSideEffects.at(_functionCall.functionName.name);

	if (optional<evmasm::Instruction> instruction = toEVMInstruction(m_dialect, functionName))
	{
		switch (*instruction)
		{
		case Instruction::SSTORE:
		case Instruction::SLOAD:
		case Instruction::MSTORE:
		case Instruction::MSTORE8:
		case Instruction::REVERT:
		case Instruction::RETURN:
		case Instruction::EXTCODECOPY:
		case Instruction::CODECOPY:
		case Instruction::CALLDATACOPY:
		case Instruction::RETURNDATACOPY:
		case Instruction::KECCAK256:
		case Instruction::LOG0:
		case Instruction::LOG1:
		case Instruction::LOG2:
		case Instruction::LOG3:
		case Instruction::LOG4:
		{
			Operation op;
			if (sideEffects.memory == SideEffects::Write || sideEffects.storage == SideEffects::Write)
				op.effect = Effect::Write;
			else
				op.effect = Effect::Read;

			op.location =
				(instruction == Instruction::SSTORE || instruction == Instruction::SLOAD) ?
				Location::Storage :
				Location::Memory;

			if (*instruction == Instruction::EXTCODECOPY)
				op.start = identifierIfSSA(_functionCall.arguments.at(1));
			else
				op.start = identifierIfSSA(_functionCall.arguments.at(0));

			if (instruction == evmasm::Instruction::MSTORE || instruction == evmasm::Instruction::MLOAD)
				op.length = YulString(thirtyTwo);
			else if (instruction == evmasm::Instruction::MSTORE8)
				op.length = YulString(one);
			else if (
				instruction == evmasm::Instruction::REVERT ||
				instruction == evmasm::Instruction::RETURN ||
				instruction == evmasm::Instruction::KECCAK256 ||
				instruction == evmasm::Instruction::LOG0 ||
				instruction == evmasm::Instruction::LOG1 ||
				instruction == evmasm::Instruction::LOG2 ||
				instruction == evmasm::Instruction::LOG3 ||
				instruction == evmasm::Instruction::LOG4
			)
				op.length = identifierIfSSA(_functionCall.arguments.at(1));
			else if (*instruction == Instruction::EXTCODECOPY)
				op.length = identifierIfSSA(_functionCall.arguments.at(3));
			else if (
				instruction == evmasm::Instruction::CALLDATACOPY ||
				instruction == evmasm::Instruction::CODECOPY ||
				instruction == evmasm::Instruction::RETURNDATACOPY
			)
				op.length = identifierIfSSA(_functionCall.arguments.at(2));
			else
				op.length = {};
			return {op};
		}
		case Instruction::STATICCALL:
		case Instruction::CALL:
		case Instruction::CALLCODE:
		case Instruction::DELEGATECALL:
		{
			size_t arguments = _functionCall.arguments.size();
			return vector<Operation>{
				Operation{
					Location::Memory,
					Effect::Read,
					identifierIfSSA(_functionCall.arguments.at(arguments - 4)),
					identifierIfSSA(_functionCall.arguments.at(arguments - 3))
				},
				// Unknown read includes unknown write.
				Operation{Location::Storage, Effect::Read, {}, {}},
				Operation{
					Location::Memory,
					Effect::Write,
					identifierIfSSA(_functionCall.arguments.at(arguments - 2)),
					identifierIfSSA(_functionCall.arguments.at(arguments - 1))
				}
			};
		}
		case Instruction::CREATE:
		case Instruction::CREATE2:
			return vector<Operation>{
				Operation{
					Location::Memory,
					Effect::Read,
					identifierIfSSA(_functionCall.arguments.at(1)),
					identifierIfSSA(_functionCall.arguments.at(2))
				}
			};
		default:
			break;
		}
	}

	vector<Operation> result;
	if (sideEffects.memory != SideEffects::Effect::None)
		result.emplace_back(Operation{Location::Memory, Effect::Read, {}, {}});
	if (sideEffects.storage != SideEffects::Effect::None)
		result.emplace_back(Operation{Location::Storage, Effect::Read, {}, {}});

	return result;
}

void RedundantStoreEliminator::applyOperation(RedundantStoreEliminator::Operation const& _operation)
{
	for (auto& [statement, state]: m_stores[YulString{}])
		if (state == State::Undecided)
		{
			Operation const& storeOperation = m_storeOperations.at(statement);
			if (_operation.effect == Effect::Read && !knownUnrelated(storeOperation, _operation))
				state = State::Used;
			else if (_operation.effect == Effect::Write && knownCovered(storeOperation, _operation))
				state = State::Unused;
		}
}

bool RedundantStoreEliminator::knownUnrelated(
	RedundantStoreEliminator::Operation const& _op1,
	RedundantStoreEliminator::Operation const& _op2
) const
{
	KnowledgeBase knowledge(m_dialect, m_ssaValues);

	if (_op1.location != _op2.location)
		return true;
	if (_op1.location == Location::Storage)
	{
		if (_op1.start && _op2.start)
			return knowledge.knownToBeDifferent(*_op1.start, *_op2.start);
	}
	else
	{
		u256 largestPositive = (u256(1) << 128) - 1;

		yulAssert(_op1.location == Location::Memory, "");
		if (_op1.length && knowledge.knownToBeZero(*_op1.length))
			return true;
		if (_op2.length && knowledge.knownToBeZero(*_op2.length))
			return true;
		if (!_op1.start || !_op2.start || !_op1.length || !_op2.length)
			// Can we say anything else here?
			return false;

		// 1.start + 1.length <= 2.start ||
		if (optional<u256> length1 = knowledge.knownConstant(*_op1.length))
			if (optional<u256> diff = knowledge.differenceIsKnownConstant(*_op2.start, *_op1.start))
				// diff = 2.start - 1.start
				if (*length1 <= *diff && *length1 <= largestPositive && *diff <= largestPositive)
					return true;
		// 2.start + 2.length <= 1.start
		if (optional<u256> length2 = knowledge.knownConstant(*_op2.length))
			if (optional<u256> diff = knowledge.differenceIsKnownConstant(*_op1.start, *_op2.start))
				// diff = 1.start - 2.start
				if (*length2 <= *diff && *length2 <= largestPositive && *diff <= largestPositive)
					return true;
	}

	return false;
}

bool RedundantStoreEliminator::knownCovered(
	RedundantStoreEliminator::Operation const& _covered,
	RedundantStoreEliminator::Operation const& _covering
) const
{
	if (_covered.location != _covering.location)
		return false;
	if (
		(_covered.start && _covered.start == _covering.start) &&
		(_covered.location == Location::Storage || (_covered.length && _covered.length == _covering.length))
	)
		return true;
	if (_covered.location == Location::Memory)
	{
		KnowledgeBase knowledge(m_dialect, m_ssaValues);
		u256 largestPositive = (u256(1) << 128) - 1;

		if (_covered.length && knowledge.knownToBeZero(*_covered.length))
			return true;
		// Condition (i = cover_i_ng, e = cover_e_d):
		// i.start <= e.start && e.start + e.length <= i.start + i.length
		if (!_covered.start || !_covering.start || !_covered.length || !_covering.length)
			return false;
		optional<u256> startDiff = knowledge.differenceIsKnownConstant(*_covered.start, *_covering.start);
		optional<u256> coveredLength = knowledge.knownConstant(*_covered.length);
		optional<u256> lengthDiff = knowledge.differenceIsKnownConstant(*_covering.length, *_covered.length);
		if (
			(startDiff && coveredLength && lengthDiff) &&
			// i.start <= e.start
			*startDiff <= largestPositive &&
			// e.length <= i.length
			*lengthDiff <= largestPositive &&
			// e.start - i.start <= i.length - e.length  <=> e.start + e.length <= i.start + i.length
			*startDiff <= *lengthDiff &&
			// Just a safety measure against overflow.
			*coveredLength <= largestPositive
		)
			return true;
	}
	return false;
}

void RedundantStoreEliminator::changeUndecidedTo(
	State _newState,
	optional<RedundantStoreEliminator::Location> _onlyLocation)
{
	for (auto& [statement, state]: m_stores[YulString{}])
		if (
			state == State::Undecided &&
			(_onlyLocation == nullopt || *_onlyLocation == m_storeOperations.at(statement).location)
		)
			state = _newState;
}

optional<YulString> RedundantStoreEliminator::identifierIfSSA(Expression const& _expression) const
{
	if (Identifier const* identifier = get_if<Identifier>(&_expression))
		if (m_ssaValues.count(identifier->name))
			return {identifier->name};
	return nullopt;
}

void RedundantStoreEliminator::scheduleUnusedForDeletion()
{
	for (auto const& [statement, state]: m_stores[YulString{}])
		if (state == State::Unused)
			m_pendingRemovals.insert(statement);
}
