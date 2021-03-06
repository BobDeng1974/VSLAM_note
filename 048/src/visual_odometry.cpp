#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <algorithm>
#include <boost/timer.hpp>

#include "myslam/config.h"
#include "myslam/visual_odometry.h"
#include "myslam/g2o_types.h"

namespace myslam
{
    VisualOdometry::VisualOdometry() :
        state_(INITIALIZING), ref_(nullptr), curr_(nullptr), map_(new Map), num_lost_(0), num_inliers_(0), matcher_flann_(new cv::flann::LshIndexParams(5, 10, 2))
    {
        num_of_features_ = Config::get<int>("number_of_features");
        scale_factor_ = Config::get<double>("scale_factor");
        level_pyramid_ = Config::get<int>("level_pyramid");
        match_ratio_ = Config::get<float>("match_ratio");
        max_num_lost_ = Config::get<float>("max_num_lost");
        min_inliers_ = Config::get<int>("min_inliers");
        key_frame_min_rot = Config::get<double>("keyframe_rotation");
        key_frame_min_trans = Config::get<double>("keyframe_translation");
        map_point_erase_ratio_ = Config::get<double>("map_point_erase_ratio");
        orb_ = cv::ORB::create(num_of_features_, scale_factor_, level_pyramid_);
    }

    VisualOdometry::~VisualOdometry()
    {

    }

    // 添加帧
    bool VisualOdometry::addFrame(Frame::Ptr frame)
    {
        switch (state_)
        {
        case INITIALIZING:
        {
            state_ = OK;
            curr_ = ref_ = frame;
            // 从第一帧中提取特征并加入地图中
            extractKeyPoints();
            computeDescriptors();
            addKeyFrame();        // 第一帧为关键帧
            break;
        }
        case OK:
        {
            curr_ = frame;
            curr_->T_c_w_ = ref_->T_c_w_;
            extractKeyPoints();
            computeDescriptors();
            featureMatching();
            poseEstimationPnP();
            if (checkEstimatedPose() == true) // 一个好的评估?
            {
                curr_->T_c_w_ = T_c_w_estimated_;
                optimizeMap();
                num_lost_ = 0;
                if (checkKeyFrame() == true) // 关键帧？
                {
                    addKeyFrame();
                }
            }
            else // 由于种种原因造成的估计错误
            {
                num_lost_++;
                if (num_lost_ > max_num_lost_)
                {
                    state_ = LOST;
                }
                return false;
            }
            break;
        }
        case LOST:
        {
            cout << "vo has lost." << endl;
            break;
        }
        }

        return true;
    }

    // 提取关键点
    void VisualOdometry::extractKeyPoints()
    {
        boost::timer timer;
        orb_->detect(curr_->color_, keypoints_curr_);
        cout << "extract keypoints cost time: " << timer.elapsed() << endl;
    }

    // 计算描述子
    void VisualOdometry::computeDescriptors()
    {
        boost::timer timer;
        orb_->compute(curr_->color_, keypoints_curr_, descriptors_curr_);
        cout << "descriptor computation cost time: " << timer.elapsed() << endl;
    }

    // 特征匹配
    void VisualOdometry::featureMatching()
    {
        boost::timer timer;
        vector<cv::DMatch> matches;
        // 在map中选择候选项
        Mat desp_map;
        vector<MapPoint::Ptr> candidate;
        for (auto& allpoints : map_->map_points_)
        {
            MapPoint::Ptr& p = allpoints.second;
            // 检查p是否在当前帧图像中
            if (curr_->isInFrame(p->pos_))
            {
                // 添加到候选
                p->visible_times_++;
                candidate.push_back(p);
                desp_map.push_back(p->descriptor_);
            }
        }

        matcher_flann_.match(desp_map, descriptors_curr_, matches);
        // 选择最佳匹配
        float min_dis = std::min_element(
            matches.begin(), matches.end(),
            [](const cv::DMatch& m1, const cv::DMatch& m2)
        {
            return m1.distance < m2.distance;
        })->distance;

        match_3dpts_.clear();
        match_2dkp_index_.clear();
        for (cv::DMatch& m : matches)
        {
            if (m.distance < max<float>(min_dis*match_ratio_, 30.0))
            {
                match_3dpts_.push_back(candidate[m.queryIdx]);
                match_2dkp_index_.push_back(m.trainIdx);
            }
        }
        cout << "good matches: " << match_3dpts_.size() << endl;
        cout << "match cost time: " << timer.elapsed() << endl;
    }

