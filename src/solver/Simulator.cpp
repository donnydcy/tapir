#include "Simulator.hpp"

#include <fstream>                      // for operator<<, basic_ostream, basic_ostream<>::__ostream_type, ofstream, endl, ostream, ifstream
#include <iomanip>
#include <iostream>                     // for operator<<, ostream, basic_ostream, endl, basic_ostream<>::__ostream_type, cout

#include "abstract-problem/Observation.hpp"
#include "abstract-problem/ModelChange.hpp"

#include "serialization/Serializer.hpp"

#include "Agent.hpp"
#include "BeliefNode.hpp"
#include "HistoryEntry.hpp"
#include "HistorySequence.hpp"
#include "Solver.hpp"
#include "StatePool.hpp"


using std::cout;
using std::endl;

namespace solver {
Simulator::Simulator(std::unique_ptr<Model> model, Solver *solver) :
        model_(std::move(model)),
        solver_(solver),
        solverModel_(solver_->getModel()),
        agent_(std::make_unique<Agent>(solver_)),
        changeSequence_(),
        stepCount_(0),
        maxStepCount_(100),
        currentDiscount_(1.0),
        totalDiscountedReward_(0.0),
        actualHistory_(std::make_unique<HistorySequence>()),
        totalChangingTime_(0.0),
        totalImprovementTime_(0.0) {
    std::unique_ptr<State> initialState = model_->sampleAnInitState();
    StateInfo *initInfo = solver_->getStatePool()->createOrGetInfo(*initialState);
    actualHistory_->addEntry(initInfo);
}
Model *Simulator::getModel() const {
    return model_.get();
}
Agent *Simulator::getAgent() const {
    return agent_.get();
}
Solver *Simulator::getSolver() const {
    return solver_;
}
Model *Simulator::getSolverModel() const {
    return solverModel_;
}


State const *Simulator::getCurrentState() const {
    return actualHistory_->getLastEntry()->getState();
}
HistorySequence *Simulator::getHistory() const {
    return actualHistory_.get();
}
long Simulator::getStepCount() const {
    return stepCount_;
}
double Simulator::getTotalChangingTime() const {
    return totalChangingTime_;
}
double Simulator::getTotalImprovementTime() const {
    return totalImprovementTime_;
}


void Simulator::setChangeSequence(ChangeSequence sequence) {
    changeSequence_ = std::move(sequence);
}
void Simulator::loadChangeSequence(std::string path) {
    std::ifstream ifs(path);
    setChangeSequence(solver_->getSerializer()->loadChangeSequence(ifs));
    ifs.close();
}
void Simulator::setMaxStepCount(long maxStepCount) {
    maxStepCount_ = maxStepCount;
}
double Simulator::runSimulation() {
    while (stepSimulation()) {
    }
    if (model_->hasVerboseOutput()) {
        cout << endl << endl << "Final State:" << endl;
        State const &currentState = *getCurrentState();
        cout << currentState << endl;
        BeliefNode *currentBelief = agent_->getCurrentBelief();
        cout << "Belief #" << currentBelief->getId() << endl;
        model_->drawSimulationState(currentBelief, currentState, cout);
    }
    return totalDiscountedReward_;
}
bool Simulator::stepSimulation() {
    if (stepCount_ >= maxStepCount_) {
        return false;
    } else if (model_->isTerminal(*getCurrentState())) {
        return false;
    }

    std::stringstream prevStream;
    State const *currentState = getCurrentState();
    BeliefNode *currentBelief = agent_->getCurrentBelief();
    if (model_->hasVerboseOutput()) {
        cout << endl << endl << "t-" << stepCount_ << endl;
        cout << "State: " << *currentState << endl;
        cout << "Heuristic Value: " << model_->getHeuristicValue(*currentState) << endl;
        cout << "Belief #" << currentBelief->getId() << endl;

        solver::HistoricalData *data = currentBelief->getHistoricalData();
        if (data != nullptr) {
            cout << endl;
            cout << *data;
            cout << endl;
        }
        model_->drawSimulationState(currentBelief, *currentState, cout);
        prevStream << "Before:" << endl;
        solver_->printBelief(currentBelief, prevStream);
    }

    ChangeSequence::iterator iter = changeSequence_.find(stepCount_);
    if (iter != changeSequence_.end()) {
        if (model_->hasVerboseOutput()) {
            cout << "Model changing." << endl;
        }
        double changingTimeStart = abt::clock_ms();
        // Apply all the changes!
        bool noError = handleChanges(iter->second);
        if (!noError) {
            return false;
        }
        double changingTime = abt::clock_ms() - changingTimeStart;
        totalChangingTime_ += changingTime;
        if (model_->hasVerboseOutput()) {
            cout << "Changes complete" << endl;
            cout << "Total of " << changingTime << " ms used for changes." << endl;
        }
    }

    double impSolTimeStart = abt::clock_ms();
    solver_->improvePolicy(currentBelief);
    totalImprovementTime_ += (impSolTimeStart - abt::clock_ms());

    if (model_->hasVerboseOutput()) {
        std::stringstream newStream;
        newStream << "After:" << endl;
        solver_->printBelief(currentBelief, newStream);
        while (prevStream.good() || newStream.good()) {
            std::string s1, s2;
            std::getline(prevStream, s1);
            std::getline(newStream, s2);
            cout << s1 << std::setw(40 - s1.size()) << "";
            cout << s2 << std::setw(40 - s2.size()) << "";
            cout << endl;
        }
    }

    std::unique_ptr<Action> action = agent_->getPreferredAction();
    if (action == nullptr) {
        debug::show_message("ERROR: Could not choose an action!");
        return false;
    }
    Model::StepResult result = model_->generateStep(*currentState, *action);

    if (model_->hasVerboseOutput()) {
        if (result.isTerminal) {
            cout << "Reached a terminal state!" << endl;
        }
        cout << "Action: " << *result.action << endl;
        cout << "Transition: ";
        if (result.transitionParameters == nullptr) {
            cout << "NULL" << endl;
        } else {
            cout << *result.transitionParameters << endl;
        }
        cout << "Reward: " << result.reward << endl;
        cout << "Observation: " << *result.observation << endl;
        cout << "Discount: " << currentDiscount_ << "; Total Reward: ";
        cout << totalDiscountedReward_ << endl;
    }

    agent_->updateBelief(*result.action, *result.observation);
    currentBelief = agent_->getCurrentBelief();
    if (currentBelief->getNumberOfParticles() == 0) {
        debug::show_message("ERROR: Resulting belief has zero particles!!");
    }

    HistoryEntry *currentEntry = actualHistory_->getLastEntry();
    currentEntry->action_ = std::move(result.action);
    currentEntry->observation_ = std::move(result.observation);
    currentEntry->reward_ = result.reward;
    currentEntry->transitionParameters_ = std::move(result.transitionParameters);
    StateInfo *nextInfo = solver_->getStatePool()->createOrGetInfo(*result.nextState);
    currentEntry = actualHistory_->addEntry(nextInfo);

    totalDiscountedReward_ += currentDiscount_ * result.reward;
    currentDiscount_ *= model_->getDiscountFactor();
    stepCount_++;

    return !result.isTerminal;
}

bool Simulator::handleChanges(std::vector<std::unique_ptr<ModelChange>> const &changes) {
    StatePool *statePool = solver_->getStatePool();
    for (std::unique_ptr<ModelChange> const &change : changes) {
        model_->applyChange(*change, nullptr);
        solverModel_->applyChange(*change, statePool);
    }

    // Check if the model changes have invalidated our history...
    StateInfo const *lastInfo = actualHistory_->getLastEntry()->getStateInfo();
    if (changes::has_flag(lastInfo->changeFlags_, ChangeFlags::DELETED)) {
        debug::show_message("ERROR: Current simulation state deleted!");
        return false;
    }
    for (long i = 0; i < actualHistory_->getLength() - 1; i++) {
        StateInfo const *info = actualHistory_->getEntry(i)->getStateInfo();
        if (changes::has_flag(info->changeFlags_, ChangeFlags::DELETED)) {
            std::ostringstream message;
            message << "ERROR: Impossible simulation history! Includes ";
            message << *info->getState();
            debug::show_message(message.str());
        }
    }

    solver_->applyChanges();
    return true;
}


} /* namespace solver */
