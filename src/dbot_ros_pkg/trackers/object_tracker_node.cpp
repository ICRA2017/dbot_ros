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


#include <fstream>
#include <ctime>

#include <ros/ros.h>
#include <pcl_ros/point_cloud.h>

#include <boost/filesystem.hpp>

#include <fl/util/profiling.hpp>

#include <dbot_ros_pkg/trackers/object_tracker.hpp>
#include <dbot_ros_pkg/utils/tracking_dataset.hpp>
#include <dbot_ros_pkg/utils/pcl_interface.hpp>
#include <dbot_ros_pkg/utils/ros_interface.hpp>

#include <dbot/utils/helper_functions.hpp>
#include <dbot/utils/distribution_test.hpp>

typedef sensor_msgs::CameraInfo::ConstPtr CameraInfoPtr;
typedef Eigen::Matrix<double, -1, -1> Image;

class Tracker
{
public:
    Tracker(boost::shared_ptr<MultiObjectTracker> tracker): tracker_(tracker), node_handle_("~")
    {
        std::string config_file;
        ri::ReadParameter("config_file", config_file, node_handle_);

        path_ = config_file;
        path_ = path_.parent_path();
        std::cout << path_ << std::endl;

        time_t rawtime;
        struct tm * timeinfo;
        char buffer[80];

        time (&rawtime);
        timeinfo = localtime(&rawtime);

        strftime(buffer,80,"%d.%m.%Y_%I.%M.%S",timeinfo);
        std::string current_time(buffer);

        path_ /= "tracking_data_" + current_time + ".txt";
    }
    ~Tracker() {}

    void Filter(const sensor_msgs::Image& ros_image)
    {
        INIT_PROFILING
        fl::FreeFloatingRigidBodiesState<-1> mean_state = tracker_->Filter(ros_image);
        MEASURE("total time for filtering")
    }

    void FilterAndStore(const sensor_msgs::Image& ros_image)
    {
        INIT_PROFILING
        fl::FreeFloatingRigidBodiesState<-1> mean_state = tracker_->Filter(ros_image);
        MEASURE("total time for filtering")

        std::ofstream file;
        file.open(path_.c_str(), std::ios::out | std::ios::app);
        if(file.is_open())
        {
            file << ros_image.header.stamp << " ";
            file << mean_state.poses().transpose() << std::endl;
            file.close();
        }
        else
        {
            std::cout << "could not open file " << path_ << std::endl;
            exit(-1);
        }
    }

private:
    boost::shared_ptr<MultiObjectTracker> tracker_;
    ros::NodeHandle node_handle_;
    boost::filesystem::path path_;
};

int main (int argc, char **argv)
{
    ros::init(argc, argv, "test_filter");
    ros::NodeHandle node_handle("~");

    // read parameters
    std::cout << "reading parameters" << std::endl;
    std::string depth_image_topic; ri::ReadParameter("depth_image_topic", depth_image_topic, node_handle);
    std::string camera_info_topic; ri::ReadParameter("camera_info_topic", camera_info_topic, node_handle);
    double min_delta_time; ri::ReadParameter("min_delta_time", min_delta_time, node_handle);
    std::string source; ri::ReadParameter("source", source, node_handle);
    std::vector<std::string> object_names; ri::ReadParameter("object_names", object_names, node_handle);

    int initial_sample_count; ri::ReadParameter("initial_sample_count", initial_sample_count, node_handle);

    // read from camera
    if(source == "camera")
    {
        std::cout << "reading data from camera " << std::endl;
        Eigen::Matrix3d camera_matrix = ri::GetCameraMatrix<double>(camera_info_topic, node_handle, 2.0);

        // get observations from camera
        sensor_msgs::Image::ConstPtr ros_image =
                ros::topic::waitForMessage<sensor_msgs::Image>(depth_image_topic, node_handle, ros::Duration(10.0));
        Image image = ri::Ros2Eigen<double>(*ros_image);

        std::vector<Eigen::VectorXd>
                initial_states = pi::SampleTableClusters(
                    fl::hf::Image2Points(image, camera_matrix),
                    initial_sample_count);

        // intialize the filter
        boost::shared_ptr<MultiObjectTracker> tracker(new MultiObjectTracker);
        tracker->Initialize(initial_states, *ros_image, camera_matrix);
        std::cout << "done initializing" << std::endl;
        Tracker interface(tracker);

        ros::Subscriber subscriber = node_handle.subscribe(depth_image_topic, 1, &Tracker::FilterAndStore, &interface);
        ros::spin();
    }
    // read from bagfile
    else
    {
        TrackingDataset TrackingDataset(source);
        std::cout << "laoding bagfile " << std::endl;
        TrackingDataset.Load();
        std::cout << "done" << std::endl;

        std::cout << "setting initial state " << std::endl;
        std::cout << TrackingDataset.GetGroundTruth(0).transpose() << std::endl;
        std::cout << "done printing vector " << std::endl;
        fl::FreeFloatingRigidBodiesState<-1> initial_state(object_names.size());
        initial_state.poses(TrackingDataset.GetGroundTruth(0).topRows(object_names.size()*6)); // we read only the part of the state we need
        std::vector<Eigen::VectorXd> initial_states(1, initial_state);

        std::cout << "initializing filter " << std::endl;
        // intialize the filter
        boost::shared_ptr<MultiObjectTracker> tracker(new MultiObjectTracker);
        tracker->Initialize(initial_states, *TrackingDataset.GetImage(0), TrackingDataset.GetCameraMatrix(0), false);
        Tracker interface(tracker);

        ros::Publisher image_publisher = node_handle.advertise<sensor_msgs::Image>("/bagfile/depth/image", 0);
        ros::Publisher cloud_publisher = node_handle.advertise<pcl::PointCloud<pcl::PointXYZ> > ("/bagfile/depth/points", 0);

        std::cout << "processing TrackingDataset of Size: " << TrackingDataset.Size() << std::endl;
        for(size_t i = 0; i < TrackingDataset.Size() && ros::ok(); i++)
        {
            INIT_PROFILING
            double start_time; GET_TIME(start_time);

            interface.FilterAndStore(*TrackingDataset.GetImage(i));
            image_publisher.publish(*TrackingDataset.GetImage(i));
            cloud_publisher.publish((*TrackingDataset.GetPointCloud(i)).makeShared());

            double end_time; GET_TIME(end_time);
            while(end_time - start_time < min_delta_time)
                GET_TIME(end_time);
            MEASURE("========================================================>>>>>>>>> ");


            std::cout << "time for frame " << i << ": " << end_time - start_time << std::endl;
        }
        std::cout << std::endl << "done processing TrackingDataset" << std::endl;
    }

    return 0;
}
