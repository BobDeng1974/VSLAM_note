#include <iostream>
#include <opencv2/core/core.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <g2o/core/base_vertex.h>
#include <g2o/core/base_unary_edge.h>
#include <g2o/core/block_solver.h>
#include <g2o/core/optimization_algorithm_levenberg.h>
#include <g2o/solvers/csparse/linear_solver_csparse.h>
#include <g2o/types/sba/types_six_dof_expmap.h>
#include <chrono>

using namespace std;
using namespace cv;

// 特征提取函数
void find_feature_matches(
    const Mat& img_1, const Mat& img_2,
    std::vector<KeyPoint>& keypoints_1,
    std::vector<KeyPoint>& keypoints_2,
    std::vector< DMatch >& matches);

// 像素坐标转相机归一化坐标函数
Point2d pixel2cam(const Point2d& p, const Mat& K);

// 光束法平差函数
void bundleAdjustment(
    const vector<Point3f> points_3d,
    const vector<Point2f> points_2d,
    const Mat& K,
    Mat& R, Mat& t
);

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        cout << "usage: pose_estimation_3d2d img1 img2 depth1" << endl;
        return 1;
    }

    //-- 读取图像
    Mat img_1 = imread(argv[1], CV_LOAD_IMAGE_COLOR);
    Mat img_2 = imread(argv[2], CV_LOAD_IMAGE_COLOR);

    //-- 特征提取
    vector<KeyPoint> keypoints_1, keypoints_2;
    vector<DMatch> matches;
    find_feature_matches(img_1, img_2, keypoints_1, keypoints_2, matches);
    cout << "一共找到了" << matches.size() << "组匹配点" << endl;

    // 建立3D点
    Mat d1 = imread(argv[3], CV_LOAD_IMAGE_UNCHANGED);  // 深度图为16位无符号数，单通道图像
    Mat K = (Mat_<double>(3, 3) << 520.9, 0, 325.1, 0, 521.0, 249.7, 0, 0, 1); // 相机内参
    vector<Point3f> pts_3d;
    vector<Point2f> pts_2d;
    for (DMatch m : matches)
    {
        ushort d = d1.ptr<unsigned short>(int(keypoints_1[m.queryIdx].pt.y))[int(keypoints_1[m.queryIdx].pt.x)];
        if (d == 0)   // bad depth
            continue;
        float dd = d / 5000.0;
        Point2d p1 = pixel2cam(keypoints_1[m.queryIdx].pt, K); // 像素坐标转相机归一化坐标
        pts_3d.push_back(Point3f(p1.x*dd, p1.y*dd, dd));       // 添加第一个图的特征位置 3D 点
        pts_2d.push_back(keypoints_2[m.trainIdx].pt);          // 添加第二个图的特征位置 2D 点
    }

    cout << "3d-2d pairs: " << pts_3d.size() << endl;

    // 调用OpenCV 的 PnP 求解，可选择EPNP，DLS等方法
    Mat r, t;
    solvePnP(pts_3d, pts_2d, K, Mat(), r, t, false);

    // r为旋转向量形式，用Rodrigues公式转换为矩阵
    Mat R;
    cv::Rodrigues(r, R);

    cout << "R=" << endl << R << endl;
    cout << "t=" << endl << t << endl;

    // 使用 BA 优化
    cout << "calling bundle adjustment" << endl;
    bundleAdjustment(pts_3d, pts_2d, K, R, t);
}

// 特征提取函数
void find_feature_matches(const Mat& img_1, const Mat& img_2,
    std::vector<KeyPoint>& keypoints_1,
    std::vector<KeyPoint>& keypoints_2,
    std::vector< DMatch >& matches)
{
    //-- 初始化
    Mat descriptors_1, descriptors_2;                    // 创建描述子
    Ptr<FeatureDetector> detector = ORB::create();       // 创建ORB特征点检测
    Ptr<DescriptorExtractor> descriptor = ORB::create(); // 使用ORB特征来描述特征点
    Ptr<DescriptorMatcher> matcher = DescriptorMatcher::create("BruteForce-Hamming"); // 设置特征匹配使用汉明距离计算

    //-- 第一步:检测 Oriented FAST 角点位置
    detector->detect(img_1, keypoints_1);
    detector->detect(img_2, keypoints_2);

    //-- 第二步:根据角点位置计算 BRIEF 描述子
    descriptor->compute(img_1, keypoints_1, descriptors_1);
    descriptor->compute(img_2, keypoints_2, descriptors_2);

    //-- 第三步:对两幅图像中的BRIEF描述子进行匹配，使用 Hamming 距离
    vector<DMatch> match;
    matcher->match(descriptors_1, descriptors_2, match);

    //-- 第四步:匹配点对筛选
    double min_dist = 10000, max_dist = 0;

    // 找出所有匹配之间的最小距离和最大距离, 即是最相似的和最不相似的两组点之间的距离
    for (int i = 0; i < descriptors_1.rows; i++)
    {
        double dist = match[i].distance;
        if (dist < min_dist) min_dist = dist;
        if (dist > max_dist) max_dist = dist;
    }
    // 显示所有匹配之间的最大距离和最小距离
    printf("-- Max dist : %f \n", max_dist);
    printf("-- Min dist : %f \n", min_dist);

    // 当描述子之间的距离大于两倍的最小距离时,即认为匹配有误
    // 但有时候最小距离会非常小,设置一个经验值30作为下限
    for (int i = 0; i < descriptors_1.rows; i++)
    {
        if (match[i].distance <= max(2 * min_dist, 30.0))
        {
            matches.push_back(match[i]);
        }
    }
}

