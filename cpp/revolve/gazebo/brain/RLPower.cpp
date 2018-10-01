/*
* Copyright (C) 2017 Vrije Universiteit Amsterdam
*
* Licensed under the Apache License, Version 2.0 (the "License");
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* Description: TODO: <Add brief description about file purpose>
* Author: Milan Jelisavcic
* Date: March 28, 2016
*
*/

#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <gsl/gsl_spline.h>

#include "RLPower.h"
#include "../motors/Motor.h"
#include "../sensors/Sensor.h"

namespace gz = gazebo;

using namespace revolve::gazebo;

/////////////////////////////////////////////////
RLPower::RLPower(
    std::string _modelName,
    sdf::ElementPtr _node,
    std::vector< MotorPtr > &_motors,
    std::vector< SensorPtr > &_sensors)
//    : evaluator_(evaluator)
    : generationCounter_(0)
    , cycleStartTime_(-1)
    , startTime_(-1)
    , robotName_(_modelName)
{
  // Create transport node
  this->node_.reset(new gz::transport::Node());
  this->node_->Init();

//  // Listen to network modification requests
//  this->alterSub_ = this->node_->Subscribe(
//      "~/" + _modelName + "/modify_spline_policy", &RLPower::Modify,
//      this);

  this->algorithmType_ = "D";
  this->evaluationRate_ = 30.0;
  this->numInterpolationPoints_ = 100;
  this->maxEvaluations_ = 1000;
  this->maxRankedPolicies_ = 10;
  this->sigma_ = 0.8;
  this->tau_ = 0.2;
  this->sourceYSize_ = 3;

  this->stepRate_ = this->numInterpolationPoints_ / this->sourceYSize_;

  // Generate first random policy
  auto numMotors = _motors.size();
  this->InitialisePolicy(numMotors);

  // Start the evaluator
//  this->evaluator_->Start();
}

/////////////////////////////////////////////////
RLPower::~RLPower() = default;

/////////////////////////////////////////////////
void RLPower::Update(
    const std::vector< MotorPtr > &_motors,
    const std::vector< SensorPtr > &_sensors,
    double _time,
    double _step)
{
//  boost::mutex::scoped_lock lock(this->rlpowerMutex_);

  if (this->startTime_ < 0)
  {
    this->startTime_ = _time;
  }

  auto numMotors = _motors.size();

  // Evaluate policy on certain time limit
  if ((_time - this->startTime_) > this->evaluationRate_ and
      this->generationCounter_ < this->maxEvaluations_)
  {
    this->UpdatePolicy(numMotors);
    this->startTime_ = _time;
//    evaluator_->Start();
  }

  // generate outputs
  auto *output = new double[numMotors];
  this->Output(numMotors, _time, output);

  // Send new signals to the actuators
  size_t p = 0;
  for (const auto &motor: _motors)
  {
    motor->Update(&output[p], _step);
    p += motor->Outputs();
  }

  delete[] output;
}

/////////////////////////////////////////////////
void RLPower::InitialisePolicy(size_t _numSplines)
{
  std::random_device rd;
  std::mt19937 mt(rd());
  std::normal_distribution< double > dist(0, this->sigma_);

  // Init first random controller
  if (not this->currentPolicy_)
  {
    this->currentPolicy_ = std::make_shared< Policy >(_numSplines);
  }

  for (size_t i = 0; i < _numSplines; i++)
  {
    Spline spline(this->sourceYSize_);
    for (size_t j = 0; j < this->sourceYSize_; ++j)
    {
      spline[j] = dist(mt);
    }
    this->currentPolicy_->at(i) = spline;
  }

  // Init of empty cache
  if (not this->interpolationCache_)
  {
    this->interpolationCache_ = std::make_shared< Policy >(_numSplines);
  }

  for (size_t i = 0; i < _numSplines; i++)
  {
    this->interpolationCache_->at(i).resize(this->numInterpolationPoints_, 0);
  }

  this->InterpolateCubic(
      _numSplines,
      this->currentPolicy_.get(),
      this->interpolationCache_.get());
}