    // 姿态估计
    void VisualOdometry::poseEstimationPnP()
    {
        // 构建3d、2d观测
        vector<cv::Point3f> pts3d;
        vector<cv::Point2f> pts2d;

        for (int index : match_2dkp_index_)
        {
            pts2d.push_back(keypoints_curr_[index].pt);
        }
        for (MapPoint::Ptr pt : match_3dpts_)
        {
            pts3d.push_back(pt->getPositionCV());
        }

        Mat K = (cv::Mat_<double>(3, 3) <<
            ref_->camera_->fx_, 0, ref_->camera_->cx_,
            0, ref_->camera_->fy_, ref_->camera_->cy_,
            0, 0, 1
            );
        Mat rvec, tvec, inliers;
        cv::solvePnPRansac(pts3d, pts2d, K, Mat(), rvec, tvec, false, 100, 4.0, 0.99, inliers);
        num_inliers_ = inliers.rows;
        cout << "pnp inliers: " << num_inliers_ << endl;
        T_c_w_estimated_ = SE3(
            SO3(rvec.at<double>(0, 0), rvec.at<double>(1, 0), rvec.at<double>(2, 0)),
            Vector3d(tvec.at<double>(0, 0), tvec.at<double>(1, 0), tvec.at<double>(2, 0))
        );

        // 优化姿态
        typedef g2o::BlockSolver<g2o::BlockSolverTraits<6, 2>> Block;
        // 线性方程求解器
        Block::LinearSolverType* linearSolver = new g2o::LinearSolverDense<Block::PoseMatrixType>();
        // 矩阵块求解器
        Block* solver_ptr = new Block(std::unique_ptr<Block::LinearSolverType>(linearSolver));
        // 梯度下降方法
        g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(std::unique_ptr<Block>(solver_ptr));
        g2o::SparseOptimizer optimizer;
        optimizer.setAlgorithm(solver);

        g2o::VertexSE3Expmap* pose = new g2o::VertexSE3Expmap();
        pose->setId(0);
        pose->setEstimate(g2o::SE3Quat(
            T_c_w_estimated_.rotation_matrix(), T_c_w_estimated_.translation()
        ));
        optimizer.addVertex(pose);

        // edges
        for (int i = 0; i < inliers.rows; i++)
        {
            int index = inliers.at<int>(i, 0);
            // 3D -> 2D 投影
            EdgeProjectXYZ2UVPoseOnly* edge = new EdgeProjectXYZ2UVPoseOnly();
            edge->setId(i);
            edge->setVertex(0, pose);
            edge->camera_ = curr_->camera_.get();
            edge->point_ = Vector3d(pts3d[index].x, pts3d[index].y, pts3d[index].z);
            edge->setMeasurement(Vector2d(pts2d[index].x, pts2d[index].y));
            edge->setInformation(Eigen::Matrix2d::Identity());
            optimizer.addEdge(edge);
            // 设置输入地图点
            match_3dpts_[index]->matched_times_++;
        }

        optimizer.initializeOptimization();
        optimizer.optimize(10);

        T_c_w_estimated_ = SE3(
            pose->estimate().rotation(),
            pose->estimate().translation()
        );

        cout << "T_c_w_estimated_: " << endl << T_c_w_estimated_.matrix() << endl;
    }

