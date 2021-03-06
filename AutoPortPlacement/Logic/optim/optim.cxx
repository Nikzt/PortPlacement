#include <optim/optim.h>

#include <nlopt.hpp>

#include <math.h>
#include <algorithm>
#include <iostream>

#include <vtkMinimalStandardRandomSequence.h>
#include <vtkNew.h>

namespace
{
  // Represents a minimax optimization problem parameterized as in:
  // http://ab-initio.mit.edu/wiki/index.php/NLopt_Introduction#Equivalent_formulations_of_optimization_problems
  struct FeasiblePlanProblem
  {
    void portConstraint(const Eigen::Matrix4d& baseFrame,
                        const std::vector<double>& q,
                        double* c) const;

    void ikConstraint(const Eigen::Matrix4d& portFrame,
                      const Eigen::Matrix4d& taskFrame,
                      std::vector<double>* c) const;

    void activeClearConstraint(const std::vector<double>& qL,
                               const std::vector<double>& qR,
                               const Eigen::Matrix4d& baseFrameL,
                               const Eigen::Matrix4d& baseFrameR,
                               const Eigen::Matrix4d& taskFrame,
                               std::vector<double>* c) const;

    void passiveClearConstraint(const Eigen::Matrix4d& baseFrameL,
                                const Eigen::Matrix4d& baseFrameR,
                                const std::vector<double>& qL,
                                const std::vector<double>& qR,
                                double *c) const;

    const DavinciKinematics& kin;
    const Eigen::Matrix3d& baseOrientationL;
    const Eigen::Matrix3d& baseOrientationR;
    const Eigen::Vector3d& baseBoxMin;
    const Eigen::Vector3d& baseBoxMax;
    const Optim::Matrix4dVec& taskFrames;
    const Eigen::Vector3d& portCurvePoint1;
    const Eigen::Vector3d& portCurvePoint2;

    static void wrapIneq(unsigned int m,
                         double* result,
                         unsigned int n,
                         const double* x,
                         double* grad,
                         void* data);

    static double feasibleMinimaxObj(const std::vector<double>& x,
                                     std::vector<double>& grad,
                                     void* data);

    void getBounds(std::vector<double>* lb, std::vector<double>* ub) const;

    void getInitialGuessIK(std::vector<double>* x0) const;

    void outputStateProperties(std::ostream& out, const std::vector<double>& x) const;

    unsigned getNumVariables() const;
    unsigned getNumConstraints() const;

    void getBaseFrames(const Eigen::Vector3d& basePosition,
                       Eigen::Matrix4d* baseFrameL,
                       Eigen::Matrix4d* baseFrameR) const;
  };

  // http://www.quantstart.com/articles/Statistical-Distributions-in-C
  double gaussianQuantile(double quantile)
  {
    static double a[4] = {   2.50662823884,
                             -18.61500062529,
                             41.39119773534,
                             -25.44106049637};

    static double b[4] = {  -8.47351093090,
                            23.08336743743,
                            -21.06224101826,
                            3.13082909833};

    static double c[9] = {0.3374754822726147,
                          0.9761690190917186,
                          0.1607979714918209,
                          0.0276438810333863,
                          0.0038405729373609,
                          0.0003951896511919,
                          0.0000321767881768,
                          0.0000002888167364,
                          0.0000003960315187};

    if (quantile >= 0.5 && quantile <= 0.92)
      {
      double num = 0.0;
      double denom = 1.0;

      for (int i=0; i<4; i++)
        {
        num += a[i] * pow((quantile - 0.5), 2*i + 1);
        denom += b[i] * pow((quantile - 0.5), 2*i);
        }
      return num/denom;

      }
    else if (quantile > 0.92 && quantile < 1)
      {
      double num = 0.0;

      for (int i=0; i<9; i++)
        {
        num += c[i] * pow((log(-log(1-quantile))), i);
        }
      return num;

      }
    else
      {
      return -1.0*gaussianQuantile(1-quantile);
      }
  }
}