/////////////////////////////////////////////////
void RLPower::InterpolateCubic(
    const size_t _numSplines,
    Policy *const _sourceY,
    Policy *_destinationY)
{
  const size_t sourceYSize = (*_sourceY)[0].size();
  const size_t destinatioYSize = (*_destinationY)[0].size();

  const size_t N = sourceYSize + 1;
  auto *x = new double[N];
  auto *y = new double[N];
  auto *xNew = new double[destinatioYSize];

  gsl_interp_accel *acc = gsl_interp_accel_alloc();
  const gsl_interp_type *t = gsl_interp_cspline_periodic;
  gsl_spline *spline = gsl_spline_alloc(t, N);

  // init x
  double step_size = RLPower::CYCLE_LENGTH / sourceYSize;
  for (size_t i = 0; i < N; i++)
  {
    x[i] = step_size * i;
  }

  // init xNew
  step_size = CYCLE_LENGTH / destinatioYSize;
  for (size_t i = 0; i < destinatioYSize; i++)
  {
    xNew[i] = step_size * i;
  }

  for (size_t j = 0; j < _numSplines; j++)
  {
    Spline &sourceYLine = _sourceY->at(j);
    Spline &destinationYLine = _destinationY->at(j);

    // init y
    // TODO use memcpy
    for (size_t i = 0; i < sourceYSize; i++)
    {
      y[i] = sourceYLine[i];
    }

    // make last equal to first
    y[N - 1] = y[0];

    gsl_spline_init(spline, x, y, N);

    for (size_t i = 0; i < destinatioYSize; i++)
    {
      destinationYLine[i] = gsl_spline_eval(spline, xNew[i], acc);
    }
  }

  gsl_spline_free(spline);
  gsl_interp_accel_free(acc);

  delete[] xNew;
  delete[] y;
  delete[] x;
}

/////////////////////////////////////////////////
void RLPower::UpdatePolicy(const size_t _numSplines)
{
  // Calculate fitness for current policy
  double curr_fitness = this->Fitness();

  // Insert ranked policy in list
  PolicyPtr backupPolicy = std::make_shared< Policy >(_numSplines);
  for (size_t i = 0; i < _numSplines; i++)
  {
    Spline &spline = this->currentPolicy_->at(i);
    backupPolicy->at(i) = Spline(spline.begin(), spline.end());

    spline.resize(this->sourceYSize_);
  }
  this->rankedPolicies_.insert({curr_fitness, backupPolicy});

  // Remove worst policies
  while (this->rankedPolicies_.size() > this->maxRankedPolicies_)
  {
    auto last = std::prev(this->rankedPolicies_.end());
    this->rankedPolicies_.erase(last);
  }

  // TODO: Record fitnesses and policies

  // Update generation counter and check is it finished
  this->generationCounter_++;
  if (this->generationCounter_ == this->maxEvaluations_)
  {
    std::exit(0);
  }

  // Increase spline points if it is a time
  if (this->generationCounter_ % this->stepRate_ == 0)
  {
    this->IncreaseSplinePoints(_numSplines);
  }

  /// Actual policy generation

  /// Determine which mutation operator to use
  /// Default, for algorithms A and B, is used standard normal distribution
  /// with decaying sigma. For algorithms C and D, is used normal distribution
  /// with self-adaptive sigma.
  std::random_device rd;
  std::mt19937 mt(rd());

  if (this->algorithmType_ == "C" or this->algorithmType_ == "D")
  {
    // uncorrelated mutation with one step size
    std::mt19937 sigma_mt(rd());
    std::normal_distribution< double > sigma_dist(0, 1);
    this->sigma_ = this->sigma_ * std::exp(this->tau_ * sigma_dist(sigma_mt));
  }
  else
  {
    // Default is decaying sigma
    if (this->rankedPolicies_.size() >= this->maxRankedPolicies_)
    {
      this->sigma_ *= SIGMA;
    }
  }
  std::normal_distribution< double > dist(0, this->sigma_);

  /// Determine which selection operator to use
  /// Default, for algorithms A and C, is used ten parent crossover
  /// For algorithms B and D, is used two parent crossover with binary
  /// tournament selection
  if (this->rankedPolicies_.size() < this->maxRankedPolicies_)
  {
    // Generate random policy if number of stored policies is less then
    // `maxRankedPolicies_`
    for (size_t i = 0; i < _numSplines; i++)
    {
      for (size_t j = 0; j < this->sourceYSize_; j++)
      {
        (*this->currentPolicy_)[i][j] = dist(mt);
      }
    }
  }
  else
  {
    // Generate new policy using weighted crossover operator
    double totalFitness = 0;
    if (this->algorithmType_ == "B" or this->algorithmType_ == "D")
    {
      // k-selection tournament
      auto parent1 = this->BinarySelection();
      auto parent2 = parent1;
      while (parent2 == parent1)
      {
        parent2 = this->BinarySelection();
      }

      double fitness1 = parent1->first;
      double fitness2 = parent2->first;

      PolicyPtr policy1 = parent1->second;
      PolicyPtr policy2 = parent2->second;

      // TODO: Verify what should be total fitness in binary
      totalFitness = fitness1 + fitness2;

      // For each spline
      for (size_t i = 0; i < _numSplines; i++)
      {
        // And for each control point
        for (size_t j = 0; j < this->sourceYSize_; j++)
        {
          // Apply modifier
          double splinePoint = 0;
          splinePoint +=
              ((policy1->at(i)[j] - (*this->currentPolicy_)[i][j])) *
              (fitness1 / totalFitness);
          splinePoint +=
              ((policy2->at(i)[j] - (*this->currentPolicy_)[i][j])) *
              (fitness2 / totalFitness);

          // Add a mutation + current
          // TODO: Verify do we use current in this case
          splinePoint += dist(mt) + (*this->currentPolicy_)[i][j];

          // Set a newly generated point as current
          (*this->currentPolicy_)[i][j] = splinePoint;
        }
      }
    }
    else
    {
      // Default is all parents selection

      // Calculate first total sum of fitnesses
      for (auto const &it : this->rankedPolicies_)
      {
        double fitness = it.first;
        totalFitness += fitness;
      }

      // For each spline
      // TODO: Verify that this should is correct formula
      for (size_t i = 0; i < _numSplines; i++)
      {
        // And for each control point
        for (size_t j = 0; j < this->sourceYSize_; j++)
        {
          // Apply modifier
          double splinePoint = 0;
          for (auto const &it : this->rankedPolicies_)
          {
            double fitness = it.first;
            PolicyPtr policy = it.second;

            splinePoint +=
                ((policy->at(i)[j] - (*this->currentPolicy_)[i][j])) *
                (fitness / totalFitness);
          }

          // Add a mutation + current
          // TODO: Verify do we use 'current_policy_' in this case
          splinePoint += dist(mt) + (*this->currentPolicy_)[i][j];

          // Set a newly generated point as current
          (*this->currentPolicy_)[i][j] = splinePoint;
        }
      }
    }
  }

  // cache update
  this->InterpolateCubic(
      _numSplines,
      this->currentPolicy_.get(),
      this->interpolationCache_.get());
}

