#include "small_gicp_relocalization/small_gicp_relocalization.hpp"

#include <small_gicp/pcl/pcl_registration.hpp>
#include <small_gicp/util/downsampling_omp.hpp>

namespace small_gicp_relocalization
{

// 构造函数：节点初始化并设置参数
/*
params:
  num_threads:                              用于并行处理的线程数
  num_neighbors:                            估计协方差时的邻域点数
  globol_leaf_size && registered_leaf_size: 用于下采样的像素网格大小
  max_dist_sq:                              最大距离平方，用于配准时的排除条件
  map_frame_id && odom_frame_id:            全局地图坐标系和里程计坐标系的标识
  prior_pcd_file:                           指向先验点云地图的路径，用于加载全局地图
*/
SmallGicpRelocalizationNode::SmallGicpRelocalizationNode(const rclcpp::NodeOptions & options)
: Node("small_gicp_relocalization", options)
{
  this->declare_parameter("num_threads", 4);
  this->declare_parameter("num_neighbors", 20);
  this->declare_parameter("global_leaf_size", 0.25);
  this->declare_parameter("registered_leaf_size", 0.25);
  this->declare_parameter("max_dist_sq", 1.0);
  this->declare_parameter("map_frame_id", "map");
  this->declare_parameter("odom_frame_id", "odom");
  this->declare_parameter("prior_pcd_file", "");

  this->get_parameter("num_threads", num_threads_);
  this->get_parameter("num_neighbors", num_neighbors_);
  this->get_parameter("global_leaf_size", global_leaf_size_);
  this->get_parameter("registered_leaf_size", registered_leaf_size_);
  this->get_parameter("max_dist_sq", max_dist_sq_);
  this->get_parameter("map_frame_id", map_frame_id_);
  this->get_parameter("odom_frame_id", odom_frame_id_);
  this->get_parameter("prior_pcd_file", prior_pcd_file_);

  registered_scan_ = std::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  global_map_ = std::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  register_ = std::make_shared<
    small_gicp::Registration<small_gicp::GICPFactor, small_gicp::ParallelReductionOMP>>();

  tf_broadcaster_ = std::make_unique<tf2_ros::TransformBroadcaster>(this);

  loadGlobalMap(prior_pcd_file_);

  // Downsample points and convert them into pcl::PointCloud<pcl::PointCovariance>
  target_ = small_gicp::voxelgrid_sampling_omp<
    pcl::PointCloud<pcl::PointXYZ>, pcl::PointCloud<pcl::PointCovariance>>(
    *global_map_, global_leaf_size_);

  // Estimate covariances of points
  small_gicp::estimate_covariances_omp(*target_, num_neighbors_, num_threads_);

  // Create KdTree for target
  target_tree_ = std::make_shared<small_gicp::KdTree<pcl::PointCloud<pcl::PointCovariance>>>(
    target_, small_gicp::KdTreeBuilderOMP(num_threads_));

  pcd_sub_ = this->create_subscription<sensor_msgs::msg::PointCloud2>(
    "registered_scan", 10,
    std::bind(&SmallGicpRelocalizationNode::registeredPcdCallback, this, std::placeholders::_1));

  register_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(500),  // 2 Hz
    std::bind(&SmallGicpRelocalizationNode::performRegistration, this));

  transform_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(50),  // 20 Hz
    std::bind(&SmallGicpRelocalizationNode::publishTransform, this));
}

// loadGlobaoMap函数用于加载全局地图
// 它从PCD文件加载点云数据，将其存储在global_map_中，并通过控制台输出确认加载的点数
void SmallGicpRelocalizationNode::loadGlobalMap(const std::string & file_name)
{
  if (pcl::io::loadPCDFile<pcl::PointXYZ>(file_name, *global_map_) == -1) {
    RCLCPP_ERROR(this->get_logger(), "Couldn't read PCD file: %s", file_name.c_str());
    return;
  }
  RCLCPP_INFO(this->get_logger(), "Loaded global map with %zu points", global_map_->points.size());
}
 
void SmallGicpRelocalizationNode::registeredPcdCallback(
  const sensor_msgs::msg::PointCloud2::SharedPtr msg)
{
  last_scan_time_ = msg->header.stamp;

  pcl::fromROSMsg(*msg, *registered_scan_);

  // Downsample Registered points and convert them into pcl::PointCloud<pcl::PointCovariance>.
  source_ = small_gicp::voxelgrid_sampling_omp<
    pcl::PointCloud<pcl::PointXYZ>, pcl::PointCloud<pcl::PointCovariance>>(
    *registered_scan_, registered_leaf_size_);

  // Estimate point covariances
  small_gicp::estimate_covariances_omp(*source_, num_neighbors_, num_threads_);

  // Create KdTree for source.
  source_tree_ = std::make_shared<small_gicp::KdTree<pcl::PointCloud<pcl::PointCovariance>>>(
    source_, small_gicp::KdTreeBuilderOMP(num_threads_));
}


// performRegistration()函数用配准器将源点云与目标点云进行配准。步骤包括：
// ·配置线程数和排除距离。
// ·调用align函数对源点云和目标点云进行对齐配准，返回一个包含转换结果的result。
// ·若配准未收敛，会输出警告信息；若收敛，则保存转换矩阵result_T_。
void SmallGicpRelocalizationNode::performRegistration()
{
  if (!source_ || !source_tree_) {
    return;
  }

  register_->reduction.num_threads = num_threads_;
  register_->rejector.max_dist_sq = max_dist_sq_;

  // Align point clouds
  auto result = register_->align(*target_, *source_, *target_tree_, Eigen::Isometry3d::Identity());

  if (!result.converged) {
    RCLCPP_WARN(this->get_logger(), "GICP did not converge.");
    return;
  }

  result_T_ = result.T_target_source.matrix().cast<float>();
}


// publishTransform()函数将配准计算的转换结果发布为TF变换。步骤包括：
// ·设置transform_stamped消息的时间戳、坐标系。
// ·从转换矩阵result_T_中提取平移和旋转信息，并填入到transform_stamped中。
// ·使用tf_broadcaster_发布转换，以供其他节点使用。
void SmallGicpRelocalizationNode::publishTransform()
{
  if (result_T_.isZero()) {
    return;
  }

  geometry_msgs::msg::TransformStamped transform_stamped;
  transform_stamped.header.stamp = last_scan_time_;
  transform_stamped.header.frame_id = map_frame_id_;
  transform_stamped.child_frame_id = odom_frame_id_;

  const Eigen::Vector3f translation = result_T_.block<3, 1>(0, 3);
  const Eigen::Quaternionf rotation(result_T_.block<3, 3>(0, 0));

  transform_stamped.transform.translation.x = translation.x();
  transform_stamped.transform.translation.y = translation.y();
  transform_stamped.transform.translation.z = translation.z();
  transform_stamped.transform.rotation.x = rotation.x();
  transform_stamped.transform.rotation.y = rotation.y();
  transform_stamped.transform.rotation.z = rotation.z();
  transform_stamped.transform.rotation.w = rotation.w();

  tf_broadcaster_->sendTransform(transform_stamped);
}

}  // namespace small_gicp_relocalization

#include "rclcpp_components/register_node_macro.hpp"
RCLCPP_COMPONENTS_REGISTER_NODE(small_gicp_relocalization::SmallGicpRelocalizationNode)
