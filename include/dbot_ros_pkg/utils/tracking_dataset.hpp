/*************************************************************************
This software allows for filtering in high-dimensional observation and
state spaces, as described in

M. Wuthrich, P. Pastor, M. Kalakrishnan, J. Bohg, and S. Schaal.
Probabilistic Object Tracking using a Range Camera
IEEE/RSJ Intl Conf on Intelligent Robots and Systems, 2013

In a publication based on this software pleace cite the above reference.


Copyright (C) 2014  Manuel Wuthrich

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*************************************************************************/

#ifndef POSE_TRACKING_INTERFACE_UTILS_TRACKING_DATASET_HPP
#define POSE_TRACKING_INTERFACE_UTILS_TRACKING_DATASET_HPP

#include <Eigen/Dense>

#include <ros/ros.h>

#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/JointState.h>
#include <tf/tfMessage.h>

#include <message_filters/simple_filter.h>

#include <fstream>

#include <boost/filesystem.hpp>

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>

class DataFrame
{
public:
    sensor_msgs::Image::ConstPtr image_;
    sensor_msgs::CameraInfo::ConstPtr info_;
    sensor_msgs::JointState::ConstPtr ground_truth_joints_;
    sensor_msgs::JointState::ConstPtr noisy_joints_;
  tf::tfMessage::ConstPtr gt_tf_;
  tf::tfMessage::ConstPtr gt_tf_fixed_;
    Eigen::VectorXd ground_truth_;
    Eigen::VectorXd deviation_;

    DataFrame(const sensor_msgs::Image::ConstPtr& image,
              const sensor_msgs::CameraInfo::ConstPtr& info,
              const Eigen::VectorXd& ground_truth = Eigen::VectorXd(),
              const Eigen::VectorXd& deviation = Eigen::VectorXd());

  DataFrame(const sensor_msgs::Image::ConstPtr& image,
	    const sensor_msgs::CameraInfo::ConstPtr& info,
	    const sensor_msgs::JointState::ConstPtr& ground_truth_joints,
	    const sensor_msgs::JointState::ConstPtr& noisy_joints,
	    const Eigen::VectorXd& ground_truth = Eigen::VectorXd(),
	    const Eigen::VectorXd& deviation = Eigen::VectorXd());

  DataFrame(const sensor_msgs::Image::ConstPtr& image,
	    const sensor_msgs::CameraInfo::ConstPtr& info,
	    const sensor_msgs::JointState::ConstPtr& ground_truth_joints,
	    const sensor_msgs::JointState::ConstPtr& noisy_joints,
	    const tf::tfMessage::ConstPtr& tf,
	    const tf::tfMessage::ConstPtr& fixed_tf,
	    const Eigen::VectorXd& ground_truth = Eigen::VectorXd(),
	    const Eigen::VectorXd& deviation = Eigen::VectorXd());

};

template <class M>
class BagSubscriber : public message_filters::SimpleFilter<M>
{
public:
    void newMessage(const boost::shared_ptr<M const> &msg)
    {
      this->signalMessage(msg);
    }
};


class TrackingDataset
{
public:
  
  enum DataType {
    GROUND_TRUTH = 1,
    DEVIATION
  };

    TrackingDataset(const std::string& path);
    ~TrackingDataset();

    void AddFrame(const sensor_msgs::Image::ConstPtr& image,
                  const sensor_msgs::CameraInfo::ConstPtr& info,
                  const Eigen::VectorXd& ground_truth = Eigen::VectorXd(),
                  const Eigen::VectorXd& deviation = Eigen::VectorXd());

    void AddFrame(const sensor_msgs::Image::ConstPtr& image,
                  const sensor_msgs::CameraInfo::ConstPtr& info);

    sensor_msgs::Image::ConstPtr GetImage(const size_t& index);

    sensor_msgs::CameraInfo::ConstPtr GetInfo(const size_t& index);

    pcl::PointCloud<pcl::PointXYZ>::ConstPtr GetPointCloud(const size_t& index);

    Eigen::Matrix3d GetCameraMatrix(const size_t& index);

    Eigen::VectorXd GetGroundTruth(const size_t& index);

    size_t Size();

    void Load();

    void Store();

protected:

  bool LoadTextFile(const char *filename, DataType type);
  bool StoreTextFile(const char *filename, DataType type);
  
  std::vector<DataFrame> data_;
  const boost::filesystem::path path_;

  const std::string image_topic_;
  const std::string info_topic_;
  const std::string observations_filename_;
  const std::string ground_truth_filename_;

private:
  const double admissible_delta_time_; // admissible time difference in s for comparing time stamps
};

#endif