/////////////////////////////////////////////////
void RLPower::IncreaseSplinePoints(const size_t _numSplines)
{
  this->sourceYSize_++;

  // LOG code
  this->stepRate_ = this->numInterpolationPoints_ / this->sourceYSize_;

  // Copy current policy for resizing
  Policy policy_copy(this->currentPolicy_->size());
  for (size_t i = 0; i < _numSplines; i++)
  {
    Spline &spline = this->currentPolicy_->at(i);
    policy_copy[i] = Spline(spline.begin(), spline.end());

    spline.resize(this->sourceYSize_);
  }

  this->InterpolateCubic(0, &policy_copy, this->currentPolicy_.get());

  for (auto &it : this->rankedPolicies_)
  {
    PolicyPtr policy = it.second;

    for (size_t j = 0; j < _numSplines; j++)
    {
      Spline &spline = policy->at(j);
      policy_copy[j] = Spline(spline.begin(), spline.end());
      spline.resize(this->sourceYSize_);
    }
    this->InterpolateCubic(0, &policy_copy, policy.get());
  }
}

/////////////////////////////////////////////////
std::map< double, PolicyPtr >::iterator RLPower::BinarySelection()
{
  std::random_device rd;
  std::mt19937 umt(rd());
  std::uniform_int_distribution <size_t> udist(0, this->maxRankedPolicies_ - 1);

  // Select two different numbers from uniform distribution
  // U(0, max_ranked_policies_ - 1
  size_t pindex1, pindex2;
  pindex1 = udist(umt);
  do
  {
    pindex2 = udist(umt);
  } while (pindex1 == pindex2);

  // Set iterators to begin of the 'ranked_policies_' map
  auto individual1 = this->rankedPolicies_.begin();
  auto individual2 = this->rankedPolicies_.begin();

  // Move iterators to indices positions
  std::advance(individual1, pindex1);
  std::advance(individual2, pindex2);

  double fitness1 = individual1->first;
  double fitness2 = individual2->first;

  return fitness1 > fitness2 ? individual1 : individual2;
}

// seconds
const double RLPower::CYCLE_LENGTH = 5;

// sigma decay
const double RLPower::SIGMA = 0.98;

double RLPower::Fitness()
{
  return 0.1;  // this->evaluator_->Fitness();
}

void RLPower::Modify(ConstModifyPolicyPtr &req)
{
  boost::mutex::scoped_lock lock(this->rlpowerMutex_);

  // TODO: Implement the rest of the method
}

void RLPower::Output(
    const size_t _numSplines,
    const double _time,
    double *_output)
{
  if (this->cycleStartTime_ < 0)
  {
    this->cycleStartTime_ = _time;
  }

  // get correct X value (between 0 and CYCLE_LENGTH)
  double x = _time - this->cycleStartTime_;
  while (x >= RLPower::CYCLE_LENGTH)
  {
    this->cycleStartTime_ += RLPower::CYCLE_LENGTH;
    x = _time - this->cycleStartTime_;
  }

  // adjust X on the cache coordinate space
  x = (x / CYCLE_LENGTH) * this->numInterpolationPoints_;
  // generate previous and next values
  auto x_a = ((int)x) % this->numInterpolationPoints_;
  auto x_b = (x_a + 1) % this->numInterpolationPoints_;

  // linear interpolation for every actuator
  for (size_t i = 0; i < _numSplines; i++)
  {
    double y_a = this->interpolationCache_->at(i)[x_a];
    double y_b = this->interpolationCache_->at(i)[x_b];

    _output[i] = y_a + ((y_b - y_a) * (x - x_a) / (x_b - x_a));
  }
}