bool Optim::findFeasiblePlan(const DavinciKinematics& kin,
                             const Eigen::Matrix3d& baseOrientationL,
                             const Eigen::Matrix3d& baseOrientationR,
                             const Eigen::Vector3d& baseBoxMin,
                             const Eigen::Vector3d& baseBoxMax,
                             const Matrix4dVec& taskFrames,
                             const Eigen::Vector3d& portCurvePoint1,
                             const Eigen::Vector3d& portCurvePoint2,
                             std::vector<double>* qL_out,
                             std::vector<double>* qR_out,
                             Eigen::Vector3d* basePosition)
{
  FeasiblePlanProblem problem = {kin, baseOrientationL, baseOrientationR,
                                 baseBoxMin, baseBoxMax, taskFrames,
                                 portCurvePoint1, portCurvePoint2};

  const unsigned numVariables = problem.getNumVariables();

  nlopt::opt opt(nlopt::LN_COBYLA, numVariables);

  std::vector<double> lb(numVariables);
  std::vector<double> ub(numVariables);
  problem.getBounds(&lb, &ub);
  opt.set_lower_bounds(lb);
  opt.set_upper_bounds(ub);

  opt.set_min_objective(&FeasiblePlanProblem::feasibleMinimaxObj, 0);

  std::size_t numIneqConstraints = problem.getNumConstraints();
  std::vector<double> ineqTol(numIneqConstraints, 0.0000001);
  opt.add_inequality_mconstraint(&FeasiblePlanProblem::wrapIneq,
                                 (void*) &problem,
                                 ineqTol);

  // Stop the optimization after some seconds have passed no matter what
  opt.set_maxtime(300.0);

  std::vector<double> x(numVariables);
  problem.getInitialGuessIK(&x);

  // Output initial value, objective and constraint costs
  std::cout << "================== x0 =================" << std::endl;
  problem.outputStateProperties(std::cout, x);

  double minf;
  nlopt::result result;
  bool validResult = true;
  try
    {
    result = opt.optimize(x, minf);
    }
  catch (nlopt::roundoff_limited& e)
    {
    std::cout << "warning: Optimization ended due to roundoff errors." << std::endl;
    validResult = false;
    }

  if (validResult)
    {
    std::cout << "=================" << std::endl;
    std::cout << "Result: " << result << std::endl;
    std::cout << "=================" << std::endl;
    }

  std::cout << "================== x_opt ==============" << std::endl;
  problem.outputStateProperties(std::cout, x);

  std::copy(x.begin(), x.begin()+6, qL_out->begin());
  std::copy(x.begin()+6, x.begin()+12, qR_out->begin());

  for (unsigned i = 0; i < 3; ++i)
    (*basePosition)(i) = x[12+i];

  return true;
}

void FeasiblePlanProblem::portConstraint(const Eigen::Matrix4d& baseFrame,
                                         const std::vector<double>& q,
                                         double* c) const
{
  Eigen::Matrix4d rcm = this->kin.passiveFK(baseFrame, q);
  double d = Collisions::distance(this->portCurvePoint1, this->portCurvePoint2,
                                  rcm.topRightCorner<3,1>());
  *c = d*d - 0.001*0.001;
}

void FeasiblePlanProblem::ikConstraint(const Eigen::Matrix4d& portFrame,
                                       const Eigen::Matrix4d& taskFrame,
                                       std::vector<double>* c) const
{
  std::vector<double> mean_q;
  std::vector<double> cov_q;
  const double POSITION_VARIANCE = 1e-6;
  const double ORIENTATION_VARIANCE = 1e-6;
  const double CHANCE_CONSTRAINT = 0.1;
  double quantile = gaussianQuantile(1 - CHANCE_CONSTRAINT);
  this->kin.unscentedIK(portFrame, taskFrame,
                        Eigen::Vector3d::Constant(POSITION_VARIANCE),
                        Eigen::Vector3d::Constant(ORIENTATION_VARIANCE),
                        &mean_q, &cov_q);
  for (unsigned i = 0; i < 6; ++i)
    {
    double midpt = 0.5*(this->kin.getActiveJointMax(i) - this->kin.getActiveJointMin(i));
    (*c)[i] = fabs(mean_q[i] - midpt) + quantile*sqrt(cov_q[i]) - midpt;
    }
}

void FeasiblePlanProblem::activeClearConstraint(const std::vector<double>& qpL,
                                                const std::vector<double>& qpR,
                                                const Eigen::Matrix4d& baseFrameL,
                                                const Eigen::Matrix4d& baseFrameR,
                                                const Eigen::Matrix4d& taskFrame,
                                                std::vector<double>* c) const
{
  const double POSITION_VARIANCE = 1e-6;
  const double ORIENTATION_VARIANCE = 1e-6;
  const double CHANCE_CONSTRAINT = 0.1;
  double quantile = gaussianQuantile(1 - CHANCE_CONSTRAINT);
  std::vector<double> mean_c, cov_c;
  this->kin.unscentedClearance(baseFrameL, baseFrameR, qpL, qpR, taskFrame,
                               Eigen::Vector3d::Constant(POSITION_VARIANCE),
                               Eigen::Vector3d::Constant(ORIENTATION_VARIANCE),
                               &mean_c, &cov_c);
  for (unsigned i = 0; i < this->kin.numActiveClearances(); ++i)
    {
    (*c)[i] = quantile*sqrt(cov_c[i]) - mean_c[i];
    }
}