// 像素坐标转相机归一化坐标函数
Point2d pixel2cam(const Point2d& p, const Mat& K)
{
    return Point2f
    (
        (p.x - K.at<double>(0, 2)) / K.at<double>(0, 0),
        (p.y - K.at<double>(1, 2)) / K.at<double>(1, 1)
    );
}

// 光束法平差函数
void bundleAdjustment(
    const vector< Point3f > points_3d,
    const vector< Point2f > points_2d,
    const Mat& K,
    Mat& R, Mat& t)
{
    // 构建图优化，先设定g2o
    typedef g2o::BlockSolver< g2o::BlockSolverTraits<6, 3> > Block;  // pose 维度为 6, landmark 维度为 3
    Block::LinearSolverType* linearSolver = new g2o::LinearSolverCSparse<Block::PoseMatrixType>(); // 线性方程求解器
    Block* solver_ptr = new Block(std::unique_ptr<Block::LinearSolverType>(linearSolver));         // 矩阵块求解器

    // 梯度下降方法，GN
    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(std::unique_ptr<Block>(solver_ptr));
    g2o::SparseOptimizer optimizer;   // 图模型
    optimizer.setAlgorithm(solver);   // 设置求解器
    optimizer.setVerbose(true);       // 打开调试输出

    // 增加顶点
    g2o::VertexSE3Expmap* pose = new g2o::VertexSE3Expmap(); // camera pose
    Eigen::Matrix3d R_mat;
    R_mat <<
        R.at<double>(0, 0), R.at<double>(0, 1), R.at<double>(0, 2),
        R.at<double>(1, 0), R.at<double>(1, 1), R.at<double>(1, 2),
        R.at<double>(2, 0), R.at<double>(2, 1), R.at<double>(2, 2);
    pose->setId(0);
    pose->setEstimate(g2o::SE3Quat(
        R_mat,
        Eigen::Vector3d(t.at<double>(0, 0), t.at<double>(1, 0), t.at<double>(2, 0))
    ));
    optimizer.addVertex(pose);

    int index = 1;
    for (const Point3f p : points_3d)   // landmarks
    {
        g2o::VertexSBAPointXYZ* point = new g2o::VertexSBAPointXYZ();
        point->setId(index++);
        point->setEstimate(Eigen::Vector3d(p.x, p.y, p.z));
        point->setMarginalized(true); // g2o 中必须设置 marg
        optimizer.addVertex(point);
    }

    // parameter: camera intrinsics
    g2o::CameraParameters* camera = new g2o::CameraParameters(
        K.at<double>(0, 0), Eigen::Vector2d(K.at<double>(0, 2), K.at<double>(1, 2)), 0
    );
    camera->setId(0);
    optimizer.addParameter(camera);


    // 增加边
    index = 1;
    for (const Point2f p : points_2d)
    {
        g2o::EdgeProjectXYZ2UV* edge = new g2o::EdgeProjectXYZ2UV();
        edge->setId(index);
        edge->setVertex(0, dynamic_cast<g2o::VertexSBAPointXYZ*> (optimizer.vertex(index)));
        edge->setVertex(1, pose);
        edge->setMeasurement(Eigen::Vector2d(p.x, p.y));
        edge->setParameterId(0, 0);
        edge->setInformation(Eigen::Matrix2d::Identity());
        optimizer.addEdge(edge);
        index++;
    }

    // 执行优化
    chrono::steady_clock::time_point t1 = chrono::steady_clock::now();
    optimizer.initializeOptimization();
    optimizer.optimize(100);
    chrono::steady_clock::time_point t2 = chrono::steady_clock::now();
    chrono::duration<double> time_used = chrono::duration_cast<chrono::duration<double>> (t2 - t1);
    cout << "optimization costs time: " << time_used.count() << " seconds." << endl;

    // 输出优化值
    cout << endl << "after optimization:" << endl;
    cout << "T=" << endl << Eigen::Isometry3d(pose->estimate()).matrix() << endl;
}
