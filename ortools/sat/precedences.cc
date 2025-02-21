// Copyright 2010-2022 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/sat/precedences.h"

#include <stdint.h>

#include <algorithm>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/meta/type_traits.h"
#include "absl/types/span.h"
#include "ortools/base/cleanup.h"
#include "ortools/base/logging.h"
#include "ortools/base/stl_util.h"
#include "ortools/base/strong_vector.h"
#include "ortools/graph/topologicalsorter.h"
#include "ortools/sat/clause.h"
#include "ortools/sat/cp_constraints.h"
#include "ortools/sat/integer.h"
#include "ortools/sat/model.h"
#include "ortools/sat/sat_base.h"
#include "ortools/sat/sat_solver.h"
#include "ortools/sat/synchronization.h"
#include "ortools/util/bitset.h"
#include "ortools/util/strong_integers.h"
#include "ortools/util/time_limit.h"

namespace operations_research {
namespace sat {

void PrecedenceRelations::Add(IntegerVariable tail, IntegerVariable head,
                              IntegerValue offset) {
  // In some case we load new linear constraint as part of the linear
  // relaxation. We just ignore anything after the first
  // ComputeFullPrecedences() call.
  if (is_built_) return;

  // Ignore trivial relation: tail + offset <= head.
  if (integer_trail_->UpperBound(tail) + offset <=
      integer_trail_->LowerBound(head)) {
    return;
  }
  if (PositiveVariable(tail) == PositiveVariable(head)) return;

  // TODO(user): Remove once we support non-DAG.
  if (offset < 0) return;

  graph_.AddArc(tail.value(), head.value());
  graph_.AddArc(NegationOf(head).value(), NegationOf(tail).value());
  arc_offset_.push_back(offset);
  arc_offset_.push_back(offset);
}

void PrecedenceRelations::Build() {
  if (is_built_) return;

  is_built_ = true;
  std::vector<int> permutation;
  graph_.Build(&permutation);
  util::Permute(permutation, &arc_offset_);

  // Is it a DAG?
  // Get a topological order of the DAG formed by all the arcs that are present.
  //
  // TODO(user): This can fail if we don't have a DAG. We could just skip Bad
  // edges instead, and have a sub-DAG as an heuristic. Or analyze the arc
  // weight and make sure cycle are not an issue. We can also start with arcs
  // with strictly positive weight.
  //
  // TODO(user): Only explore the sub-graph reachable from "vars".
  const int num_nodes = graph_.num_nodes();
  DenseIntStableTopologicalSorter sorter(num_nodes);
  for (int arc = 0; arc < graph_.num_arcs(); ++arc) {
    sorter.AddEdge(graph_.Tail(arc), graph_.Head(arc));
  }
  int next;
  bool graph_has_cycle = false;
  topological_order_.clear();
  while (sorter.GetNext(&next, &graph_has_cycle, nullptr)) {
    topological_order_.push_back(IntegerVariable(next));
    if (graph_has_cycle) {
      is_dag_ = false;
      return;
    }
  }
  is_dag_ = !graph_has_cycle;

  // Lets build full precedences if we don't have too many of them.
  // TODO(user): Also do that if we don't have a DAG?
  if (!is_dag_) return;

  int work = 0;
  const int kWorkLimit = 1e6;
  absl::StrongVector<IntegerVariable, std::vector<IntegerVariable>> before(
      graph_.num_nodes());
  const auto add = [&before, this](IntegerVariable a, IntegerVariable b,
                                   IntegerValue offset) {
    const auto [it, inserted] = all_relations_.insert({{a, b}, offset});
    if (inserted) {
      before[b].push_back(a);
    } else {
      it->second = std::max(it->second, offset);
    }
  };

  // TODO(user): We probably do not need to do both var and its negation.
  for (const IntegerVariable tail_var : topological_order_) {
    if (++work > kWorkLimit) break;
    for (const int arc : graph_.OutgoingArcs(tail_var.value())) {
      CHECK_EQ(tail_var.value(), graph_.Tail(arc));
      const IntegerVariable head_var = IntegerVariable(graph_.Head(arc));
      const IntegerValue arc_offset = arc_offset_[arc];

      if (++work > kWorkLimit) break;
      add(tail_var, head_var, arc_offset);
      add(NegationOf(head_var), NegationOf(tail_var), -arc_offset);

      for (const IntegerVariable before_var : before[tail_var]) {
        if (++work > kWorkLimit) break;
        const IntegerValue offset =
            all_relations_.at({before_var, tail_var}) + arc_offset;
        add(before_var, head_var, offset);
        add(NegationOf(head_var), NegationOf(before_var), -offset);
      }
    }
  }

  VLOG(2) << "Full precedences. Work=" << work
          << " Relations=" << all_relations_.size();
}

void PrecedenceRelations::ComputeFullPrecedences(
    const std::vector<IntegerVariable>& vars,
    std::vector<FullIntegerPrecedence>* output) {
  output->clear();
  if (!is_built_) Build();
  if (!is_dag_) return;

  VLOG(2) << "num_nodes: " << graph_.num_nodes()
          << " num_arcs: " << graph_.num_arcs() << " is_dag: " << is_dag_;

  // Compute all precedences.
  // We loop over the node in topological order, and we maintain for all
  // variable we encounter, the list of "to_consider" variables that are before.
  //
  // TODO(user): use vector of fixed size.
  absl::flat_hash_set<IntegerVariable> is_interesting;
  absl::flat_hash_set<IntegerVariable> to_consider(vars.begin(), vars.end());
  absl::flat_hash_map<IntegerVariable,
                      absl::flat_hash_map<IntegerVariable, IntegerValue>>
      vars_before_with_offset;
  absl::flat_hash_map<IntegerVariable, IntegerValue> tail_map;
  for (const IntegerVariable tail_var : topological_order_) {
    if (!to_consider.contains(tail_var) &&
        !vars_before_with_offset.contains(tail_var)) {
      continue;
    }

    // We copy the data for tail_var here, because the pointer is not stable.
    // TODO(user): optimize when needed.
    tail_map.clear();
    {
      const auto it = vars_before_with_offset.find(tail_var);
      if (it != vars_before_with_offset.end()) {
        tail_map = it->second;
      }
    }

    for (const int arc : graph_.OutgoingArcs(tail_var.value())) {
      CHECK_EQ(tail_var.value(), graph_.Tail(arc));
      const IntegerVariable head_var = IntegerVariable(graph_.Head(arc));
      const IntegerValue arc_offset = arc_offset_[arc];

      // No need to create an empty entry in this case.
      if (tail_map.empty() && !to_consider.contains(tail_var)) continue;

      auto& to_update = vars_before_with_offset[head_var];
      for (const auto& [var_before, offset] : tail_map) {
        if (!to_update.contains(var_before)) {
          to_update[var_before] = arc_offset + offset;
        } else {
          to_update[var_before] =
              std::max(arc_offset + offset, to_update[var_before]);
        }
      }
      if (to_consider.contains(tail_var)) {
        if (!to_update.contains(tail_var)) {
          to_update[tail_var] = arc_offset;
        } else {
          to_update[tail_var] = std::max(arc_offset, to_update[tail_var]);
        }
      }

      // Small filtering heuristic: if we have (before) < tail, and tail < head,
      // we really do not need to list (before, tail) < head. We only need that
      // if the list of variable before head contains some variable that are not
      // already before tail.
      if (to_update.size() > tail_map.size() + 1) {
        is_interesting.insert(head_var);
      } else {
        is_interesting.erase(head_var);
      }
    }

    // Extract the output for tail_var. Because of the topological ordering, the
    // data for tail_var is already final now.
    //
    // TODO(user): Release the memory right away.
    if (!is_interesting.contains(tail_var)) continue;
    if (tail_map.size() == 1) continue;

    FullIntegerPrecedence data;
    data.var = tail_var;
    IntegerValue min_offset = kMaxIntegerValue;
    for (int i = 0; i < vars.size(); ++i) {
      const auto offset_it = tail_map.find(vars[i]);
      if (offset_it == tail_map.end()) continue;
      data.indices.push_back(i);
      data.offsets.push_back(offset_it->second);
      min_offset = std::min(data.offsets.back(), min_offset);
    }
    output->push_back(std::move(data));
  }
}

namespace {

void AppendLowerBoundReasonIfValid(IntegerVariable var,
                                   const IntegerTrail& i_trail,
                                   std::vector<IntegerLiteral>* reason) {
  if (var != kNoIntegerVariable) {
    reason->push_back(i_trail.LowerBoundAsLiteral(var));
  }
}

}  // namespace

PrecedencesPropagator::~PrecedencesPropagator() {
  if (!VLOG_IS_ON(1)) return;
  if (shared_stats_ == nullptr) return;
  std::vector<std::pair<std::string, int64_t>> stats;
  stats.push_back({"precedences/num_cycles", num_cycles_});
  stats.push_back({"precedences/num_pushes", num_pushes_});
  stats.push_back(
      {"precedences/num_enforcement_pushes", num_enforcement_pushes_});
  shared_stats_->AddStats(stats);
}

bool PrecedencesPropagator::Propagate(Trail* trail) { return Propagate(); }

bool PrecedencesPropagator::Propagate() {
  while (propagation_trail_index_ < trail_->Index()) {
    const Literal literal = (*trail_)[propagation_trail_index_++];
    if (literal.Index() >= literal_to_new_impacted_arcs_.size()) continue;

    // IMPORTANT: Because of the way Untrail() work, we need to add all the
    // potential arcs before we can abort. It is why we iterate twice here.
    for (const ArcIndex arc_index :
         literal_to_new_impacted_arcs_[literal.Index()]) {
      if (--arc_counts_[arc_index] == 0) {
        const ArcInfo& arc = arcs_[arc_index];
        AddToConditionalRelations(arc);
        impacted_arcs_[arc.tail_var].push_back(arc_index);
      }
    }

    // Iterate again to check for a propagation and indirectly update
    // modified_vars_.
    for (const ArcIndex arc_index :
         literal_to_new_impacted_arcs_[literal.Index()]) {
      if (arc_counts_[arc_index] > 0) continue;
      const ArcInfo& arc = arcs_[arc_index];
      if (integer_trail_->IsCurrentlyIgnored(arc.head_var)) continue;
      const IntegerValue new_head_lb =
          integer_trail_->LowerBound(arc.tail_var) + ArcOffset(arc);
      if (new_head_lb > integer_trail_->LowerBound(arc.head_var)) {
        if (!EnqueueAndCheck(arc, new_head_lb, trail_)) return false;
      }
    }
  }

  // Do the actual propagation of the IntegerVariable bounds.
  InitializeBFQueueWithModifiedNodes();
  if (!BellmanFordTarjan(trail_)) return false;

  // We can only test that no propagation is left if we didn't enqueue new
  // literal in the presence of optional variables.
  //
  // TODO(user): Because of our code to deal with InPropagationLoop(), this is
  // not always true. Find a cleaner way to DCHECK() while not failing in this
  // corner case.
  if (/*DISABLES CODE*/ (false) &&
      propagation_trail_index_ == trail_->Index()) {
    DCHECK(NoPropagationLeft(*trail_));
  }

  // Propagate the presence literals of the arcs that can't be added.
  PropagateOptionalArcs(trail_);

  // Clean-up modified_vars_ to do as little as possible on the next call.
  modified_vars_.ClearAndResize(integer_trail_->NumIntegerVariables());
  return true;
}

bool PrecedencesPropagator::PropagateOutgoingArcs(IntegerVariable var) {
  CHECK_NE(var, kNoIntegerVariable);
  if (var >= impacted_arcs_.size()) return true;
  for (const ArcIndex arc_index : impacted_arcs_[var]) {
    const ArcInfo& arc = arcs_[arc_index];
    if (integer_trail_->IsCurrentlyIgnored(arc.head_var)) continue;
    const IntegerValue new_head_lb =
        integer_trail_->LowerBound(arc.tail_var) + ArcOffset(arc);
    if (new_head_lb > integer_trail_->LowerBound(arc.head_var)) {
      if (!EnqueueAndCheck(arc, new_head_lb, trail_)) return false;
    }
  }
  return true;
}

// TODO(user): Add as fixed precedence if we fix at level zero.
void PrecedencesPropagator::AddToConditionalRelations(const ArcInfo& arc) {
  if (arc.presence_literals.size() != 1) return;

  // We currently do not handle variable size in the reasons.
  // TODO(user): we could easily take a level zero ArcOffset() instead, or
  // add this to the reason though.
  if (arc.offset_var != kNoIntegerVariable) return;
  const std::pair<IntegerVariable, IntegerVariable> key = {arc.tail_var,
                                                           arc.head_var};
  const IntegerValue offset = ArcOffset(arc);

  // We only insert if it is not already present!
  conditional_relations_.insert({key, {arc.presence_literals[0], offset}});
}

void PrecedencesPropagator::RemoveFromConditionalRelations(const ArcInfo& arc) {
  if (arc.presence_literals.size() != 1) return;
  if (arc.offset_var != kNoIntegerVariable) return;
  const std::pair<IntegerVariable, IntegerVariable> key = {arc.tail_var,
                                                           arc.head_var};
  const auto it = conditional_relations_.find(key);
  if (it == conditional_relations_.end()) return;
  if (it->second.first != arc.presence_literals[0]) return;

  // It is okay if we erase a wrong one on untrail, what is important is not to
  // forget to erase one we added.
  conditional_relations_.erase(it);
}

void PrecedencesPropagator::Untrail(const Trail& trail, int trail_index) {
  if (propagation_trail_index_ > trail_index) {
    // This means that we already propagated all there is to propagate
    // at the level trail_index, so we can safely clear modified_vars_ in case
    // it wasn't already done.
    modified_vars_.ClearAndResize(integer_trail_->NumIntegerVariables());
  }
  while (propagation_trail_index_ > trail_index) {
    const Literal literal = trail[--propagation_trail_index_];
    if (literal.Index() >= literal_to_new_impacted_arcs_.size()) continue;
    for (const ArcIndex arc_index :
         literal_to_new_impacted_arcs_[literal.Index()]) {
      if (arc_counts_[arc_index]++ == 0) {
        const ArcInfo& arc = arcs_[arc_index];
        RemoveFromConditionalRelations(arc);
        impacted_arcs_[arc.tail_var].pop_back();
      }
    }
  }
}

// Instead of simply sorting the IntegerPrecedences returned by .var,
// experiments showed that it is faster to regroup all the same .var "by hand"
// by first computing how many times they appear and then apply the sorting
// permutation.
void PrecedencesPropagator::ComputePrecedences(
    const std::vector<IntegerVariable>& vars,
    std::vector<IntegerPrecedences>* output) {
  tmp_sorted_vars_.clear();
  tmp_precedences_.clear();
  for (int index = 0; index < vars.size(); ++index) {
    const IntegerVariable var = vars[index];
    CHECK_NE(kNoIntegerVariable, var);
    if (var >= impacted_arcs_.size()) continue;
    for (const ArcIndex arc_index : impacted_arcs_[var]) {
      const ArcInfo& arc = arcs_[arc_index];
      if (integer_trail_->IsCurrentlyIgnored(arc.head_var)) continue;

      IntegerValue offset = arc.offset;
      if (arc.offset_var != kNoIntegerVariable) {
        offset += integer_trail_->LowerBound(arc.offset_var);
      }

      // TODO(user): it seems better to ignore negative min offset as we will
      // often have relation of the form interval_start >= interval_end -
      // offset, and such relation are usually not useful. Revisit this in case
      // we see problems where we can propagate more without this test.
      if (offset < 0) continue;

      if (var_to_degree_[arc.head_var] == 0) {
        tmp_sorted_vars_.push_back(
            {arc.head_var, integer_trail_->LowerBound(arc.head_var)});
      } else {
        // This "seen" mechanism is needed because we may have multi-arc and we
        // don't want any duplicates in the "is_before" relation. Note that it
        // works because var_to_last_index_ is reset by the var_to_degree_ == 0
        // case.
        if (var_to_last_index_[arc.head_var] == index) continue;
      }
      var_to_last_index_[arc.head_var] = index;
      var_to_degree_[arc.head_var]++;
      tmp_precedences_.push_back(
          {index, arc.head_var, arc_index.value(), offset});
    }
  }

  // This order is a topological order for the precedences relation order
  // provided that all the offset between the involved IntegerVariable are
  // positive.
  //
  // TODO(user): use an order that is always topological? This is not clear
  // since it may be slower to compute and not worth it because the order below
  // is more natural and may work better.
  std::sort(tmp_sorted_vars_.begin(), tmp_sorted_vars_.end());

  // Permute tmp_precedences_ into the output to put it in the correct order.
  // For that we transform var_to_degree_ to point to the first position of
  // each lbvar in the output vector.
  int start = 0;
  for (const SortedVar pair : tmp_sorted_vars_) {
    const int degree = var_to_degree_[pair.var];
    if (degree > 1) {
      var_to_degree_[pair.var] = start;
      start += degree;
    } else {
      // Optimization: we remove degree one relations.
      var_to_degree_[pair.var] = -1;
    }
  }
  output->resize(start);
  for (const IntegerPrecedences& precedence : tmp_precedences_) {
    if (var_to_degree_[precedence.var] < 0) continue;
    (*output)[var_to_degree_[precedence.var]++] = precedence;
  }

  // Cleanup var_to_degree_, note that we don't need to clean
  // var_to_last_index_.
  for (const SortedVar pair : tmp_sorted_vars_) {
    var_to_degree_[pair.var] = 0;
  }
}

void PrecedencesPropagator::ComputePartialPrecedences(
    const std::vector<IntegerVariable>& vars,
    std::vector<FullIntegerPrecedence>* output) {
  output->clear();
  DCHECK_EQ(trail_->CurrentDecisionLevel(), 0);

  std::vector<PrecedencesPropagator::IntegerPrecedences> before;
  ComputePrecedences(vars, &before);

  // Convert format.
  const int size = before.size();
  for (int i = 0; i < size;) {
    FullIntegerPrecedence data;
    data.var = before[i].var;
    const IntegerVariable var = before[i].var;
    DCHECK_NE(var, kNoIntegerVariable);
    for (; i < size && before[i].var == var; ++i) {
      data.indices.push_back(before[i].index);
      data.offsets.push_back(before[i].offset);
    }
    output->push_back(std::move(data));
  }
}

void PrecedencesPropagator::AddPrecedenceReason(
    int arc_index, IntegerValue min_offset,
    std::vector<Literal>* literal_reason,
    std::vector<IntegerLiteral>* integer_reason) const {
  const ArcInfo& arc = arcs_[ArcIndex(arc_index)];
  for (const Literal l : arc.presence_literals) {
    literal_reason->push_back(l.Negated());
  }
  if (arc.offset_var != kNoIntegerVariable) {
    // Reason for ArcOffset(arc) to be >= min_offset.
    integer_reason->push_back(IntegerLiteral::GreaterOrEqual(
        arc.offset_var, min_offset - arc.offset));
  }
}

void PrecedencesPropagator::AdjustSizeFor(IntegerVariable i) {
  const int index = std::max(i.value(), NegationOf(i).value());
  if (index >= impacted_arcs_.size()) {
    // TODO(user): only watch lower bound of the relevant variable instead
    // of watching everything in [0, max_index_of_variable_used_in_this_class).
    for (IntegerVariable var(impacted_arcs_.size()); var <= index; ++var) {
      watcher_->WatchLowerBound(var, watcher_id_);
    }
    impacted_arcs_.resize(index + 1);
    impacted_potential_arcs_.resize(index + 1);
    var_to_degree_.resize(index + 1);
    var_to_last_index_.resize(index + 1);
  }
}

void PrecedencesPropagator::AddArc(
    IntegerVariable tail, IntegerVariable head, IntegerValue offset,
    IntegerVariable offset_var, absl::Span<const Literal> presence_literals) {
  AdjustSizeFor(tail);
  AdjustSizeFor(head);
  if (offset_var != kNoIntegerVariable) AdjustSizeFor(offset_var);

  // This arc is present iff all the literals here are true.
  absl::InlinedVector<Literal, 6> enforcement_literals;
  {
    for (const Literal l : presence_literals) {
      enforcement_literals.push_back(l);
    }
    if (integer_trail_->IsOptional(tail)) {
      enforcement_literals.push_back(
          integer_trail_->IsIgnoredLiteral(tail).Negated());
    }
    if (integer_trail_->IsOptional(head)) {
      enforcement_literals.push_back(
          integer_trail_->IsIgnoredLiteral(head).Negated());
    }
    if (offset_var != kNoIntegerVariable &&
        integer_trail_->IsOptional(offset_var)) {
      enforcement_literals.push_back(
          integer_trail_->IsIgnoredLiteral(offset_var).Negated());
    }
    gtl::STLSortAndRemoveDuplicates(&enforcement_literals);

    if (trail_->CurrentDecisionLevel() == 0) {
      int new_size = 0;
      for (const Literal l : enforcement_literals) {
        if (trail_->Assignment().LiteralIsTrue(Literal(l))) {
          continue;  // At true, ignore this literal.
        } else if (trail_->Assignment().LiteralIsFalse(Literal(l))) {
          return;  // At false, ignore completely this arc.
        }
        enforcement_literals[new_size++] = l;
      }
      enforcement_literals.resize(new_size);
    }
  }

  if (head == tail) {
    // A self-arc is either plain SAT or plain UNSAT or it forces something on
    // the given offset_var or presence_literal_index. In any case it could be
    // presolved in something more efficient.
    VLOG(1) << "Self arc! This could be presolved. "
            << "var:" << tail << " offset:" << offset
            << " offset_var:" << offset_var
            << " conditioned_by:" << presence_literals;
  }

  // Remove the offset_var if it is fixed.
  // TODO(user): We should also handle the case where tail or head is fixed.
  if (offset_var != kNoIntegerVariable) {
    const IntegerValue lb = integer_trail_->LevelZeroLowerBound(offset_var);
    if (lb == integer_trail_->LevelZeroUpperBound(offset_var)) {
      offset += lb;
      offset_var = kNoIntegerVariable;
    }
  }

  // Deal first with impacted_potential_arcs_/potential_arcs_.
  if (!enforcement_literals.empty()) {
    const OptionalArcIndex arc_index(potential_arcs_.size());
    potential_arcs_.push_back(
        {tail, head, offset, offset_var, enforcement_literals});
    impacted_potential_arcs_[tail].push_back(arc_index);
    impacted_potential_arcs_[NegationOf(head)].push_back(arc_index);
    if (offset_var != kNoIntegerVariable) {
      impacted_potential_arcs_[offset_var].push_back(arc_index);
    }
  }

  // Now deal with impacted_arcs_/arcs_.
  struct InternalArc {
    IntegerVariable tail_var;
    IntegerVariable head_var;
    IntegerVariable offset_var;
  };
  std::vector<InternalArc> to_add;
  if (offset_var == kNoIntegerVariable) {
    // a + offset <= b and -b + offset <= -a
    to_add.push_back({tail, head, kNoIntegerVariable});
    to_add.push_back({NegationOf(head), NegationOf(tail), kNoIntegerVariable});
  } else {
    // tail (a) and offset_var (b) are symmetric, so we add:
    // - a + b + offset <= c
    to_add.push_back({tail, head, offset_var});
    to_add.push_back({offset_var, head, tail});
    // - a - c + offset <= -b
    to_add.push_back({tail, NegationOf(offset_var), NegationOf(head)});
    to_add.push_back({NegationOf(head), NegationOf(offset_var), tail});
    // - b - c + offset <= -a
    to_add.push_back({offset_var, NegationOf(tail), NegationOf(head)});
    to_add.push_back({NegationOf(head), NegationOf(tail), offset_var});
  }
  for (const InternalArc a : to_add) {
    // Since we add a new arc, we will need to consider its tail during the next
    // propagation. Note that the size of modified_vars_ will be automatically
    // updated when new integer variables are created since we register it with
    // IntegerTrail in this class constructor.
    //
    // TODO(user): Adding arcs and then calling Untrail() before Propagate()
    // will cause this mecanism to break. Find a more robust implementation.
    //
    // TODO(user): In some rare corner case, rescanning the whole list of arc
    // leaving tail_var can make AddVar() have a quadratic complexity where it
    // shouldn't. A better solution would be to see if this new arc currently
    // propagate something, and if it does, just update the lower bound of
    // a.head_var and let the normal "is modified" mecanism handle any eventual
    // follow up propagations.
    modified_vars_.Set(a.tail_var);

    // If a.head_var is optional, we can potentially remove some literal from
    // enforcement_literals.
    const ArcIndex arc_index(arcs_.size());
    arcs_.push_back(
        {a.tail_var, a.head_var, offset, a.offset_var, enforcement_literals});
    auto& presence_literals = arcs_.back().presence_literals;
    if (integer_trail_->IsOptional(a.head_var)) {
      // TODO(user): More generally, we can remove any literal that is implied
      // by to_remove.
      const Literal to_remove =
          integer_trail_->IsIgnoredLiteral(a.head_var).Negated();
      const auto it = std::find(presence_literals.begin(),
                                presence_literals.end(), to_remove);
      if (it != presence_literals.end()) presence_literals.erase(it);
    }

    if (presence_literals.empty()) {
      impacted_arcs_[a.tail_var].push_back(arc_index);
    } else {
      for (const Literal l : presence_literals) {
        if (l.Index() >= literal_to_new_impacted_arcs_.size()) {
          literal_to_new_impacted_arcs_.resize(l.Index().value() + 1);
        }
        literal_to_new_impacted_arcs_[l.Index()].push_back(arc_index);
      }
    }

    if (trail_->CurrentDecisionLevel() == 0) {
      arc_counts_.push_back(presence_literals.size());
    } else {
      arc_counts_.push_back(0);
      for (const Literal l : presence_literals) {
        if (!trail_->Assignment().LiteralIsTrue(l)) {
          ++arc_counts_.back();
        }
      }
      CHECK(presence_literals.empty() || arc_counts_.back() > 0);
    }
  }
}

bool PrecedencesPropagator::AddPrecedenceWithOffsetIfNew(IntegerVariable i1,
                                                         IntegerVariable i2,
                                                         IntegerValue offset) {
  DCHECK_EQ(trail_->CurrentDecisionLevel(), 0);
  if (i1 < impacted_arcs_.size() && i2 < impacted_arcs_.size()) {
    for (const ArcIndex index : impacted_arcs_[i1]) {
      const ArcInfo& arc = arcs_[index];
      if (arc.head_var == i2) {
        const IntegerValue current = ArcOffset(arc);
        if (offset <= current) {
          return false;
        } else {
          // TODO(user): Modify arc in place!
        }
        break;
      }
    }
  }

  AddPrecedenceWithOffset(i1, i2, offset);
  return true;
}

// TODO(user): On jobshop problems with a lot of tasks per machine (500), this
// takes up a big chunk of the running time even before we find a solution.
// This is because, for each lower bound changed, we inspect 500 arcs even
// though they will never be propagated because the other bound is still at the
// horizon. Find an even sparser algorithm?
void PrecedencesPropagator::PropagateOptionalArcs(Trail* trail) {
  for (const IntegerVariable var : modified_vars_.PositionsSetAtLeastOnce()) {
    // The variables are not in increasing order, so we need to continue.
    if (var >= impacted_potential_arcs_.size()) continue;

    // Note that we can currently check the same ArcInfo up to 3 times, one for
    // each of the arc variables: tail, NegationOf(head) and offset_var.
    for (const OptionalArcIndex arc_index : impacted_potential_arcs_[var]) {
      const ArcInfo& arc = potential_arcs_[arc_index];
      int num_not_true = 0;
      Literal to_propagate;
      for (const Literal l : arc.presence_literals) {
        if (!trail->Assignment().LiteralIsTrue(l)) {
          ++num_not_true;
          to_propagate = l;
        }
      }
      if (num_not_true != 1) continue;
      if (trail->Assignment().LiteralIsFalse(to_propagate)) continue;

      // Test if this arc can be present or not.
      // Important arc.tail_var can be different from var here.
      const IntegerValue tail_lb = integer_trail_->LowerBound(arc.tail_var);
      const IntegerValue head_ub = integer_trail_->UpperBound(arc.head_var);
      if (tail_lb + ArcOffset(arc) > head_ub) {
        integer_reason_.clear();
        integer_reason_.push_back(
            integer_trail_->LowerBoundAsLiteral(arc.tail_var));
        integer_reason_.push_back(
            integer_trail_->UpperBoundAsLiteral(arc.head_var));
        AppendLowerBoundReasonIfValid(arc.offset_var, *integer_trail_,
                                      &integer_reason_);
        literal_reason_.clear();
        for (const Literal l : arc.presence_literals) {
          if (l != to_propagate) literal_reason_.push_back(l.Negated());
        }
        ++num_enforcement_pushes_;
        integer_trail_->EnqueueLiteral(to_propagate.Negated(), literal_reason_,
                                       integer_reason_);
      }
    }
  }
}

IntegerValue PrecedencesPropagator::ArcOffset(const ArcInfo& arc) const {
  return arc.offset + (arc.offset_var == kNoIntegerVariable
                           ? IntegerValue(0)
                           : integer_trail_->LowerBound(arc.offset_var));
}

bool PrecedencesPropagator::EnqueueAndCheck(const ArcInfo& arc,
                                            IntegerValue new_head_lb,
                                            Trail* trail) {
  ++num_pushes_;
  DCHECK_GT(new_head_lb, integer_trail_->LowerBound(arc.head_var));

  // Compute the reason for new_head_lb.
  //
  // TODO(user): do like for clause and keep the negation of
  // arc.presence_literals? I think we could change the integer.h API to accept
  // true literal like for IntegerVariable, it is really confusing currently.
  literal_reason_.clear();
  for (const Literal l : arc.presence_literals) {
    literal_reason_.push_back(l.Negated());
  }

  integer_reason_.clear();
  integer_reason_.push_back(integer_trail_->LowerBoundAsLiteral(arc.tail_var));
  AppendLowerBoundReasonIfValid(arc.offset_var, *integer_trail_,
                                &integer_reason_);

  // The code works without this block since Enqueue() below can already take
  // care of conflicts. However, it is better to deal with the conflict
  // ourselves because we can be smarter about the reason this way.
  //
  // The reason for a "precedence" conflict is always a linear reason
  // involving the tail lower_bound, the head upper bound and eventually the
  // size lower bound. Because of that, we can use the RelaxLinearReason()
  // code.
  if (new_head_lb > integer_trail_->UpperBound(arc.head_var)) {
    const IntegerValue slack =
        new_head_lb - integer_trail_->UpperBound(arc.head_var) - 1;
    integer_reason_.push_back(
        integer_trail_->UpperBoundAsLiteral(arc.head_var));
    std::vector<IntegerValue> coeffs(integer_reason_.size(), IntegerValue(1));
    integer_trail_->RelaxLinearReason(slack, coeffs, &integer_reason_);

    if (!integer_trail_->IsOptional(arc.head_var)) {
      return integer_trail_->ReportConflict(literal_reason_, integer_reason_);
    } else {
      CHECK(!integer_trail_->IsCurrentlyIgnored(arc.head_var));
      const Literal l = integer_trail_->IsIgnoredLiteral(arc.head_var);
      if (trail->Assignment().LiteralIsFalse(l)) {
        literal_reason_.push_back(l);
        return integer_trail_->ReportConflict(literal_reason_, integer_reason_);
      } else {
        integer_trail_->EnqueueLiteral(l, literal_reason_, integer_reason_);
        return true;
      }
    }
  }

  return integer_trail_->Enqueue(
      IntegerLiteral::GreaterOrEqual(arc.head_var, new_head_lb),
      literal_reason_, integer_reason_);
}

bool PrecedencesPropagator::NoPropagationLeft(const Trail& trail) const {
  const int num_nodes = impacted_arcs_.size();
  for (IntegerVariable var(0); var < num_nodes; ++var) {
    for (const ArcIndex arc_index : impacted_arcs_[var]) {
      const ArcInfo& arc = arcs_[arc_index];
      if (integer_trail_->IsCurrentlyIgnored(arc.head_var)) continue;
      if (integer_trail_->LowerBound(arc.tail_var) + ArcOffset(arc) >
          integer_trail_->LowerBound(arc.head_var)) {
        return false;
      }
    }
  }
  return true;
}

void PrecedencesPropagator::InitializeBFQueueWithModifiedNodes() {
  // Sparse clear of the queue. TODO(user): only use the sparse version if
  // queue.size() is small or use SparseBitset.
  const int num_nodes = impacted_arcs_.size();
  bf_in_queue_.resize(num_nodes, false);
  for (const int node : bf_queue_) bf_in_queue_[node] = false;
  bf_queue_.clear();
  DCHECK(std::none_of(bf_in_queue_.begin(), bf_in_queue_.end(),
                      [](bool v) { return v; }));
  for (const IntegerVariable var : modified_vars_.PositionsSetAtLeastOnce()) {
    if (var >= num_nodes) continue;
    bf_queue_.push_back(var.value());
    bf_in_queue_[var.value()] = true;
  }
}

void PrecedencesPropagator::CleanUpMarkedArcsAndParents() {
  // To be sparse, we use the fact that each node with a parent must be in
  // modified_vars_.
  const int num_nodes = impacted_arcs_.size();
  for (const IntegerVariable var : modified_vars_.PositionsSetAtLeastOnce()) {
    if (var >= num_nodes) continue;
    const ArcIndex parent_arc_index = bf_parent_arc_of_[var.value()];
    if (parent_arc_index != -1) {
      arcs_[parent_arc_index].is_marked = false;
      bf_parent_arc_of_[var.value()] = -1;
      bf_can_be_skipped_[var.value()] = false;
    }
  }
  DCHECK(std::none_of(bf_parent_arc_of_.begin(), bf_parent_arc_of_.end(),
                      [](ArcIndex v) { return v != -1; }));
  DCHECK(std::none_of(bf_can_be_skipped_.begin(), bf_can_be_skipped_.end(),
                      [](bool v) { return v; }));
}

bool PrecedencesPropagator::DisassembleSubtree(
    int source, int target, std::vector<bool>* can_be_skipped) {
  // Note that we explore a tree, so we can do it in any order, and the one
  // below seems to be the fastest.
  tmp_vector_.clear();
  tmp_vector_.push_back(source);
  while (!tmp_vector_.empty()) {
    const int tail = tmp_vector_.back();
    tmp_vector_.pop_back();
    for (const ArcIndex arc_index : impacted_arcs_[IntegerVariable(tail)]) {
      const ArcInfo& arc = arcs_[arc_index];
      if (arc.is_marked) {
        arc.is_marked = false;  // mutable.
        if (arc.head_var.value() == target) return true;
        DCHECK(!(*can_be_skipped)[arc.head_var.value()]);
        (*can_be_skipped)[arc.head_var.value()] = true;
        tmp_vector_.push_back(arc.head_var.value());
      }
    }
  }
  return false;
}

void PrecedencesPropagator::AnalyzePositiveCycle(
    ArcIndex first_arc, Trail* trail, std::vector<Literal>* must_be_all_true,
    std::vector<Literal>* literal_reason,
    std::vector<IntegerLiteral>* integer_reason) {
  must_be_all_true->clear();
  literal_reason->clear();
  integer_reason->clear();

  // Follow bf_parent_arc_of_[] to find the cycle containing first_arc.
  const IntegerVariable first_arc_head = arcs_[first_arc].head_var;
  ArcIndex arc_index = first_arc;
  std::vector<ArcIndex> arc_on_cycle;

  // Just to be safe and avoid an infinite loop we use the fact that the maximum
  // cycle size on a graph with n nodes is of size n. If we have more in the
  // code below, it means first_arc is not part of a cycle according to
  // bf_parent_arc_of_[], which should never happen.
  const int num_nodes = impacted_arcs_.size();
  while (arc_on_cycle.size() <= num_nodes) {
    arc_on_cycle.push_back(arc_index);
    const ArcInfo& arc = arcs_[arc_index];
    if (arc.tail_var == first_arc_head) break;
    arc_index = bf_parent_arc_of_[arc.tail_var.value()];
    CHECK_NE(arc_index, ArcIndex(-1));
  }
  CHECK_NE(arc_on_cycle.size(), num_nodes + 1) << "Infinite loop.";

  // Compute the reason for this cycle.
  IntegerValue sum(0);
  for (const ArcIndex arc_index : arc_on_cycle) {
    const ArcInfo& arc = arcs_[arc_index];
    sum += ArcOffset(arc);
    AppendLowerBoundReasonIfValid(arc.offset_var, *integer_trail_,
                                  integer_reason);
    for (const Literal l : arc.presence_literals) {
      literal_reason->push_back(l.Negated());
    }

    // If the cycle happens to contain optional variable not yet ignored, then
    // it is not a conflict anymore, but we can infer that these variable must
    // all be ignored. This is because since we propagated them even if they
    // where not present for sure, their presence literal must form a cycle
    // together (i.e. they are all absent or present at the same time).
    if (integer_trail_->IsOptional(arc.head_var)) {
      must_be_all_true->push_back(
          integer_trail_->IsIgnoredLiteral(arc.head_var));
    }
  }

  // TODO(user): what if the sum overflow? this is just a check so I guess
  // we don't really care, but fix the issue.
  CHECK_GT(sum, 0);
}

// Note that in our settings it is important to use an algorithm that tries to
// minimize the number of integer_trail_->Enqueue() as much as possible.
//
// TODO(user): The current algorithm is quite efficient, but there is probably
// still room for improvements.
bool PrecedencesPropagator::BellmanFordTarjan(Trail* trail) {
  const int num_nodes = impacted_arcs_.size();

  // These vector are reset by CleanUpMarkedArcsAndParents() so resize is ok.
  bf_can_be_skipped_.resize(num_nodes, false);
  bf_parent_arc_of_.resize(num_nodes, ArcIndex(-1));
  const auto cleanup =
      ::absl::MakeCleanup([this]() { CleanUpMarkedArcsAndParents(); });

  // The queue initialization is done by InitializeBFQueueWithModifiedNodes().
  while (!bf_queue_.empty()) {
    const int node = bf_queue_.front();
    bf_queue_.pop_front();
    bf_in_queue_[node] = false;

    // TODO(user): we don't need bf_can_be_skipped_ since we can detect this
    // if this node has a parent arc which is not marked. Investigate if it is
    // faster without the vector<bool>.
    //
    // TODO(user): An alternative algorithm is to remove all these nodes from
    // the queue instead of simply marking them. This should also lead to a
    // better "relaxation" order of the arcs. It is however a bit more work to
    // remove them since we need to track their position.
    if (bf_can_be_skipped_[node]) {
      DCHECK_NE(bf_parent_arc_of_[node], -1);
      DCHECK(!arcs_[bf_parent_arc_of_[node]].is_marked);
      continue;
    }

    const IntegerValue tail_lb =
        integer_trail_->LowerBound(IntegerVariable(node));
    for (const ArcIndex arc_index : impacted_arcs_[IntegerVariable(node)]) {
      const ArcInfo& arc = arcs_[arc_index];
      DCHECK_EQ(arc.tail_var, node);
      const IntegerValue candidate = tail_lb + ArcOffset(arc);
      if (candidate > integer_trail_->LowerBound(arc.head_var)) {
        if (integer_trail_->IsCurrentlyIgnored(arc.head_var)) continue;
        if (!EnqueueAndCheck(arc, candidate, trail)) return false;

        // This is the Tarjan contribution to Bellman-Ford. This code detect
        // positive cycle, and because it disassemble the subtree while doing
        // so, the cost is amortized during the algorithm execution. Another
        // advantages is that it will mark the node explored here as skippable
        // which will avoid to propagate them too early (knowing that they will
        // need to be propagated again later).
        if (DisassembleSubtree(arc.head_var.value(), arc.tail_var.value(),
                               &bf_can_be_skipped_)) {
          std::vector<Literal> must_be_all_true;
          AnalyzePositiveCycle(arc_index, trail, &must_be_all_true,
                               &literal_reason_, &integer_reason_);
          if (must_be_all_true.empty()) {
            ++num_cycles_;
            return integer_trail_->ReportConflict(literal_reason_,
                                                  integer_reason_);
          } else {
            gtl::STLSortAndRemoveDuplicates(&must_be_all_true);
            for (const Literal l : must_be_all_true) {
              if (trail_->Assignment().LiteralIsFalse(l)) {
                literal_reason_.push_back(l);
                return integer_trail_->ReportConflict(literal_reason_,
                                                      integer_reason_);
              }
            }
            for (const Literal l : must_be_all_true) {
              if (trail_->Assignment().LiteralIsTrue(l)) continue;
              integer_trail_->EnqueueLiteral(l, literal_reason_,
                                             integer_reason_);
            }

            // We just marked some optional variable as ignored, no need
            // to update bf_parent_arc_of_[].
            continue;
          }
        }

        // We need to enforce the invariant that only the arc_index in
        // bf_parent_arc_of_[] are marked (but not necessarily all of them
        // since we unmark some in DisassembleSubtree()).
        if (bf_parent_arc_of_[arc.head_var.value()] != -1) {
          arcs_[bf_parent_arc_of_[arc.head_var.value()]].is_marked = false;
        }

        // Tricky: We just enqueued the fact that the lower-bound of head is
        // candidate. However, because the domain of head may be discrete, it is
        // possible that the lower-bound of head is now higher than candidate!
        // If this is the case, we don't update bf_parent_arc_of_[] so that we
        // don't wrongly detect a positive weight cycle because of this "extra
        // push".
        const IntegerValue new_bound = integer_trail_->LowerBound(arc.head_var);
        if (new_bound == candidate) {
          bf_parent_arc_of_[arc.head_var.value()] = arc_index;
          arcs_[arc_index].is_marked = true;
        } else {
          // We still unmark any previous dependency, since we have pushed the
          // value of arc.head_var further.
          bf_parent_arc_of_[arc.head_var.value()] = -1;
        }

        // We do not re-enqueue if we are in a propagation loop and new_bound
        // was not pushed to candidate or higher.
        bf_can_be_skipped_[arc.head_var.value()] = false;
        if (!bf_in_queue_[arc.head_var.value()] && new_bound >= candidate) {
          bf_queue_.push_back(arc.head_var.value());
          bf_in_queue_[arc.head_var.value()] = true;
        }
      }
    }
  }
  return true;
}

int PrecedencesPropagator::AddGreaterThanAtLeastOneOfConstraintsFromClause(
    const absl::Span<const Literal> clause, Model* model) {
  CHECK_EQ(model->GetOrCreate<Trail>()->CurrentDecisionLevel(), 0);
  if (clause.size() < 2) return 0;

  // Collect all arcs impacted by this clause.
  std::vector<ArcInfo> infos;
  for (const Literal l : clause) {
    if (l.Index() >= literal_to_new_impacted_arcs_.size()) continue;
    for (const ArcIndex arc_index : literal_to_new_impacted_arcs_[l.Index()]) {
      const ArcInfo& arc = arcs_[arc_index];
      if (arc.presence_literals.size() != 1) continue;

      // TODO(user): Support variable offset.
      if (arc.offset_var != kNoIntegerVariable) continue;
      infos.push_back(arc);
    }
  }
  if (infos.size() <= 1) return 0;

  // Stable sort by head_var so that for a same head_var, the entry are sorted
  // by Literal as they appear in clause.
  std::stable_sort(infos.begin(), infos.end(),
                   [](const ArcInfo& a, const ArcInfo& b) {
                     return a.head_var < b.head_var;
                   });

  // We process ArcInfo with the same head_var toghether.
  int num_added_constraints = 0;
  auto* solver = model->GetOrCreate<SatSolver>();
  for (int i = 0; i < infos.size();) {
    const int start = i;
    const IntegerVariable head_var = infos[start].head_var;
    for (i++; i < infos.size() && infos[i].head_var == head_var; ++i) {
    }
    const absl::Span<ArcInfo> arcs(&infos[start], i - start);

    // Skip single arcs since it will already be fully propagated.
    if (arcs.size() < 2) continue;

    // Heuristic. Look for full or almost full clauses. We could add
    // GreaterThanAtLeastOneOf() with more enforcement literals. TODO(user):
    // experiments.
    if (arcs.size() + 1 < clause.size()) continue;

    std::vector<IntegerVariable> vars;
    std::vector<IntegerValue> offsets;
    std::vector<Literal> selectors;
    std::vector<Literal> enforcements;

    int j = 0;
    for (const Literal l : clause) {
      bool added = false;
      for (; j < arcs.size() && l == arcs[j].presence_literals.front(); ++j) {
        added = true;
        vars.push_back(arcs[j].tail_var);
        offsets.push_back(arcs[j].offset);

        // Note that duplicate selector are supported.
        //
        // TODO(user): If we support variable offset, we should regroup the arcs
        // into one (tail + offset <= head) though, instead of having too
        // identical entries.
        selectors.push_back(l);
      }
      if (!added) {
        enforcements.push_back(l.Negated());
      }
    }

    // No point adding a constraint if there is not at least two different
    // literals in selectors.
    if (enforcements.size() + 1 == clause.size()) continue;

    ++num_added_constraints;
    model->Add(GreaterThanAtLeastOneOf(head_var, vars, offsets, selectors,
                                       enforcements));
    if (!solver->FinishPropagation()) return num_added_constraints;
  }
  return num_added_constraints;
}

int PrecedencesPropagator::
    AddGreaterThanAtLeastOneOfConstraintsWithClauseAutoDetection(Model* model) {
  auto* time_limit = model->GetOrCreate<TimeLimit>();
  auto* solver = model->GetOrCreate<SatSolver>();

  // Fill the set of incoming conditional arcs for each variables.
  absl::StrongVector<IntegerVariable, std::vector<ArcIndex>> incoming_arcs_;
  for (ArcIndex arc_index(0); arc_index < arcs_.size(); ++arc_index) {
    const ArcInfo& arc = arcs_[arc_index];

    // Only keep arc that have a fixed offset and a single presence_literals.
    if (arc.offset_var != kNoIntegerVariable) continue;
    if (arc.tail_var == arc.head_var) continue;
    if (arc.presence_literals.size() != 1) continue;

    if (arc.head_var >= incoming_arcs_.size()) {
      incoming_arcs_.resize(arc.head_var.value() + 1);
    }
    incoming_arcs_[arc.head_var].push_back(arc_index);
  }

  int num_added_constraints = 0;
  for (IntegerVariable target(0); target < incoming_arcs_.size(); ++target) {
    if (incoming_arcs_[target].size() <= 1) continue;
    if (time_limit->LimitReached()) return num_added_constraints;

    // Detect set of incoming arcs for which at least one must be present.
    // TODO(user): Find more than one disjoint set of incoming arcs.
    // TODO(user): call MinimizeCoreWithPropagation() on the clause.
    solver->Backtrack(0);
    if (solver->ModelIsUnsat()) return num_added_constraints;
    std::vector<Literal> clause;
    for (const ArcIndex arc_index : incoming_arcs_[target]) {
      const Literal literal = arcs_[arc_index].presence_literals.front();
      if (solver->Assignment().LiteralIsFalse(literal)) continue;
      const SatSolver::Status status =
          solver->EnqueueDecisionAndBacktrackOnConflict(literal.Negated());
      if (status == SatSolver::INFEASIBLE) return num_added_constraints;
      if (status == SatSolver::ASSUMPTIONS_UNSAT) {
        clause = solver->GetLastIncompatibleDecisions();
        break;
      }
    }
    solver->Backtrack(0);

    if (clause.size() > 1) {
      // Extract the set of arc for which at least one must be present.
      const absl::btree_set<Literal> clause_set(clause.begin(), clause.end());
      std::vector<ArcIndex> arcs_in_clause;
      for (const ArcIndex arc_index : incoming_arcs_[target]) {
        const Literal literal(arcs_[arc_index].presence_literals.front());
        if (clause_set.contains(literal.Negated())) {
          arcs_in_clause.push_back(arc_index);
        }
      }

      VLOG(2) << arcs_in_clause.size() << "/" << incoming_arcs_[target].size();

      ++num_added_constraints;
      std::vector<IntegerVariable> vars;
      std::vector<IntegerValue> offsets;
      std::vector<Literal> selectors;
      for (const ArcIndex a : arcs_in_clause) {
        vars.push_back(arcs_[a].tail_var);
        offsets.push_back(arcs_[a].offset);
        selectors.push_back(Literal(arcs_[a].presence_literals.front()));
      }
      model->Add(GreaterThanAtLeastOneOf(target, vars, offsets, selectors, {}));
      if (!solver->FinishPropagation()) return num_added_constraints;
    }
  }

  return num_added_constraints;
}

int PrecedencesPropagator::AddGreaterThanAtLeastOneOfConstraints(Model* model) {
  VLOG(1) << "Detecting GreaterThanAtLeastOneOf() constraints...";
  auto* time_limit = model->GetOrCreate<TimeLimit>();
  auto* solver = model->GetOrCreate<SatSolver>();
  auto* clauses = model->GetOrCreate<LiteralWatchers>();
  int num_added_constraints = 0;

  // We have two possible approaches. For now, we prefer the first one except if
  // there is too many clauses in the problem.
  //
  // TODO(user): Do more extensive experiment. Remove the second approach as
  // it is more time consuming? or identify when it make sense. Note that the
  // first approach also allows to use "incomplete" at least one between arcs.
  if (clauses->AllClausesInCreationOrder().size() < 1e6) {
    // TODO(user): This does not take into account clause of size 2 since they
    // are stored in the BinaryImplicationGraph instead. Some ideas specific
    // to size 2:
    // - There can be a lot of such clauses, but it might be nice to consider
    //   them. we need to experiments.
    // - The automatic clause detection might be a better approach and it
    //   could be combined with probing.
    for (const SatClause* clause : clauses->AllClausesInCreationOrder()) {
      if (time_limit->LimitReached()) return num_added_constraints;
      if (solver->ModelIsUnsat()) return num_added_constraints;
      num_added_constraints += AddGreaterThanAtLeastOneOfConstraintsFromClause(
          clause->AsSpan(), model);
    }

    // It is common that there is only two alternatives to push a variable.
    // In this case, our presolve most likely made sure that the two are
    // controlled by a single Boolean. This allows to detect this and add the
    // appropriate greater than at least one of.
    const int num_booleans = solver->NumVariables();
    if (num_booleans < 1e6) {
      for (int i = 0; i < num_booleans; ++i) {
        if (time_limit->LimitReached()) return num_added_constraints;
        if (solver->ModelIsUnsat()) return num_added_constraints;
        num_added_constraints +=
            AddGreaterThanAtLeastOneOfConstraintsFromClause(
                {Literal(BooleanVariable(i), true),
                 Literal(BooleanVariable(i), false)},
                model);
      }
    }

  } else {
    num_added_constraints +=
        AddGreaterThanAtLeastOneOfConstraintsWithClauseAutoDetection(model);
  }

  if (num_added_constraints > 0) {
    SOLVER_LOG(model->GetOrCreate<SolverLogger>(), "[Precedences] Added ",
               num_added_constraints,
               " GreaterThanAtLeastOneOf() constraints.");
  }
  return num_added_constraints;
}

}  // namespace sat
}  // namespace operations_research
