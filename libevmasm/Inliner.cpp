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
 * @file Inliner.cpp
 * Inlines small code snippets by replacing JUMP with a copy of the code jumped to.
 */

#include <libevmasm/Inliner.h>

#include <libevmasm/AssemblyItem.h>
#include <libevmasm/SemanticInformation.h>

#include <libsolutil/CommonData.h>

#include <range/v3/numeric/accumulate.hpp>
#include <range/v3/view/drop_last.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/slice.hpp>
#include <range/v3/view/transform.hpp>

#include <optional>


using namespace std;
using namespace solidity;
using namespace solidity::evmasm;

bool Inliner::isInlineCandidate(u256 const& _tag, InlinableBlock const& _block) const
{
	assertThrow(_block.items.size() > 0, OptimizerException, "");

	// Never inline tags that reference themselves.
	for (AssemblyItem const& item: _block.items)
		if (item.type() == PushTag && _tag == item.data())
				return false;

	return true;
}

map<u256, Inliner::InlinableBlock> Inliner::determineInlinableBlocks(AssemblyItems const& _items) const
{
	std::map<u256, ranges::span<AssemblyItem const>> inlinableBlockItems;
	std::map<u256, uint64_t> numPushTags;
	std::optional<size_t> lastTag;
	for (auto&& [index, item]: _items | ranges::views::enumerate)
	{
		// The number of PushTags approximates the number of calls to a block.
		if (item.type() == PushTag)
			numPushTags[item.data()]++;

		// We can only inline blocks with straight control flow that end in a jump.
		// Using breaksCSEAnalysisBlock will hopefully allow the return jump to be optimized after inlining.
		if (lastTag && SemanticInformation::breaksCSEAnalysisBlock(item, false))
		{
			if (item == Instruction::JUMP)
				inlinableBlockItems[_items[*lastTag].data()] = _items | ranges::views::slice(*lastTag + 1, index + 1);
			lastTag.reset();
		}

		if (item.type() == Tag)
			lastTag = index;
	}

	// Filter candidates for general inlinability and store the number of PushTags alongside the assembly items.
	map<u256, InlinableBlock> result;
	for (auto&& [tag, items]: inlinableBlockItems)
		if (uint64_t const* numPushes = util::valueOrNullptr(numPushTags, tag))
		{
			InlinableBlock block{items, *numPushes};
			if (isInlineCandidate(tag, block))
				result.emplace(tag, block);
		}
	return result;
}

optional<AssemblyItem> Inliner::shouldInline(u256 const&, AssemblyItem const& _jump, InlinableBlock const& _block) const
{
	AssemblyItem exitJump = _block.items.back();

	if (
		_jump.getJumpType() == AssemblyItem::JumpType::IntoFunction &&
		exitJump.getJumpType() == AssemblyItem::JumpType::OutOfFunction
	)
	{
		exitJump.setJumpType(AssemblyItem::JumpType::Ordinary);
		// Accumulate size of inlined block in bytes.
		size_t codeSize = ranges::accumulate(
			ranges::views::drop_last(_block.items, 1) |
			ranges::views::transform(
				[](auto const& _item) { return _item.bytesRequired(3); }
			), 0u);
		// Use the number of push tags as approximation of the number of calls to the function.
		uint64_t numberOfCalls = _block.pushTagCount;
		// Without inlining the execution of each call consists of two PushTags, two Jumps and two tags, totaling 24 gas.
		bigint uninlinedExecutionCost = bigint(m_runs) * 24u * numberOfCalls;
		// For each call two PushTags, one Jump and one tag are inserted in the code, totalling 8 bytes per call.
		// Additionally the function body itself together with another tag and a return jump occur once.
		bigint uninlinedDepositCost = (8u * _block.pushTagCount + 2 + codeSize) * 200u;
		// When inlining the execution cost beyond the actual function execution is zero,
		// but for each call a copy of the function is stored.
		bigint inlinedCost = _block.pushTagCount * codeSize * 200;

		if (uninlinedExecutionCost + uninlinedDepositCost > inlinedCost)
			return exitJump;
	}

	return nullopt;
}


void Inliner::optimise()
{
	std::map<u256, InlinableBlock> inlinableBlocks = determineInlinableBlocks(m_items);

	if (inlinableBlocks.empty())
		return;

	AssemblyItems newItems;
	for (auto it = m_items.begin(); it != m_items.end(); ++it)
	{
		AssemblyItem const& item = *it;
		if (next(it) != m_items.end())
		{
			AssemblyItem const& nextItem = *next(it);
			if (item.type() == PushTag && nextItem == Instruction::JUMP)
				if (auto* inlinableBlock = util::valueOrNullptr(inlinableBlocks, item.data()))
					if (auto exitJump = shouldInline(item.data(), nextItem, *inlinableBlock))
					{
						newItems += ranges::views::drop_last(inlinableBlock->items, 1);
						newItems.emplace_back(*exitJump);

						// We are removing one push tag to the block we inline.
						--inlinableBlock->pushTagCount;
						// We might increase the number of push tags to other blocks.
						for (AssemblyItem const& inlinedItem: inlinableBlock->items)
							if (inlinedItem.type() == PushTag)
								if (auto* block = util::valueOrNullptr(inlinableBlocks, inlinedItem.data()))
									++block->pushTagCount;

						// Skip the original jump to the inlined tag and continue.
						++it;
						continue;
					}
		}
		newItems.emplace_back(item);
	}

	m_items = move(newItems);
}
