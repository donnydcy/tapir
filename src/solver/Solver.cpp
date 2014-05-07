#include "Solver.hpp"

#include <cmath>                        // for pow, exp
#include <cstdio>

#include <algorithm>                    // for max
#include <iostream>                     // for operator<<, ostream, basic_ostream, endl, basic_ostream<>::__ostream_type, cout
#include <limits>
#include <memory>                       // for unique_ptr
#include <random>                       // for uniform_int_distribution, bernoulli_distribution
#include <set>                          // for set, _Rb_tree_const_iterator, set<>::iterator
#include <tuple>                        // for tie, tuple
#include <type_traits>                  // for remove_reference<>::type
#include <utility>                      // for move, make_pair, pair
#include <vector>                       // for vector, vector<>::iterator, vector<>::reverse_iterator

#include "global.hpp"                     // for RandomGenerator

#include "abstract-problem/Action.hpp"                   // for Action
#include "abstract-problem/Model.hpp"                    // for Model::StepResult, Model
#include "abstract-problem/ModelChange.hpp"                    // for Model::StepResult, Model
#include "abstract-problem/Observation.hpp"              // for Observation
#include "abstract-problem/State.hpp"                    // for State, operator<<

#include "backpropagation/BackpropagationStrategy.hpp"

#include "changes/ChangeFlags.hpp"               // for ChangeFlags, ChangeFlags::UNCHANGED, ChangeFlags::ADDOBSERVATION, ChangeFlags::ADDOBSTACLE, ChangeFlags::ADDSTATE, ChangeFlags::DELSTATE, ChangeFlags::REWARD, ChangeFlags::TRANSITION
#include "changes/HistoryCorrector.hpp"

#include "mappings/ActionMapping.hpp"
#include "mappings/ActionPool.hpp"
#include "mappings/ObservationMapping.hpp"
#include "mappings/ObservationPool.hpp"

#include "search/SearchStatus.hpp"
#include "search/SearchStrategy.hpp"

#include "serialization/Serializer.hpp"               // for Serializer

#include "indexing/RTree.hpp"
#include "indexing/SpatialIndexVisitor.hpp"

#include "ActionNode.hpp"               // for BeliefNode, BeliefNode::startTime
#include "BeliefNode.hpp"               // for BeliefNode, BeliefNode::startTime
#include "BeliefTree.hpp"               // for BeliefTree
#include "Histories.hpp"                // for Histories
#include "HistoryEntry.hpp"             // for HistoryEntry
#include "HistorySequence.hpp"          // for HistorySequence
#include "StateInfo.hpp"                // for StateInfo
#include "StatePool.hpp"                // for StatePool

using std::cout;
using std::endl;