void FeasiblePlanProblem::passiveClearConstraint(const Eigen::Matrix4d& baseFrameL,
                                                 const Eigen::Matrix4d& baseFrameR,
                                                 const std::vector<double>& qL,
                                                 const std::vector<double>& qR,
                                                 double *c) const
{
  std::vector<Collisions::Cylisphere> cL, cR;
  std::vector<Collisions::Sphere> sL(1), sR(1);
  this->kin.getPassivePrimitives(baseFrameL, qL, &cL, &sL[0]);
  this->kin.getPassivePrimitives(baseFrameR, qR, &cR, &sR[0]);
  double d = Collisions::distance(cL, sL, cR, sR);
  *c = -d;
}

// inequality constraints:
// 1 passive clear constraint
// 2 port constraints
// k*6*2 ik constraints
// k*1 active clear constraints
void FeasiblePlanProblem::wrapIneq(unsigned int m,
                                   double* result,
                                   unsigned int n,
                                   const double* x,
                                   double* /*grad*/,
                                   void* data)
{
  FeasiblePlanProblem* problem = reinterpret_cast<FeasiblePlanProblem*>(data);

  if (m != problem->getNumConstraints())
    throw std::runtime_error("wrapIneq Error: wrong number of constraints!");

  if (n != problem->getNumVariables())
    throw std::runtime_error("wrapIneq Error: wrong number of variables!");

  std::vector<double> qL(&x[0], &x[6]);
  std::vector<double> qR(&x[6], &x[12]);

  // Get robot base frame for this value of x
  Eigen::Vector3d basePosition;
  for (unsigned i = 0; i < 3; ++i)
    basePosition(i) = x[12+i];
  Eigen::Matrix4d baseFrameL, baseFrameR;
  problem->getBaseFrames(basePosition, &baseFrameL, &baseFrameR);

  // passive clear constraint
  problem->passiveClearConstraint(baseFrameL, baseFrameR, qL, qR, &result[0]);

  // port constraints
  problem->portConstraint(baseFrameL, qL, &result[1]);
  problem->portConstraint(baseFrameR, qR, &result[2]);

  // ik constraints
  std::vector<double> c(6);
  Eigen::Matrix4d portFrameL = problem->kin.passiveFK(baseFrameL, qL);
  Eigen::Matrix4d portFrameR = problem->kin.passiveFK(baseFrameR, qR);
  for (std::size_t k = 0; k < problem->taskFrames.size(); ++k)
    {
    problem->ikConstraint(portFrameL,
                          problem->taskFrames[k],
                          &c);
    for (unsigned i = 0; i < 6; ++i)
      result[3+(k*2*6)+i] = c[i];

    problem->ikConstraint(portFrameR,
                          problem->taskFrames[k],
                          &c);
    for (unsigned i = 0; i < 6; ++i)
      result[3+(k*2*6)+i+6] = c[i];
    }

  // active clearance constraints
  for (unsigned k = 0; k < problem->taskFrames.size(); ++k)
    {
    std::vector<double> c(problem->kin.numActiveClearances());
    problem->activeClearConstraint(qL, qR, baseFrameL, baseFrameR, problem->taskFrames[k],
                                   &c);
    for (unsigned i = 0; i < c.size(); ++i)
      result[3+(problem->taskFrames.size()*2*6) + c.size()*k + i] = c[i];
    }

  // make sure to subtract away t from all constraints
  double t = x[15];
  for (unsigned i = 0; i < m; ++i)
    result[i] -= t;
}

double FeasiblePlanProblem::feasibleMinimaxObj(const std::vector<double>& x,
                                               std::vector<double>& /*grad*/,
                                               void* /*data*/)
{
  return x[15];
}

void FeasiblePlanProblem::getBounds(std::vector<double>* lb,
                                    std::vector<double>* ub) const
{
  for (unsigned i = 0; i < 6; ++i)
    {
    (*lb)[i] = (*lb)[6+i] = this->kin.getPassiveJointMin(i);
    (*ub)[i] = (*ub)[6+i] = this->kin.getPassiveJointMax(i);
    }
  for (unsigned i = 0; i < 3; ++i)
    {
    (*lb)[12+i] = baseBoxMin(i);
    (*ub)[12+i] = baseBoxMax(i);
    }
  (*lb)[15] = -100.0; // trying to make an order of magnitude or 2 greater than constraint values
  (*ub)[15] = 100.0; // trying to make an order of magnitude or 2 greater than constraint values
}

