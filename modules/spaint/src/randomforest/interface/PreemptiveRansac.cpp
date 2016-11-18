/**
 * spaint: PreemptiveRansac.cpp
 * Copyright (c) Torr Vision Group, University of Oxford, 2016. All rights reserved.
 */

#include "randomforest/interface/PreemptiveRansac.h"

#include <boost/timer/timer.hpp>

#include <ceres/ceres.h>
#include <ceres/rotation.h>

#include <Eigen/Dense>
#include <libalglib/optimization.h>
#include <omp.h>
#include "ORUtils/SE3Pose.h"

#include "util/MemoryBlockFactory.h"

//#define ENABLE_TIMERS

namespace spaint
{
PreemptiveRansac::PreemptiveRansac()
{
  // Set params as in scoreforests
  m_nbPointsForKabschBoostrap = 3;
  m_useAllModesPerLeafInPoseHypothesisGeneration = true;
  m_checkMinDistanceBetweenSampledModes = true;
  m_minSquaredDistanceBetweenSampledModes = 0.3f * 0.3f;
//  m_checkRigidTransformationConstraint = false; // Speeds up a lot, was true in scoreforests
  m_checkRigidTransformationConstraint = true;
  m_translationErrorMaxForCorrectPose = 0.05f;
  m_batchSizeRansac = 500;
  m_trimKinitAfterFirstEnergyComputation = 64;
//  m_trimKinitAfterFirstEnergyComputation = 1024;
  m_poseUpdate = true; // original
//  m_poseUpdate = false; // faster, might be OK
  m_usePredictionCovarianceForPoseOptimization = true; // original implementation
//  m_usePredictionCovarianceForPoseOptimization = false;
  m_poseOptimizationInlierThreshold = 0.2f;

  const MemoryBlockFactory &mbf = MemoryBlockFactory::instance();

  m_nbMaxPoseCandidates = 1024;
  m_poseCandidates = mbf.make_block<PoseCandidate>(m_nbMaxPoseCandidates);

  m_nbMaxInliers = 3000; // 500 per ransac iteration, starting from 64, not 1024.
  m_inliersIndicesImage = mbf.make_image<int>(m_nbMaxInliers);
  m_inliersMaskImage = mbf.make_image<int>();
}

PreemptiveRansac::~PreemptiveRansac()
{
}

int PreemptiveRansac::get_min_nb_required_points() const
{
  return std::max(m_nbPointsForKabschBoostrap, m_batchSizeRansac);
}

boost::optional<PoseCandidate> PreemptiveRansac::estimate_pose(
    const RGBDPatchFeatureImage_CPtr &features,
    const GPUForestPredictionsImage_CPtr &forestPredictions)
{
  m_featureImage = features;
  m_predictionsImage = forestPredictions;

  m_featureImage->UpdateHostFromDevice(); // Need the features on the host for now
  m_predictionsImage->UpdateHostFromDevice(); // Also the predictions

  {
#ifdef ENABLE_TIMERS
    boost::timer::auto_cpu_timer t(6,
        "generating initial candidates: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
    generate_pose_candidates();
  }

  PoseCandidate *candidates = m_poseCandidates->GetData(MEMORYDEVICE_CPU);

  // Reset the number of inliers for the new pose estimation.
  m_inliersIndicesImage->dataSize = 0;

  if (m_trimKinitAfterFirstEnergyComputation < m_poseCandidates->dataSize)
  {
#ifdef ENABLE_TIMERS
    boost::timer::auto_cpu_timer t(6,
        "first trim: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif

    {
#ifdef ENABLE_TIMERS
      boost::timer::auto_cpu_timer t(6,
          "sample inliers: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
      sample_inlier_candidates(false); // no mask for the first pass
    }

    {
#ifdef ENABLE_TIMERS
      boost::timer::auto_cpu_timer t(6,
          "compute and sort energies: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
      compute_and_sort_energies();
    }

    m_poseCandidates->dataSize = m_trimKinitAfterFirstEnergyComputation;
  }

  //  std::cout << candidates.size() << " candidates remaining." << std::endl;
  //  std::cout << "Premptive RANSAC" << std::endl;

#ifdef ENABLE_TIMERS
  boost::timer::auto_cpu_timer t(6,
      "ransac: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif

  // Reset inlier mask (and inliers)
  m_inliersMaskImage->ChangeDims(m_featureImage->noDims); // Happens only once
  m_inliersMaskImage->Clear();
  m_inliersIndicesImage->dataSize = 0;

  float iteration = 0.0f;

  while (m_poseCandidates->dataSize > 1)
  {
    //    boost::timer::auto_cpu_timer t(
    //        6, "ransac iteration: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
    ++iteration;
    //    std::cout << candidates.size() << " camera remaining" << std::endl;

    sample_inlier_candidates(true);

    if (m_poseUpdate)
    {
      update_candidate_poses();
    }

    compute_and_sort_energies();

    // Remove half of the candidates with the worse energies
    m_poseCandidates->dataSize /= 2;
  }

  return
      m_poseCandidates->dataSize > 0 ?
          candidates[0] : boost::optional<PoseCandidate>();
}

namespace
{
void Kabsch_impl(Eigen::MatrixXf &P, Eigen::MatrixXf &Q,
    Eigen::VectorXf &weights, Eigen::MatrixXf &resRot,
    Eigen::VectorXf &resTrans)
{
  if (P.cols() != Q.cols() || P.rows() != Q.rows())
    throw std::runtime_error("Kabsch: P and Q have different dimensions");
  int D = P.rows();  // dimension of the space
  int N = P.cols();  // number of points
  Eigen::VectorXf normalizedWeights = Eigen::VectorXf(weights.size());

  // normalize weights to sum to 1
  {
    float sumWeights = 0;
    for (int i = 0; i < weights.size(); ++i)
    {
      sumWeights += weights(i);
    }
    normalizedWeights = weights * (1.0f / sumWeights);
  }

  // Centroids
  Eigen::VectorXf p0 = P * normalizedWeights;
  Eigen::VectorXf q0 = Q * normalizedWeights;
  Eigen::VectorXf v1 = Eigen::VectorXf::Ones(N);

  Eigen::MatrixXf P_centred = P - p0 * v1.transpose(); // translating P to center the origin
  Eigen::MatrixXf Q_centred = Q - q0 * v1.transpose(); // translating Q to center the origin

      // Covariance between both matrices
  Eigen::MatrixXf C = P_centred * normalizedWeights.asDiagonal()
      * Q_centred.transpose();

  // SVD
  Eigen::JacobiSVD<Eigen::MatrixXf> svd(C,
      Eigen::ComputeThinU | Eigen::ComputeThinV);

  Eigen::MatrixXf V = svd.matrixU();
  Eigen::VectorXf S = svd.singularValues();
  Eigen::MatrixXf W = svd.matrixV();
  Eigen::MatrixXf I = Eigen::MatrixXf::Identity(D, D);

  if ((V * W.transpose()).determinant() < 0)
    I(D - 1, D - 1) = -1;

  // Recover the rotation and translation
  resRot = W * I * V.transpose();
  resTrans = q0 - resRot * p0;

  return;
}

void Kabsch_impl(Eigen::MatrixXf &P, Eigen::MatrixXf &Q,
    Eigen::MatrixXf &resRot, Eigen::VectorXf &resTrans)
{
  Eigen::VectorXf weights = Eigen::VectorXf::Ones(P.cols());
  Kabsch_impl(P, Q, weights, resRot, resTrans);
}
}

Eigen::Matrix4f PreemptiveRansac::Kabsch(Eigen::MatrixXf &P,
    Eigen::MatrixXf &Q) const
{
  Eigen::MatrixXf resRot;
  Eigen::VectorXf resTrans;

  Kabsch_impl(P, Q, resRot, resTrans);

  // recompose R + t in Rt
  Eigen::Matrix4f res;
  res.block<3, 3>(0, 0) = resRot;
  res.block<3, 1>(0, 3) = resTrans;
  return res;
}

void PreemptiveRansac::update_candidate_poses()
{
  const size_t nbPoseCandidates = m_poseCandidates->dataSize;
  PoseCandidate *poseCandidates = m_poseCandidates->GetData(MEMORYDEVICE_CPU);

#pragma omp parallel for schedule(dynamic)
  for (size_t i = 0; i < nbPoseCandidates; ++i)
  {
    update_candidate_pose(poseCandidates[i]);
  }
}

namespace
{
struct PointForLM
{
  Vector3f point;
  GPUForestMode mode;

  PointForLM()
  {
  }

  PointForLM(const Vector3f &pt, const GPUForestMode &md) :
      point(pt), mode(md)
  {
  }

  template<typename T>
  inline bool operator()(const T * const camera, T *residual) const
  {
    T pointT[3];
    pointT[0] = T(point.x);
    pointT[1] = T(point.y);
    pointT[2] = T(point.z);

    T transformedPt[3];

    // rotation
    ceres::AngleAxisRotatePoint(&camera[3], pointT, transformedPt);
    // translation
    transformedPt[0] += camera[0];
    transformedPt[1] += camera[1];
    transformedPt[2] += camera[2];

    T modePosition[3];
    modePosition[0] = T(mode.position.x);
    modePosition[1] = T(mode.position.y);
    modePosition[2] = T(mode.position.z);

    T pointDiff[3];
    pointDiff[0] = transformedPt[0] - modePosition[0];
    pointDiff[1] = transformedPt[1] - modePosition[1];
    pointDiff[2] = transformedPt[2] - modePosition[2];

    T modeInverseCovariance[9];
    // Col major to row major to perform dot product later
    modeInverseCovariance[0] = T(mode.positionInvCovariance.m[0]);
    modeInverseCovariance[1] = T(mode.positionInvCovariance.m[3]);
    modeInverseCovariance[2] = T(mode.positionInvCovariance.m[6]);

    modeInverseCovariance[3] = T(mode.positionInvCovariance.m[1]);
    modeInverseCovariance[4] = T(mode.positionInvCovariance.m[4]);
    modeInverseCovariance[5] = T(mode.positionInvCovariance.m[7]);

    modeInverseCovariance[6] = T(mode.positionInvCovariance.m[2]);
    modeInverseCovariance[7] = T(mode.positionInvCovariance.m[5]);
    modeInverseCovariance[8] = T(mode.positionInvCovariance.m[8]);

    // compute the mahalanobis square distance
    T firstDot[3];
    firstDot[0] = ceres::DotProduct(&modeInverseCovariance[0], pointDiff);
    firstDot[1] = ceres::DotProduct(&modeInverseCovariance[3], pointDiff);
    firstDot[2] = ceres::DotProduct(&modeInverseCovariance[6], pointDiff);

    // Finish computing the distance
    residual[0] = ceres::DotProduct(pointDiff, firstDot);
//    residual[0] = ceres::DotProduct(pointDiff, pointDiff);
    return true;
  }
};

typedef std::vector<PointForLM> PointsForLM;

//struct PointsForLM
//{
//  std::vector<Vector3f, GPUForestMode>> pts;
////  PointsForLM(int nbPts) :
////      pts(nbPts), blurred_img(NULL)
////  {
////  }
////  ~PointsForLM()
////  {
////  }
////  std::vector<
////      std::pair<std::vector<Eigen::VectorXd>,
////          std::vector<PredictedGaussianMean *>>> pts;
////  GaussianAggregatedRGBImage *blurred_img;
//};

static double EnergyForContinuous3DOptimizationUsingFullCovariance(
    const PointsForLM &pts, const ORUtils::SE3Pose &candidateCameraPose)
{
  double res = 0.0;

  for (size_t i = 0; i < pts.size(); ++i)
  {
    const PointForLM &pt = pts[i];
    const Vector3f transformedPt = candidateCameraPose.GetM() * pt.point;
    const Vector3f diff = transformedPt - pt.mode.position;
    const double err = dot(diff, pt.mode.positionInvCovariance * diff); // Mahalanobis sqr distance
    res += err;
  }

  return res;
}

static void Continuous3DOptimizationUsingFullCovariance(
    const alglib::real_1d_array &ksi, alglib::real_1d_array &fi, void *ptr)
{
  const PointsForLM *ptsLM = reinterpret_cast<PointsForLM *>(ptr);
  const ORUtils::SE3Pose testPose(ksi[0], ksi[1], ksi[2], ksi[3], ksi[4],
      ksi[5]);

  fi[0] = EnergyForContinuous3DOptimizationUsingFullCovariance(*ptsLM,
      testPose);
}

/***************************************************/
/* Routines to optimize the sum of 3D L2 distances */
/***************************************************/

static double EnergyForContinuous3DOptimizationUsingL2(const PointsForLM &pts,
    const ORUtils::SE3Pose &candidateCameraPose)
{
  double res = 0.0;

  for (size_t i = 0; i < pts.size(); ++i)
  {
    const PointForLM &pt = pts[i];
    const Vector3f transformedPt = candidateCameraPose.GetM() * pt.point;
    const Vector3f diff = transformedPt - pt.mode.position;
    const double err = length(diff); // sqr distance
    res += err;
  }

  return res;
}

static void Continuous3DOptimizationUsingL2(const alglib::real_1d_array &ksi,
    alglib::real_1d_array &fi, void *ptr)
{
  const PointsForLM *ptsLM = reinterpret_cast<PointsForLM *>(ptr);
  const ORUtils::SE3Pose testPose(ksi[0], ksi[1], ksi[2], ksi[3], ksi[4],
      ksi[5]);

  fi[0] = EnergyForContinuous3DOptimizationUsingL2(*ptsLM, testPose);
}

static void call_after_each_step(const alglib::real_1d_array &x, double func,
    void *ptr)
{
  return;
}
}

bool PreemptiveRansac::update_candidate_pose(PoseCandidate &poseCandidate) const
{
  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
      MEMORYDEVICE_CPU);
  const GPUForestPrediction *predictionsData = m_predictionsImage->GetData(
      MEMORYDEVICE_CPU);
  const size_t nbInliers = m_inliersIndicesImage->dataSize;
  const int *inliersData = m_inliersIndicesImage->GetData(MEMORYDEVICE_CPU);

  ORUtils::SE3Pose candidateCameraPose(poseCandidate.cameraPose);

#if 0
  // Build the problem.
  ceres::Problem problem;

  double cameraState[6];

  for (int i = 0; i < 6; ++i)
  {
    cameraState[i] = candidateCameraPose.GetParams()[i];
  }

  for (int inlierIdx = 0; inlierIdx < poseCandidate.nbInliers; ++inlierIdx)
  {
    const PoseCandidate::Inlier &inlier = poseCandidate.inliers[inlierIdx];
    const Vector3f inlierCameraPosition =
    patchFeaturesData[inlier.linearIdx].position.toVector3();
    const Vector3f inlierWorldPosition = candidateCameraPose.GetM()
    * inlierCameraPosition;
    const GPUForestPrediction &prediction = predictionsData[inlier.linearIdx];
    // The assumption is that the inlier is valid (checked before)

    // Find the best mode
    // (do not rely on the one stored in the inlier because for the randomly sampled inliers it's not set)
    int bestModeIdx = prediction.get_best_mode(inlierWorldPosition);
    if (bestModeIdx < 0 || bestModeIdx >= prediction.nbModes)
    throw std::runtime_error("best mode idx invalid.");// should have not been selected as inlier

    if (length(prediction.modes[bestModeIdx].position - inlierWorldPosition)
        < m_poseOptimizationInlierThreshold)
    {

      ceres::CostFunction *costFunction = new ceres::AutoDiffCostFunction<
      PointForLM, 1, 6>(
          new PointForLM(inlierCameraPosition, prediction.modes[bestModeIdx]));

//      ceres::CostFunction *costFunction = new ceres::NumericDiffCostFunction<
//          PointForLM, ceres::CENTRAL, 1, 6>(
//          new PointForLM(inlierCameraPosition, prediction.modes[bestModeIdx]));
      problem.AddResidualBlock(costFunction,
          new ceres::CauchyLoss(
              m_poseOptimizationInlierThreshold
              * m_poseOptimizationInlierThreshold), cameraState);
    }
  }

  if (problem.NumResidualBlocks() > 3)
  {
    ceres::Solver::Options options;
//    options.gradient_tolerance = 1e-6; // as in the alglib implementation
//    options.linear_solver_type = ceres::DENSE_QR;
//  options.minimizer_progress_to_stdout = true;
//  options.update_state_every_iteration = true;

    // Run the solver.
    ceres::Solver::Summary summary;
    Solve(options, &problem, &summary);

//    std::cout << summary.FullReport() << '\n';

    if (summary.termination_type == ceres::TerminationType::CONVERGENCE)
    {
      candidateCameraPose.SetFrom(cameraState[0], cameraState[1],
          cameraState[2], cameraState[3], cameraState[4], cameraState[5]);
      poseCandidate.cameraPose = candidateCameraPose.GetM();

      // Output a report.
//    std::cout << summary.BriefReport() << "\n";

      return true;
    }
  }

  return false;

#else

  PointsForLM ptsForLM;
  for (size_t inlierIdx = 0; inlierIdx < nbInliers; ++inlierIdx)
  {
    const int inlierLinearIdx = inliersData[inlierIdx];
    const Vector3f inlierCameraPosition =
        patchFeaturesData[inlierLinearIdx].position.toVector3();
    const Vector3f inlierWorldPosition = candidateCameraPose.GetM()
        * inlierCameraPosition;
    const GPUForestPrediction &prediction = predictionsData[inlierLinearIdx];

    PointForLM ptLM;
    // The assumption is that the inlier is valid (checked before)
    ptLM.point = inlierCameraPosition;

    // Find the best mode
    // (do not rely on the one stored in the inlier because for the randomly sampled inliers it's not set)
    const int bestModeIdx = prediction.get_best_mode(inlierWorldPosition);
    if (bestModeIdx < 0 || bestModeIdx >= prediction.nbModes)
      throw std::runtime_error("best mode idx invalid."); // should have not been selected as inlier
    ptLM.mode = prediction.modes[bestModeIdx];

    if (length(ptLM.mode.position - inlierWorldPosition)
        < m_poseOptimizationInlierThreshold)
    {
      ptsForLM.push_back(ptLM);
    }
  }

// Continuous optimization
  if (ptsForLM.size() > 3)
  {
    const float *ksiF = candidateCameraPose.GetParams();
    double ksiD[6];

    // Cast to double
    for (int i = 0; i < 6; ++i)
      ksiD[i] = ksiF[i];

    alglib::real_1d_array ksi_;
    ksi_.setcontent(6, ksiD);

//    Eigen::MatrixXd candidateCameraPoseD = candidateCameraPose.cast<double>();
//
//    Eigen::VectorXd ksivd = Helpers::LieGroupToLieAlgebraSE3(
//        candidateCameraPoseD);
//
//    for (int i = 0; i < 6; ++i)
//    {
//      ksiD[i] = ksivd(i);
//    }
//
//    ksi_.setcontent(6, ksiD);

    alglib::minlmstate state;
    alglib::minlmreport rep;

    double differentiationStep = 0.0001;
    alglib::minlmcreatev(6, 1, ksi_, differentiationStep, state);

    double epsg = 0.000001;
    double epsf = 0;
    double epsx = 0;
    alglib::ae_int_t maxits = 100;
    alglib::minlmsetcond(state, epsg, epsf, epsx, maxits);

    double energyBefore, energyAfter;
    if (m_usePredictionCovarianceForPoseOptimization)
    {
      energyBefore = EnergyForContinuous3DOptimizationUsingFullCovariance(
          ptsForLM, candidateCameraPose);
      alglib::minlmoptimize(state, Continuous3DOptimizationUsingFullCovariance,
          call_after_each_step, &ptsForLM);
    }
    else
    {
      energyBefore = EnergyForContinuous3DOptimizationUsingL2(ptsForLM,
          candidateCameraPose);
      alglib::minlmoptimize(state, Continuous3DOptimizationUsingL2,
          call_after_each_step, &ptsForLM);
    }

    alglib::minlmresults(state, ksi_, rep);

    candidateCameraPose.SetFrom(ksi_[0], ksi_[1], ksi_[2], ksi_[3], ksi_[4],
        ksi_[5]);

//    memcpy(ksiD, ksi_.getcontent(), sizeof(double) * 6);
//    for (int i = 0; i < 6; ++i)
//    {
//      ksivd(i) = ksiD[i];
//    }
//    Eigen::MatrixXd updatedCandidateCameraPoseD =
//        Helpers::LieAlgebraToLieGroupSE3(ksivd);

    if (m_usePredictionCovarianceForPoseOptimization)
    {
      energyAfter = EnergyForContinuous3DOptimizationUsingFullCovariance(
          ptsForLM, candidateCameraPose);
    }
    else
    {
      energyAfter = EnergyForContinuous3DOptimizationUsingL2(ptsForLM,
          candidateCameraPose);
    }

    if (energyAfter < energyBefore)
    {
      poseCandidate.cameraPose = candidateCameraPose.GetM();
      return true;
    }
  }

////////////////////////////

//  std::vector<std::pair<int, int>> &samples = std::get < 1 > (poseCandidate);
//
//  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
//      MEMORYDEVICE_CPU);
//
//  PointsForLM ptsForLM(0);
//
//  for (int s = 0; s < samples.size(); ++s)
//  {
//    const int x = samples[s].first % m_featureImage->noDims.width;
//    const int y = samples[s].first / m_featureImage->noDims.width;
//    const int linearizedIdx = samples[s].first;
//
//    std::pair<std::vector<Eigen::VectorXd>, std::vector<PredictedGaussianMean *>> pt;
//
//    Eigen::VectorXf pixelLocalCoordinates = Eigen::Map<const Eigen::Vector4f>(
//        patchFeaturesData[linearizedIdx].position.v);
//
//    pt.first.push_back(pixelLocalCoordinates.cast<double>());
//    // Eigen::VectorXf  projectedPixel = candidateCameraPose * pixelLocalCoordinates;
//    Eigen::VectorXd projectedPixel = (candidateCameraPose
//        * pixelLocalCoordinates).cast<double>();
//
//    boost::shared_ptr<EnsemblePredictionGaussianMean> epgm =
//        m_featurePredictions[linearizedIdx];
//
//    int argmax = epgm->GetArgMax3D(projectedPixel, 0);
//    if (argmax == -1)
//      continue;
//    pt.second.push_back(epgm->_modes[argmax][0]);
//
//    if ((epgm->_modes[argmax][0]->_mean
//        - Helpers::ConvertWorldCoordinatesFromHomogeneousCoordinates(
//            projectedPixel)).norm() < 0.2)
//      ptsForLM.pts.push_back(pt);
//  }

  return false;
#endif
}

}