#include "pipeline.hpp"
#include "isaeslam/optimizers/AngularAdjustmentCERESAnalytic.h"

const Eigen::Vector3d Pipeline::llhToEcef(const Eigen::Vector3d &llh) {
    double Sp = std::sin(llh.x() * deg2rad);
    double Cp = std::cos(llh.x() * deg2rad);
    double Sl = std::sin(llh.y() * deg2rad);
    double Cl = std::cos(llh.y() * deg2rad);
    double N  = _a / std::sqrt(1 - _e2 * Sp * Sp);
    Eigen::Vector3d out;
    out.x() = (N + llh.z()) * Cp * Cl;
    out.y() = (N + llh.z()) * Cp * Sl;
    out.z() = (N * (1 - _e2) + llh.z()) * Sp;
    return out;
}

const Eigen::Vector3d Pipeline::ecefToENU(const Eigen::Vector3d &ecef) {
    Eigen::Vector3d dx = ecef - _ecef_ref;
    return _R_n_e * dx;
}

void Pipeline::setRef(const Eigen::Vector3d &llh_ref) {
    _llh_ref  = llh_ref;
    _ecef_ref = llhToEcef(_llh_ref);

    // Compute projection matrix PM_ used to project coordinates in LTP
    _R_n_e(0, 0) = -std::sin(_llh_ref.y() * deg2rad);
    _R_n_e(0, 1) = +std::cos(_llh_ref.y() * deg2rad);
    _R_n_e(0, 2) = 0.0f;

    _R_n_e(1, 0) = -std::sin(_llh_ref.x() * deg2rad) * std::cos(_llh_ref.y() * deg2rad);
    _R_n_e(1, 1) = -std::sin(_llh_ref.x() * deg2rad) * std::sin(_llh_ref.y() * deg2rad);
    _R_n_e(1, 2) = std::cos(_llh_ref.x() * deg2rad);

    _R_n_e(2, 0) = std::cos(_llh_ref.x() * deg2rad) * std::cos(_llh_ref.y() * deg2rad);
    _R_n_e(2, 1) = std::cos(_llh_ref.x() * deg2rad) * std::sin(_llh_ref.y() * deg2rad);
    _R_n_e(2, 2) = std::sin(_llh_ref.x() * deg2rad);
}

std::shared_ptr<NavFrame> Pipeline::next() {
    std::shared_ptr<NavFrame> nf = std::shared_ptr<NavFrame>(new NavFrame());

    while (_nf_queue.empty())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    nf = _nf_queue.front();
    _nf_queue.pop();

    return nf;
}