void FeasiblePlanProblem::getInitialGuessIK(std::vector<double>* x) const
{
  // Use center of baseBox as initial guess for robot base position
  Eigen::Vector3d baseBoxCenter = (baseBoxMin + baseBoxMax) / 2.0;
  Eigen::Matrix4d baseFrameL, baseFrameR;
  this->getBaseFrames(baseBoxCenter, &baseFrameL, &baseFrameR);

  vtkNew<vtkMinimalStandardRandomSequence> rng;

  // Use Jacobian IK to find a nice initial guess
  // Place RCM's at some points on port curve
  rng->Next();
  double curveParamL = rng->GetValue();
  rng->Next();
  double curveParamR = rng->GetValue();;
  Eigen::Vector3d rcmL =
    this->portCurvePoint1 + curveParamL*(this->portCurvePoint2 - this->portCurvePoint1);
  Eigen::Vector3d rcmR =
    this->portCurvePoint1 + curveParamR*(this->portCurvePoint2 - this->portCurvePoint1);
  std::vector<double> qL(6), qR(6);
  for (unsigned i = 0; i < 6; ++i)
    {
    rng->Next();
    qL[i] = this->kin.getPassiveJointMin(i) +
      rng->GetValue()*(this->kin.getPassiveJointMax(i) - this->kin.getPassiveJointMin(i));
    rng->Next();
    qR[i] = this->kin.getPassiveJointMin(i) +
      rng->GetValue()*(this->kin.getPassiveJointMax(i) - this->kin.getPassiveJointMin(i));
    }
  kin.passiveIK(baseFrameL, rcmL, &qL);
  std::cout << "Left IK done!" << std::endl; // debug
  std::cout << "left error: "
            << (kin.passiveFK(baseFrameL, qL).topRightCorner<3,1>() - rcmL).norm()
            << std::endl;

  kin.passiveIK(baseFrameR, rcmR, &qR);
  std::cout << "Right IK done!" << std::endl; // debug
  std::cout << "right error: "
            << (kin.passiveFK(baseFrameR, qR).topRightCorner<3,1>() - rcmR).norm()
            << std::endl;


  std::copy(qL.begin(), qL.end(), x->begin());
  std::copy(qR.begin(), qR.end(), x->begin()+6);
  for (unsigned i = 0; i < 3; ++i)
    (*x)[12+i] = baseBoxCenter(i);
  (*x)[15] = 90.0;
}

void FeasiblePlanProblem::outputStateProperties(std::ostream& out,
                                                const std::vector<double>& x) const
{
  out << "x:";
  for (std::size_t i = 0; i < x.size(); ++i)
    out << " " << x[i];
  out << std::endl;

  std::vector<double> dummy;
  out << "f(x): " << FeasiblePlanProblem::feasibleMinimaxObj(x, dummy, (void*) this)  << std::endl;

  double* c_ineq = new double[this->getNumConstraints()];
  double* x_array = new double[this->getNumVariables()];
  for (unsigned i = 0; i < this->getNumVariables(); ++i)
    x_array[i] = x[i];
  FeasiblePlanProblem::wrapIneq(this->getNumConstraints(), c_ineq, this->getNumVariables(), x_array, 0, (void*) this);
  out << "c_ineq(x):";
  for (unsigned i = 0; i < this->getNumConstraints(); ++i)
    out << " " << (c_ineq[i] + x[15]);
  out << std::endl;

  Eigen::Matrix4d baseFrameL, baseFrameR;
  Eigen::Vector3d basePosition;
  for (unsigned i = 0; i < 3; ++i)
    basePosition(i) = x[12+i];
  this->getBaseFrames(basePosition, &baseFrameL, &baseFrameR);

  std::vector<double> qpL(x.begin(), x.begin()+6);
  std::vector<double> qpR(x.begin()+6, x.begin()+12);
  std::vector<double> qaL(6);
  std::vector<double> qaR(6);
  this->kin.intraIK(this->kin.passiveFK(baseFrameL, qpL), this->taskFrames[0], &qaL);
  this->kin.intraIK(this->kin.passiveFK(baseFrameR, qpR), this->taskFrames[0], &qaR);
  out << "aL:";
  for (unsigned i = 0; i < 6; ++i)
    out << " " << qaL[i];
  out << std::endl << "aR:";
  for (unsigned i = 0; i < 6; ++i)
    out << " " << qaR[i];
  out << std::endl << "base:";
  for (unsigned i = 0; i < 3; ++i)
    out << " " << x[12+i];
  out << std::endl;

  delete [] c_ineq;
  delete [] x_array;
}

unsigned FeasiblePlanProblem::getNumVariables() const
{
  return 16;
}

unsigned FeasiblePlanProblem::getNumConstraints() const
{
  return 3 + (12+kin.numActiveClearances())*taskFrames.size();
}

void FeasiblePlanProblem::getBaseFrames(const Eigen::Vector3d& basePosition,
                                        Eigen::Matrix4d* baseFrameL,
                                        Eigen::Matrix4d* baseFrameR) const
{
  baseFrameL->setIdentity();
  baseFrameR->setIdentity();
  baseFrameL->topLeftCorner<3,3>() = this->baseOrientationL;
  baseFrameR->topLeftCorner<3,3>() = this->baseOrientationR;
  baseFrameL->topRightCorner<3,1>() = basePosition;
  baseFrameR->topRightCorner<3,1>() = basePosition;
}
