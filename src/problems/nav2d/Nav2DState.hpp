#ifndef NAV2D_STATE_HPP_
#define NAV2D_STATE_HPP_

#include <cstddef>                      // for size_t

#include <ostream>                      // for ostream
#include <vector>                       // for vector

#include "problems/shared/GridPosition.hpp"  // for GridPosition
#include "solver/geometry/State.hpp"             // for State
#include "solver/geometry/Vector.hpp"             // for Vector

#include "global.hpp"

double normalizeAngle(double angle) {
    double numRotations;
    angle = std::modf(angle, &numRotations);
    if (angle < -0.5) {
        angle += 1;
    } else if (angle > 0.5) {
        angle -= 1;
    }
    return angle;
}

namespace nav2d {
class Nav2DState : public solver::Vector {
    friend class Nav2DTextSerializer;
  public:
    Nav2DState(double x, double y, double direction,
            double costPerUnitDistance = 1.0, double costPerRevolution = 1.0);

    virtual ~Nav2DState() = default;
    // Copy constructor is allowed, but not others.
    Nav2DState(Nav2DState const &other);
    Nav2DState(Nav2DState &&) = delete;
    Nav2DState &operator=(Nav2DState const &) = delete;
    Nav2DState &operator=(Nav2DState &&) = delete;

    std::unique_ptr<solver::State> copy() const override;
    double distanceTo(solver::State const &otherState) const override;
    bool equals(solver::State const &otherState) const override;
    std::size_t hash() const override;
    void print(std::ostream &os) const override;
    std::vector<double> asVector() const override;

    double getX() const;
    double getY() const;
    double getDirection() const;

  private:
    double costPerUnitDistance_;
    double costPerRevolution_;
    double x_;
    double y_;
    double direction_;
};
} /* namespace nav2d */

#endif /* NAV2DSTATE_HPP_ */