namespace solver {
Solver::Solver(RandomGenerator *randGen, std::unique_ptr<Model> model) :
    randGen_(randGen),
    serializer_(nullptr),
    model_(std::move(model)),
    statePool_(nullptr),
    histories_(nullptr),
    policy_(nullptr),
    actionPool_(nullptr),
    observationPool_(nullptr),
    historyCorrector_(nullptr),
    selectionStrategy_(nullptr),
    rolloutStrategy_(nullptr),
    backpropagationStrategy_(nullptr) {
}

// Default destructor
Solver::~Solver() {
}

/* ------------------ Simple getters. ------------------- */
BeliefTree *Solver::getPolicy() const {
    return policy_.get();
}
StatePool *Solver::getStatePool() const {
    return statePool_.get();
}
Model *Solver::getModel() const {
    return model_.get();
}
ActionPool *Solver::getActionPool() const {
    return actionPool_.get();
}
ObservationPool *Solver::getObservationPool() const {
    return observationPool_.get();
}

/* ------------------ Initialization methods ------------------- */
void Solver::initializeEmpty() {
    // Basic initialization.
    initialize();

    // Create new instances of these.
    actionPool_ = model_->createActionPool(this);
    observationPool_ = model_->createObservationPool(this);

    // Initialize the root node properly.
    BeliefNode *rootPtr = policy_->getRoot();
    rootPtr->setHistoricalData(model_->createRootHistoricalData());
    rootPtr->setMapping(actionPool_->createActionMapping());
    rootPtr->getMapping()->initialize();
}

Serializer *Solver::getSerializer() {
    return serializer_.get();
}
void Solver::setSerializer(std::unique_ptr<Serializer> serializer) {
    serializer_ = std::move(serializer);
}

/* ------------------- Policy mutators ------------------- */
void Solver::improvePolicy(long numberOfHistories, long maximumDepth) {
    if (numberOfHistories < 0) {
        numberOfHistories = model_->getNumberOfHistoriesPerStep();
    }
    if (maximumDepth < 0) {
        maximumDepth = model_->getMaximumDepth();
    }

    // Generate the initial states.
    std::vector<StateInfo *> states;
    for (long i = 0; i < numberOfHistories; i++) {
    	states.push_back(statePool_->createOrGetInfo(
    			*model_->sampleAnInitState()));
    }
    multipleSearches(policy_->getRoot(), states, maximumDepth);
}

void Solver::improvePolicy(BeliefNode *startNode, long numberOfHistories,
        long maximumDepth) {
    if (numberOfHistories < 0) {
        numberOfHistories = model_->getNumberOfHistoriesPerStep();
    }
    if (maximumDepth < 0) {
        maximumDepth = model_->getMaximumDepth();
    }
    if (startNode->getNumberOfParticles() == 0) {
        debug::show_message("ERROR: No particles in the BeliefNode!");
        std::exit(10);
    }

    std::vector<StateInfo *> nonTerminalStates;
    for (long index = 0; index < startNode->getNumberOfParticles(); index++) {
        HistoryEntry *entry = startNode->particles_.get(index);
        if (!model_->isTerminal(*entry->getState())) {
            nonTerminalStates.push_back(entry->stateInfo_);
        }
    }
    if (nonTerminalStates.empty()) {
        debug::show_message("ERROR: No non-terminal particles!");
        std::exit(11);
    }

    std::vector<StateInfo *> samples;
    for (long i = 0; i < numberOfHistories; i++) {
        long index = std::uniform_int_distribution<long>(
                0, nonTerminalStates.size() - 1)(*randGen_);
        samples.push_back(nonTerminalStates[index]);
    }

    multipleSearches(startNode, samples, maximumDepth);
}

void Solver::applyChanges() {
    std::unordered_set<HistorySequence *> affectedSequences;
    for (StateInfo *stateInfo : statePool_->getAffectedStates()) {
        for (HistoryEntry *entry : stateInfo->usedInHistoryEntries_) {
            HistorySequence *sequence = entry->owningSequence_;
            long entryId = entry->entryId_;
            sequence->setChangeFlags(entryId, stateInfo->changeFlags_);
            if (changes::has_flag(entry->changeFlags_, ChangeFlags::DELETED)) {
                if (entryId > 0) {
                    sequence->setChangeFlags(entryId - 1,
                            ChangeFlags::TRANSITION);
                }
            }
            if (changes::has_flag(entry->changeFlags_,
                    ChangeFlags::OBSERVATION_BEFORE)) {
                if (entryId > 0) {
                    sequence->setChangeFlags(entryId - 1,
                            ChangeFlags::OBSERVATION);
                }
            }
            affectedSequences.insert(sequence);
        }
    }
    if (model_->hasVerboseOutput()) {
        cout << "Updating " << affectedSequences.size() << " histories!";
        cout << endl;
    }

    // Delete and remove any sequences where the first entry is now invalid.
    std::unordered_set<HistorySequence *>::iterator it = affectedSequences.begin();
    while (it != affectedSequences.end()) {
        HistorySequence *sequence = *it;

        // Undo backup and deregister.
        backup(sequence, false);
        sequence->registerWith(nullptr, nullptr);

        if (changes::has_flag(sequence->getFirstEntry()->changeFlags_,
                ChangeFlags::DELETED)) {
            it = affectedSequences.erase(it);
            // Now remove the sequence entirely.
            histories_->deleteSequence(sequence->id_);
        } else {
            it++;
        }
    }

    // Revise all of the histories.
    historyCorrector_->reviseHistories(affectedSequences);

    // Clear flags and fix up all the sequences.
    for (HistorySequence *sequence : affectedSequences) {
        sequence->resetChangeFlags();
        HistoryEntry *lastEntry = sequence->getLastEntry();
        State const *lastState = lastEntry->getState();
        // If it didn't end in a terminal state, we apply the heuristic.
        if (!model_->isTerminal(*lastState)) {
            lastEntry->rewardFromHere_ = model_->getHeuristicValue(*lastState);
        }

        // Now we register and then backup.
        sequence->registerWith(sequence->getFirstEntry()->associatedBeliefNode_,
                    policy_.get());
        backup(sequence, true);
    }

    // Reset the set of affected states in the pool.
    statePool_->resetAffectedStates();
}

BeliefNode *Solver::addChild(BeliefNode *currNode, Action const &action,
        Observation const &obs) {
    BeliefNode *nextNode = policy_->createOrGetChild(currNode, action, obs);

    std::vector<State const *> particles;
    std::vector<HistoryEntry *>::iterator it;
    for (HistoryEntry *entry : currNode->particles_) {
        particles.push_back(entry->getState());
    }
    // Attempt to generate particles for next state based on the current belief,
    // the observation, and the action.
    std::vector<std::unique_ptr<State>> nextParticles(
            model_->generateParticles(currNode, action, obs, particles));
    if (nextParticles.empty()) {
        debug::show_message("WARNING: Could not generate based on belief!");
        // If that fails, ignore the current belief.
        nextParticles = model_->generateParticles(currNode, action, obs);
    }
    if (nextParticles.empty()) {
        debug::show_message("ERROR: Failed to generate new particles!");
    }
    for (std::unique_ptr<State> &uniqueStatePtr : nextParticles) {
        StateInfo *stateInfo = statePool_->createOrGetInfo(*uniqueStatePtr);

        // Create a new history sequence and entry for the new particle.
        HistorySequence *histSeq = histories_->createSequence();
        HistoryEntry *histEntry = histSeq->addEntry(stateInfo);
        State const *state = stateInfo->getState();
        if (!model_->isTerminal(*state)) {
            // Use the heuristic value for non-terminal particles.
            histEntry->rewardFromHere_ = model_->getHeuristicValue(*state);
        }
        // Register and backup
        histSeq->registerWith(nextNode, policy_.get());
        backup(histSeq, true);
    }
    return nextNode;
}

/* ------------------ Display methods  ------------------- */
void Solver::printBelief(BeliefNode *belief, std::ostream &os) {
    os << belief->getQValue();
    os << " from " << belief->getNumberOfParticles() << " p.";
    os << " with " << belief->getNumberOfStartingSequences() << " starts.";
    os << endl;
    os << "Action children: " << endl;
    std::multimap<double, solver::ActionMappingEntry const *> actionValues;
    for (solver::ActionMappingEntry const *entry : belief->getMapping()->getVisitedEntries()) {
        actionValues.emplace(entry->getMeanQValue(), entry);
    }
    for (auto it = actionValues.rbegin(); it != actionValues.rend(); it++) {
        abt::print_double(it->first, os, 8, 2,
                std::ios_base::fixed | std::ios_base::showpos);
        os << ": ";
        std::ostringstream sstr;
        sstr << *it->second->getAction();
        abt::print_with_width(sstr.str(), os, 17);
        abt::print_with_width(it->second->getVisitCount(), os, 8);
        os << endl;
    }
}

/* ============================ PRIVATE ============================ */


/* ------------------ Initialization methods ------------------- */
void Solver::initialize() {
    // Core data structures
    statePool_ = std::make_unique<StatePool>(model_->createStateIndex());
    histories_ = std::make_unique<Histories>();
    policy_ = std::make_unique<BeliefTree>(this);

    // Serializable model-specific customizations
    actionPool_ = nullptr;
    observationPool_ = nullptr;

    // Possible model-specific customizations
    historyCorrector_ = model_->createHistoryCorrector(this);
    selectionStrategy_ = model_->createSelectionStrategy(this);
    rolloutStrategy_ = model_->createRolloutStrategy(this);
    backpropagationStrategy_ = model_->createBackpropagationStrategy(this);
}

/* ------------------ Episode sampling methods ------------------- */
void Solver::multipleSearches(BeliefNode *startNode,
        std::vector<StateInfo *> states, long maximumDepth) {
    if (maximumDepth < 0) {
        maximumDepth = model_->getMaximumDepth();
    }
	for (StateInfo *stateInfo : states) {
		singleSearch(startNode, stateInfo, maximumDepth);
	}
}

void Solver::singleSearch(BeliefNode *startNode, StateInfo *startStateInfo,
		long maximumDepth) {
    if (maximumDepth < 0) {
        maximumDepth = model_->getMaximumDepth();
    }
    HistorySequence *sequence = histories_->createSequence();
    sequence->addEntry(startStateInfo);
    sequence->getFirstEntry()->associatedBeliefNode_ = startNode;
    continueSearch(sequence, maximumDepth);
}

void Solver::continueSearch(HistorySequence *sequence, long maximumDepth) {
    if (maximumDepth < 0) {
        maximumDepth = model_->getMaximumDepth();
    }
    if (model_->isTerminal(*sequence->getLastEntry()->getState())) {
        debug::show_message("WARNING: Attempted to continue sequence"
                " from a terminal state.");
        return;
    }
    SearchStatus status = SearchStatus::UNINITIALIZED;

    std::unique_ptr<SearchInstance> searchInstance = nullptr;
    searchInstance = selectionStrategy_->createSearchInstance(sequence,
            maximumDepth);
    status = searchInstance->initialize();
    if (status != SearchStatus::INITIAL) {
        debug::show_message("WARNING: Search algorithm could not initialize!?");
    }
    status = searchInstance->extendSequence();
    if (status == SearchStatus::REACHED_ROLLOUT_NODE) {
        searchInstance = rolloutStrategy_->createSearchInstance(sequence,
                maximumDepth);
        status = searchInstance->initialize();
        if (status != SearchStatus::INITIAL) {
            debug::show_message("WARNING: Rollout algorithm could not initialize!?");
        }
        status = searchInstance->extendSequence();
    }
    if (status == SearchStatus::ROLLOUT_COMPLETE || status == SearchStatus::HIT_DEPTH_LIMIT) {
        HistoryEntry *lastEntry = sequence->getLastEntry();
        State const *lastState = lastEntry->getState();
        if (model_->isTerminal(*lastState)) {
            debug::show_message("ERROR: Terminal state, but the search status"
                    " didn't reflect this!");
        }
        // Use the heuristic estimate.
        lastEntry->rewardFromHere_ = model_->getHeuristicValue(*lastState);
    } else if (status == SearchStatus::HIT_TERMINAL_STATE) {
        // Don't do anything for a terminal state.
    } else {
        debug::show_message("ERROR: Search failed!?");
        return;
    }
    // Register and backup.
    sequence->registerWith(sequence->getFirstEntry()->associatedBeliefNode_,
            policy_.get());
    backup(sequence, true);
}

/* ------------------ Tree backup methods ------------------- */
void Solver::calculateRewards(HistorySequence *sequence) {
    double discountFactor = model_->getDiscountFactor();
    std::vector<std::unique_ptr<HistoryEntry>>::reverse_iterator itHist = (
                sequence->entrySequence_.rbegin());
    // Retrieve the value of the last entry.
    double totalReward = (*itHist)->rewardFromHere_;
    itHist++;
    for (; itHist != sequence->entrySequence_.rend(); itHist++) {
           HistoryEntry *entry = itHist->get();
           // Apply the discount
           totalReward *= discountFactor;
           // Include the reward from this entry.
           entry->rewardFromHere_ = totalReward = entry->reward_ + totalReward;
    }
}

void Solver::backup(HistorySequence *sequence, bool backingUp) {
    sequence->testBackup(backingUp);
    calculateRewards(sequence);
    backpropagationStrategy_->propagate(sequence, !backingUp);
}
} /* namespace solver */