    // 检查估计姿势
    bool VisualOdometry::checkEstimatedPose()
    {
        // 检查预估姿势是否正确
        if (num_inliers_ < min_inliers_)
        {
            cout << "reject because inlier is too small: " << num_inliers_ << endl;
            return false;
        }
        // 如果运动太大，它可能是错误的
        SE3 T_r_c = ref_->T_c_w_ * T_c_w_estimated_.inverse();
        Sophus::Vector6d d = T_r_c.log();
        if (d.norm() > 5.0)
        {
            cout << "reject because motion is too large: " << d.norm() << endl;
            return false;
        }
        return true;
    }
    // 检查关键帧
    bool VisualOdometry::checkKeyFrame()
    {
        SE3 T_r_c = ref_->T_c_w_ * T_c_w_estimated_.inverse();
        Sophus::Vector6d d = T_r_c.log();
        Vector3d trans = d.head<3>();
        Vector3d rot = d.tail<3>();
        if (rot.norm() > key_frame_min_rot || trans.norm() > key_frame_min_trans)
            return true;
        return false;
    }

    // 添加关键帧
    void VisualOdometry::addKeyFrame()
    {
        if (map_->keyframes_.empty())
        {
            // 第一个关键帧，添加所有3d点到地图
            for (size_t i = 0; i < keypoints_curr_.size(); i++)
            {
                double d = curr_->findDepth(keypoints_curr_[i]);
                if (d < 0)
                    continue;
                Vector3d p_world = ref_->camera_->pixel2world(
                    Vector2d(keypoints_curr_[i].pt.x, keypoints_curr_[i].pt.y), curr_->T_c_w_, d
                );
                Vector3d n = p_world - ref_->getCamCenter();
                n.normalize();
                MapPoint::Ptr map_point = MapPoint::createMapPoint(
                    p_world, n, descriptors_curr_.row(i).clone(), curr_.get()
                );
                map_->insertMapPoint(map_point);
            }
        }

        map_->insertKeyFrame(curr_);
        ref_ = curr_;
    }

    // 添加地图点
    void VisualOdometry::addMapPoints()
    {
        // 将新的地图点添加到地图中
        vector<bool> matched(keypoints_curr_.size(), false);
        for (int index : match_2dkp_index_)
            matched[index] = true;
        for (int i = 0; i < keypoints_curr_.size(); i++)
        {
            if (matched[i] == true)
                continue;
            double d = ref_->findDepth(keypoints_curr_[i]);
            if (d < 0)
                continue;
            Vector3d p_world = ref_->camera_->pixel2world(
                Vector2d(keypoints_curr_[i].pt.x, keypoints_curr_[i].pt.y),
                curr_->T_c_w_, d
            );
            Vector3d n = p_world - ref_->getCamCenter();
            n.normalize();
            MapPoint::Ptr map_point = MapPoint::createMapPoint(
                p_world, n, descriptors_curr_.row(i).clone(), curr_.get()
            );
            map_->insertMapPoint(map_point);
        }
    }

    // 优化地图
    void VisualOdometry::optimizeMap()
    {
        // 除去几乎看不见的和不可见的点
        for (auto iter = map_->map_points_.begin(); iter != map_->map_points_.end(); )
        {
            if (!curr_->isInFrame(iter->second->pos_))
            {
                iter = map_->map_points_.erase(iter);
                continue;
            }
            float match_ratio = float(iter->second->matched_times_) / iter->second->visible_times_;
            if (match_ratio < map_point_erase_ratio_)
            {
                iter = map_->map_points_.erase(iter);
                continue;
            }

            double angle = getViewAngle(curr_, iter->second);
            if (angle > M_PI / 6.)
            {
                iter = map_->map_points_.erase(iter);
                continue;
            }
            if (iter->second->good_ == false)
            {
                // 试着对这个地图点进行三角测量
            }
            iter++;
        }

        if (match_2dkp_index_.size() < 100)
            addMapPoints();
        if (map_->map_points_.size() > 1000)
        {
            // 地图太大，删除一些
            map_point_erase_ratio_ += 0.05;
        }
        else
            map_point_erase_ratio_ = 0.1;
        cout << "map points: " << map_->map_points_.size() << endl;
    }

    // 获取视角
    double VisualOdometry::getViewAngle(Frame::Ptr frame, MapPoint::Ptr point)
    {
        Vector3d n = point->pos_ - frame->getCamCenter();
        n.normalize();
        return acos(n.transpose()*point->norm_);
    }
}