void Pipeline::init() {
    std::cout << "Pipeline init" << std::endl;

    // Init SLAM
    while (!_slam->_is_init) {
        _nf = next();
        _slam->_slam_param->getDataProvider()->addFrameToTheQueue(_nf->_frame);
    }

    // Wait for a frame with GPS
    while (_nf->_gnss_meas == nullptr) {
        _nf = next();

        // Set KF if GNSS meas
        if (_nf->_gnss_meas != nullptr)
            _nf->_frame->setKeyFrame();

        _slam->_slam_param->getDataProvider()->addFrameToTheQueue(_nf->_frame);
    }

    // Get the ouput from the SLAM
    std::shared_ptr<isae::Frame> frame_ready = _slam->_frame_to_display;
    while (frame_ready != _nf->_frame) {
        frame_ready = _slam->_frame_to_display;

        // If the SLAM is not initialized, reinitialize
        if (!_slam->_is_init) {
            _is_init = false;
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Save the pose in the SLAM frame
    _nf->_T_w_f = frame_ready->getFrame2WorldTransform();

    // Set the current frame as the reference frame if first init
    if (_llh_ref.isZero()) {
        setRef(_nf->_gnss_meas->llh_meas);
        Eigen::Affine3d T_n_a = Eigen::Affine3d::Identity();
        _nf->_T_n_f           = T_n_a * _T_a_f;
    }

    // add absolute pose contraint
    AbsolutePoseFactor af;
    af.T   = _nf->_T_n_f;
    af.nf  = _nf;
    af.inf = 100 * Eigen::MatrixXd::Identity(6, 6);
    _pg->_nf_abspose_map.emplace(_nf, af);

    // Add to the nav frame vector
    _nav_frames.push_back(_nf);

    // Calibrate the orientation

    // Add frames until a reasonable displacement is performed
    while (_nf->_T_n_f.translation().norm() < 3) {
        step();
    }

    // Then compute the yaw between ENU and W
    // and update the poses
    calibrateRotation4DoF();

    _is_init = true;
}

void Pipeline::step() {
    // Get the last frame in the queue
    _nf = next();
    _slam->_slam_param->getDataProvider()->addFrameToTheQueue(_nf->_frame);

    // Wait for a frame with GPS
    std::shared_ptr<isae::Frame> frame_ready;
    while (_nf->_gnss_meas == nullptr) {
        _nf = next();

        // Ignore if IMU only
        if (_nf->_frame->getSensors().size() == 0)
            continue;

        // Set KF if GNSS meas
        if (_nf->_gnss_meas != nullptr)
            _nf->_frame->setKeyFrame();

        // Send frame to the SLAM
        _slam->_slam_param->getDataProvider()->addFrameToTheQueue(_nf->_frame);

        // Get the ouput from the SLAM
        std::shared_ptr<isae::Frame> frame_ready = _slam->_frame_to_display;
        while (frame_ready != _nf->_frame) {
            frame_ready = _slam->_frame_to_display;

            // If the SLAM is not initialized, reinitialize
            if (!_slam->_is_init) {
                _is_init = false;
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        _nf->_T_w_f = frame_ready->getFrame2WorldTransform();

        // Compute the current pose
        Eigen::Affine3d T_n_flast = _nav_frames.back()->_T_n_f;
        Eigen::Affine3d T_flast_f = _nav_frames.back()->_T_w_f.inverse() * _nf->_T_w_f;
        _T_n_f                    = T_n_flast * T_flast_f;
        _nf->_T_n_f               = _T_n_f;
    }

    // Compute position in the local frame
    Eigen::Vector3d t_n_a            = ecefToENU(llhToEcef(_nf->_gnss_meas->llh_meas));
    Eigen::Affine3d T_n_f            = Eigen::Affine3d::Identity();
    T_n_f.translation()              = t_n_a + _T_a_f.translation();
    T_n_f.affine().block(0, 0, 3, 3) = _T_n_w.rotation() * _nf->_T_w_f.rotation();

    // Threshold on the covariance of the GNSS estimate
    if (_nf->_gnss_meas->cov.norm() < _thresh_cov) {

        // Use the pose of the GNSS when not initialized or VSLAM is not initialized
        if (!_is_init || !_slam->_is_init) {
            _nf->_T_n_f = T_n_f;
        }

        // add absolute position contraint
        AbsolutePositionFactor af;
        af.t = T_n_f.translation();
        if (_remove_z_estimate)
            af.t.z() = 0; //_nf->_T_w_f.translation().z(); // Set the altitude with the slam as the estimate tend to drift
        af.nf  = _nf;
        af.inf = Eigen::Matrix3d::Identity();
        if (_nf->_gnss_meas->cov.norm() > 1e-4) {
            af.inf << std::sqrt(1 / _nf->_gnss_meas->cov(0)), 0, 0, 0, std::sqrt(1 / _nf->_gnss_meas->cov(1)), 0, 0, 0,
                std::sqrt(1 / _nf->_gnss_meas->cov(2));
            // af.inf *= 0.1;
        } else {
            af.inf(2, 2) = 1;
        }

        // Put a low weight on the z axis if the estimate is removed
        if (_remove_z_estimate) {
            af.inf(2, 2) = 1;
        }

        _pg->_nf_absfact_map.emplace(_nf, af);
    }

    // add relative pose constraints if the VIO is initialized
    if (_slam->_is_init) {

        // If the system is init and the SLAM is init the frame is aligned
        if (_is_init) {
            _nf->_is_aligned = true;
            _nf->_T_n_w      = _T_n_w;
        }

        RelativePoseFactor rf;
        rf.nf_a             = _nav_frames.back();
        rf.nf_b             = _nf;
        rf.T_a_b            = _nav_frames.back()->_T_w_f.inverse() * _nf->_T_w_f;
        Eigen::MatrixXd cov = Eigen::MatrixXd::Identity(6, 6);
        _slam->_local_map_to_display->computeRelativePose(_nav_frames.back()->_frame, _nf->_frame, rf.T_a_b, cov);

        // Compute the information matrix
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> saes(cov);
        Eigen::VectorXd S =
            Eigen::VectorXd((saes.eigenvalues().array() > 1e-8).select(saes.eigenvalues().array().inverse(), 0));
        Eigen::VectorXd S_sqrt   = S.cwiseSqrt();
        Eigen::MatrixXd inf_sqrt = saes.eigenvectors() * S_sqrt.asDiagonal() * saes.eigenvectors().transpose();
        rf.inf                   = inf_sqrt.diagonal().asDiagonal();

        _pg->_nf_relfact_map.emplace(_nf, rf);
    } else // If the slam is not init, the system needs to re align
    {
        _is_init         = false;
        _nf->_is_aligned = false;
    }

    // Add to the nav frame vector
    _nav_frames.push_back(_nf);

    // Sliding window
    if (_pg->_nf_absfact_map.size() > _window_size) {
        // Marginalize
        _removed_frame_poses.push_back({_nav_frames.front()->_timestamp, _nav_frames.front()->_T_n_f});
        _removed_vo_poses.push_back(
            {_nav_frames.front()->_timestamp, _nav_frames.front()->_T_n_w * _nav_frames.front()->_T_w_f});
        _pg->marginalize(_nav_frames.front());
        _nav_frames.pop_front();
    }

    // Solve pg
    if (_is_init) {
        updateRelativeFactors();
        _pg->solveGraph();
        profiler();
    }
}

void Pipeline::run() {

    while (true) {

        if (!_is_init)
            init();
        else
            step();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Pipeline::calibrateRotation() {

    // Build the ceres problem
    ceres::Problem problem;
    ceres::LossFunction *loss_function = nullptr;

    double theta[1] = {0.0};
    problem.AddParameterBlock(theta, 1);

    // Add all constraints
    for (auto &nf : _nav_frames) {
        if (!nf->_is_aligned) {
            ceres::CostFunction *cost_fct =
                new OrientationCalib2D(nf->_T_w_f.translation(), nf->_T_n_f.translation(), Eigen::Matrix2d::Identity());

            problem.AddResidualBlock(cost_fct, loss_function, theta);
        }
    }

    // Solve the problem we just built
    ceres::Solver::Options options;
    options.trust_region_strategy_type         = ceres::LEVENBERG_MARQUARDT;
    options.linear_solver_type                 = ceres::SPARSE_NORMAL_CHOLESKY;
    options.max_num_iterations                 = 40;
    options.minimizer_progress_to_stdout       = false;
    options.use_explicit_schur_complement      = true;
    options.function_tolerance                 = 1e-3;
    options.sparse_linear_algebra_library_type = ceres::SUITE_SPARSE;
    options.num_threads                        = 4;
    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);

    std::cout << summary.FullReport() << std::endl;

    // Update the parameter and the poses
    _T_n_w = Eigen::Affine3d::Identity();
    _T_n_w.affine().block(0, 0, 3, 3) << std::cos(theta[0]), -std::sin(theta[0]), 0, std::sin(theta[0]),
        std::cos(theta[0]), 0, 0, 0, 1;

    for (auto &nf : _nav_frames) {
        if (!nf->_is_aligned) {
            nf->_T_n_f.affine().block(0, 0, 3, 3) = _T_n_w.rotation() * nf->_T_w_f.rotation();
            if (_pg->_nf_abspose_map.find(nf) != _pg->_nf_abspose_map.end())
                _pg->_nf_abspose_map.at(nf).T.affine().block(0, 0, 3, 3) = nf->_T_n_f.rotation();
            nf->_is_aligned = true;
            nf->_T_n_w      = _T_n_w;
        }
    }
}

void Pipeline::calibrateRotation4DoF() {

    // Build the ceres problem
    ceres::Problem problem;
    ceres::LossFunction *loss_function = nullptr;

    double theta[1] = {0.0};
    problem.AddParameterBlock(theta, 1);
    isae::PointXYZParametersBlock dt(Eigen::Vector3d::Zero());
    problem.AddParameterBlock(dt.values(), 3);

    // Add all constraints on non aligned frames
    for (auto &nf : _nav_frames) {
        if (!nf->_is_aligned) {
            ceres::CostFunction *cost_fct = new OrientationCalib4DoF(
                nf->_T_w_f.translation(), nf->_T_n_f.translation(), Eigen::Matrix3d::Identity());

            problem.AddResidualBlock(cost_fct, loss_function, theta, dt.values());
        }
    }

    // Solve the problem we just built
    ceres::Solver::Options options;
    options.trust_region_strategy_type         = ceres::LEVENBERG_MARQUARDT;
    options.linear_solver_type                 = ceres::SPARSE_NORMAL_CHOLESKY;
    options.max_num_iterations                 = 40;
    options.minimizer_progress_to_stdout       = false;
    options.use_explicit_schur_complement      = true;
    options.function_tolerance                 = 1e-3;
    options.sparse_linear_algebra_library_type = ceres::SUITE_SPARSE;
    options.num_threads                        = 4;
    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);

    std::cout << summary.FullReport() << std::endl;

    // Update the parameter and the poses
    _T_n_w = Eigen::Affine3d::Identity();
    _T_n_w.matrix() << std::cos(theta[0]), -std::sin(theta[0]), 0, dt.getPose().translation().x(), std::sin(theta[0]),
        std::cos(theta[0]), 0, dt.getPose().translation().y(), 0, 0, 1, dt.getPose().translation().z(), 0, 0, 0, 1;

    for (auto &nf : _nav_frames) {
        if (!nf->_is_aligned) {
            nf->_T_n_f = _T_n_w * nf->_T_w_f;
            if (_pg->_nf_abspose_map.find(nf) != _pg->_nf_abspose_map.end())
                _pg->_nf_abspose_map.at(nf).T.affine().block(0, 0, 3, 3) = nf->_T_n_f.rotation();
            nf->_is_aligned = true;
            nf->_T_n_w      = _T_n_w;
        }
    }

    std::cout << "Theta: " << theta[0] << std::endl;
    std::cout << "dt: " << dt.getPose().translation().transpose() << std::endl;
}

void Pipeline::updateRelativeFactors() {

    // Update current pose
    _nf->_T_w_f               = _nf->_frame->getFrame2WorldTransform();
    Eigen::Affine3d T_n_flast = _nav_frames.back()->_T_n_f;
    Eigen::Affine3d T_flast_f = _nav_frames.back()->_T_w_f.inverse() * _nf->_T_w_f;
    _T_n_f                    = T_n_flast * T_flast_f;

    // Parse every relative factor
    for (auto nf_relfact : _pg->_nf_relfact_map) {

        // Compute the updated delta pose
        Eigen::Affine3d T_a_b_updated = nf_relfact.second.nf_a->_frame->getWorld2FrameTransform() *
                                        nf_relfact.second.nf_b->_frame->getFrame2WorldTransform();

        // Update the factor
        nf_relfact.second.T_a_b = T_a_b_updated;

        // Update the pose
        nf_relfact.second.nf_a->_T_w_f = nf_relfact.second.nf_a->_frame->getFrame2WorldTransform();
        nf_relfact.second.nf_b->_T_w_f = nf_relfact.second.nf_b->_frame->getFrame2WorldTransform();
    }
}

void Pipeline::profiler() {

    if (!std::filesystem::is_directory("log_pg"))
        std::filesystem::create_directory("log_pg");

    // Clean the result file
    std::ofstream fw_res("log_pg/results.csv", std::ofstream::out | std::ofstream::trunc);
    fw_res << "timestamp (ns), T_wf(00), T_wf(01), T_wf(02), T_wf(03), T_wf(10), T_wf(11), T_wf(12), "
           << "T_wf(13), T_wf(20), T_wf(21), T_wf(22), T_wf(23)\n";

    for (auto &ts_pose : _removed_frame_poses) {
        Eigen::Affine3d T_n_f   = ts_pose.second;
        const Eigen::Matrix3d R = T_n_f.linear();
        Eigen::Vector3d tnf     = T_n_f.translation();
        fw_res << ts_pose.first << "," << R(0, 0) << "," << R(0, 1) << "," << R(0, 2) << "," << tnf.x() << ","
               << R(1, 0) << "," << R(1, 1) << "," << R(1, 2) << "," << tnf.y() << "," << R(2, 0) << "," << R(2, 1)
               << "," << R(2, 2) << "," << tnf.z() << "\n";
    }

    for (auto &nf : _nav_frames) {
        Eigen::Affine3d T_n_f   = nf->_T_n_f;
        const Eigen::Matrix3d R = T_n_f.linear();
        Eigen::Vector3d tnf     = T_n_f.translation();
        fw_res << nf->_timestamp << "," << R(0, 0) << "," << R(0, 1) << "," << R(0, 2) << "," << tnf.x() << ","
               << R(1, 0) << "," << R(1, 1) << "," << R(1, 2) << "," << tnf.y() << "," << R(2, 0) << "," << R(2, 1)
               << "," << R(2, 2) << "," << tnf.z() << "\n";
    }

    fw_res.close();
}