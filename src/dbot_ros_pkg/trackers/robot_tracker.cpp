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

#include <memory>

#include <sensor_msgs/PointCloud2.h>

#include <fl/util/profiling.hpp>

#include <dbot_ros_pkg/trackers/robot_tracker.hpp>
#include <dbot_ros_pkg/utils/image_visualizer.hpp>
#include <dbot_ros_pkg/utils/cloud_visualizer.hpp>
#include <dbot_ros_pkg/utils/ros_interface.hpp>

#include <dbot/utils/helper_functions.hpp>

#include <ros/package.h>



using namespace std;

RobotTracker::RobotTracker():
    node_handle_("~"),
    tf_prefix_("MEAN"),
    last_measurement_time_(std::numeric_limits<Scalar>::quiet_NaN())
{
    ri::ReadParameter("downsampling_factor", downsampling_factor_, node_handle_);
    ri::ReadParameter("evaluation_count", evaluation_count_, node_handle_);
    ri::ReadParameter("camera_frame", camera_frame_, node_handle_);
    ri::ReadParameter("data_in_meters", data_in_meters_, node_handle_);

pub_point_cloud_ = boost::shared_ptr<ros::Publisher>(new ros::Publisher());
*pub_point_cloud_ = node_handle_.advertise<sensor_msgs::PointCloud2> ("/XTION/depth/points", 5);


boost::shared_ptr<image_transport::ImageTransport> it(new image_transport::ImageTransport(node_handle_));
pub_rgb_image_ = it->advertise ("/XTION/depth/image_color", 5);

}

void RobotTracker::Initialize(std::vector<Eigen::VectorXd> initial_samples_eigen,
                const sensor_msgs::Image& ros_image,
                Eigen::Matrix3d camera_matrix,
                boost::shared_ptr<KinematicsFromURDF> &urdf_kinematics)
{
    boost::mutex::scoped_lock lock(mutex_);

    urdf_kinematics_ = urdf_kinematics;

    // convert initial samples to our state format
    std::vector<State> initial_samples(initial_samples_eigen.size());
    for(size_t i = 0; i < initial_samples.size(); i++)
        initial_samples[i] = initial_samples_eigen[i];

    // convert camera matrix and image to desired format
    camera_matrix.topLeftCorner(2,3) /= double(downsampling_factor_);
    camera_matrix_ = camera_matrix;
    Observation image = ri::Ros2Eigen<double>(ros_image, downsampling_factor_);
    if(!data_in_meters_) {
      //ROS_INFO("Converting to meters");
      image = image/1000.; // convert to meters
    }

    // read some parameters ---------------------------------------------------------------------------------------------------------
    bool use_gpu; ri::ReadParameter("use_gpu", use_gpu, node_handle_);
    int max_sample_count; ri::ReadParameter("max_sample_count", max_sample_count, node_handle_);
    double initial_occlusion_prob; ri::ReadParameter("initial_occlusion_prob", initial_occlusion_prob, node_handle_);
    double p_occluded_visible; ri::ReadParameter("p_occluded_visible", p_occluded_visible, node_handle_);
    double p_occluded_occluded; ri::ReadParameter("p_occluded_occluded", p_occluded_occluded, node_handle_);
    double joint_angle_sigma; ri::ReadParameter("joint_angle_sigma", joint_angle_sigma, node_handle_);
    double damping; ri::ReadParameter("damping", damping, node_handle_);
    double tail_weight; ri::ReadParameter("tail_weight", tail_weight, node_handle_);
    double model_sigma; ri::ReadParameter("model_sigma", model_sigma, node_handle_);
    double sigma_factor; ri::ReadParameter("sigma_factor", sigma_factor, node_handle_);
    std::vector<std::vector<size_t> > sampling_blocks;
    ri::ReadParameter("sampling_blocks", sampling_blocks, node_handle_);
    std::vector<double> joint_sigmas;
    node_handle_.getParam("joint_sigmas", joint_sigmas);
    double max_kl_divergence; ri::ReadParameter("max_kl_divergence", max_kl_divergence, node_handle_);


    // initialize observation model =================================================================================================
    // Read the URDF for the specific robot and get part meshes
    std::vector<boost::shared_ptr<PartMeshModel> > part_meshes_;
    urdf_kinematics->GetPartMeshes(part_meshes_);
    ROS_INFO("Number of part meshes %d", (int)part_meshes_.size());
    ROS_INFO("Number of links %d", urdf_kinematics->num_links());
    ROS_INFO("Number of joints %d", urdf_kinematics->num_joints());

    std::vector<std::string> joints = urdf_kinematics->GetJointMap();
    fl::hf::PrintVector(joints);


    // get the name of the root frame
    root_ = urdf_kinematics->GetRootFrameID();

    // initialize the robot state publisher
    robot_state_publisher_ = boost::shared_ptr<robot_state_pub::RobotStatePublisher>
    (new robot_state_pub::RobotStatePublisher(urdf_kinematics->GetTree()));

    std::vector<std::vector<Eigen::Vector3d> > part_vertices(part_meshes_.size());
    std::vector<std::vector<std::vector<int> > > part_triangle_indices(part_meshes_.size());
    int n_triangles = 0;
    for(size_t i = 0; i < part_meshes_.size(); i++)
    {
        part_vertices[i] = *(part_meshes_[i]->get_vertices());
        part_triangle_indices[i] = *(part_meshes_[i]->get_indices());
	n_triangles +=part_triangle_indices[i].size();
    }

    std::cout << "Total number of triangles " << n_triangles << std::endl;


    cout << "setting kinematics " << endl;
   State::kinematics_ = urdf_kinematics;\
   cout << "done setting kinematics " << endl;

    boost::shared_ptr<fl::RigidBodiesState<> > robot_state(new State(State::Zero(urdf_kinematics->num_joints())));
    dimension_ = urdf_kinematics->num_joints();

    // initialize the result container for the emperical mean
    mean_ = boost::shared_ptr<State > (new State);

    robot_renderer_ = boost::shared_ptr<fl::RigidBodyRenderer>(
                new fl::RigidBodyRenderer(part_vertices,
                                               part_triangle_indices,
                                               robot_state));

    // FOR DEBUGGING
    std::cout << "Image rows and cols " << image.rows() << " " << image.cols() << std::endl;

    robot_renderer_->state(initial_samples[0]);
    std::vector<int> indices;
    std::vector<float> depth;
    robot_renderer_->Render(camera_matrix,
            image.rows(),
            image.cols(),
            indices,
            depth);
//    vis::ImageVisualizer image_viz(image.rows(),image.cols());
//    image_viz.set_image(image);
//    image_viz.add_points(indices, depth);
//    image_viz.show_image("enchilada ");

    /*
    std::vector<std::vector<Eigen::Vector3d> > vertices = robot_renderer_->vertices();
    vis::CloudVisualizer cloud_vis;
    std::vector<std::vector<Eigen::Vector3d> >::iterator it = vertices.begin();
    for(; it!=vertices.end();++it){
        if(!it->empty())
            cloud_vis.add_cloud(*it);
    }

    cloud_vis.show();
    */

    std::shared_ptr<ObservationModel> observation_model;

    if(!use_gpu)
    {
        // cpu obseration model
        boost::shared_ptr<fl::KinectPixelObservationModel> kinect_pixel_observation_model(
                    new fl::KinectPixelObservationModel(tail_weight, model_sigma, sigma_factor));
        boost::shared_ptr<fl::OcclusionProcessModel> occlusion_process_model(
                    new fl::OcclusionProcessModel(p_occluded_visible, p_occluded_occluded));
        observation_model = std::shared_ptr<ObservationModelCPUType>(
                    new ObservationModelCPUType(camera_matrix,
                                                image.rows(),
                                                image.cols(),
                                                initial_samples.size(),
                                                robot_renderer_,
                                                kinect_pixel_observation_model,
                                                occlusion_process_model,
                                                initial_occlusion_prob));
    }
    else
    {
#ifdef BUILD_GPU
        // gpu obseration model
        std::shared_ptr<ObservationModelGPUType>
                gpu_observation_model(new ObservationModelGPUType(
                                          camera_matrix,
                                          image.rows(),
                                          image.cols(),
                                          max_sample_count,
                                          initial_occlusion_prob));




        std::string vertex_shader_path =
                ros::package::getPath("state_filtering")
                + "/src/dbot/models/observation_models/"
                + "kinect_image_observation_model_gpu/shaders/"
                + "VertexShader.vertexshader";

        std::string fragment_shader_path =
                ros::package::getPath("state_filtering")
                + "/src/dbot/models/observation_models/"
                + "kinect_image_observation_model_gpu/shaders/"
                + "FragmentShader.fragmentshader";

        if(!boost::filesystem::exists(fragment_shader_path))
        {
            std::cout << "vertex shader does not exist at: "
                 << vertex_shader_path << std::endl;
            exit(-1);
        }
        if(!boost::filesystem::exists(vertex_shader_path))
        {
            std::cout << "fragment_shader does not exist at: "
                 << fragment_shader_path << std::endl;
            exit(-1);
        }


        gpu_observation_model->Constants(part_vertices,
                                         part_triangle_indices,
                                         p_occluded_visible,
                                         p_occluded_occluded,
                                         tail_weight,
                                         model_sigma,
                                         sigma_factor,
                                         6.0f,         // max_depth
                                         -log(0.5),
                                         vertex_shader_path,
                                         fragment_shader_path);   // exponential_rate

        gpu_observation_model->Initialize(*urdf_kinematics);
        observation_model = gpu_observation_model;
#endif
    }



    // initialize process model =====================================================================================================
    if(dimension_ != joint_sigmas.size())
    {
        std::cout << "the dimension of the joint sigmas is " << joint_sigmas.size()
             << " while the state dimension is " << dimension_ << std::endl;
        exit(-1);
    }
    std::shared_ptr<ProcessModel> process(new ProcessModel(dimension_));
    Eigen::MatrixXd joint_covariance = Eigen::MatrixXd::Zero(dimension_, dimension_);
    for(size_t i = 0; i < dimension_; i++)
        joint_covariance(i, i) = pow(joint_sigmas[i], 2);
    process->parameters(damping, joint_covariance);

    // initialize coordinate_filter =================================================================================================
    filter_ = boost::shared_ptr<FilterType>(
                new FilterType(process, observation_model, sampling_blocks, max_kl_divergence));

    // we evaluate the initial particles and resample -------------------------------------------------------------------------------
    std::cout << "evaluating initial particles cpu ..." << std::endl;
    filter_->Samples(initial_samples);
    filter_->Filter(image, 0.0, Input::Zero(dimension_));
    filter_->Resample(evaluation_count_/sampling_blocks.size());
}

void RobotTracker::Filter(const sensor_msgs::Image& ros_image)
{
    INIT_PROFILING;
    boost::mutex::scoped_lock lock(mutex_);

    double delta_time;
    if(std::isnan(last_measurement_time_))
        delta_time = 0;
    else
        delta_time = ros_image.header.stamp.toSec() - last_measurement_time_;
    last_measurement_time_ = ros_image.header.stamp.toSec();

    if(delta_time > 0.04)
    {
        std::cout << "delta_time: " << delta_time;
        std::cout << "!!!!!!!!!!!!!!!!!!!!!!! SKIPPED FRAME!!!!!!! !!!!!!!!!" << std::endl;
        std::cout << "last_measurement_time_: " << last_measurement_time_ << std::endl;
        std::cout << "ros_image.header.stamp.toSec(): " << ros_image.header.stamp.toSec() << std::endl;


    }

    // convert image
    Observation image = ri::Ros2Eigen<Scalar>(ros_image, downsampling_factor_);
    if(!data_in_meters_)
      image = image/1000.; // convert to meters
      

    // filter
    {
    INIT_PROFILING;
    filter_->Filter(image, delta_time, Eigen::VectorXd::Zero(dimension_));
    MEASURE("-----------------> total time for filtering");
    }

    // get the mean estimation for the robot joints
    // Casting is a disgusting hack to make sure that the correct equal-operator is used
    // TODO: Make this right
    *mean_ = (Eigen::VectorXd)(filter_->state_distribution().mean());

    // DEBUG to see depth images
    robot_renderer_->state(*mean_);
        std::vector<int> indices;
        std::vector<float> depth;
        robot_renderer_->Render(camera_matrix_,
                image.rows(),
                image.cols(),
                indices,
                depth);
    //image_viz_ = boost::shared_ptr<vis::ImageVisualizer>(new vis::ImageVisualizer(image.rows(),image.cols()));
        vis::ImageVisualizer image_viz(image.rows(),image.cols());
        image_viz.set_image(image);
        image_viz.add_points(indices, depth);
    //image_viz.show_image("enchilada ", 500, 500, 1.0);
    //////

    std::map<std::string, double> joint_positions;
    mean_->GetJointState(joint_positions);


    ros::Time t = ros::Time::now();
    // publish movable joints
    robot_state_publisher_->publishTransforms(joint_positions,  t, tf_prefix_);
    // make sure there is a identity transformation between base of real robot and estimated robot
    publishTransform(t, root_, tf::resolve(tf_prefix_, root_));
    // publish fixed transforms
    robot_state_publisher_->publishFixedTransforms(tf_prefix_);
    // publish image
    sensor_msgs::Image overlay;
    image_viz.get_image(overlay);
    publishImage(t, overlay);

    // publish point cloud
    Observation full_image = ri::Ros2Eigen<Scalar>(ros_image, 1);
    if(!data_in_meters_)
      full_image = full_image/1000.; // convert to meters
    Eigen::Matrix3d temp = camera_matrix_;
    camera_matrix_.topLeftCorner(2,3) *= double(downsampling_factor_);
    publishPointCloud(full_image, t);
    camera_matrix_ = temp;


    MEASURE("-----------------> GRAND TOTAL");

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}























Eigen::VectorXd RobotTracker::FilterAndReturn(const sensor_msgs::Image& ros_image)
{
    INIT_PROFILING;
    boost::mutex::scoped_lock lock(mutex_);

    double delta_time;
    if(std::isnan(last_measurement_time_))
        delta_time = 0;
    else
        delta_time = ros_image.header.stamp.toSec() - last_measurement_time_;
    last_measurement_time_ = ros_image.header.stamp.toSec();

    if(delta_time > 0.04)
    {
        std::cout << "delta_time: " << delta_time;
        std::cout << "!!!!!!!!!!!!!!!!!!!!!!! SKIPPED FRAME!!!!!!! !!!!!!!!!" << std::endl;
        std::cout << "last_measurement_time_: " << last_measurement_time_ << std::endl;
        std::cout << "ros_image.header.stamp.toSec(): " << ros_image.header.stamp.toSec() << std::endl;


    }

    // convert image
    Observation image = ri::Ros2Eigen<Scalar>(ros_image, downsampling_factor_);
    if(!data_in_meters_)
      image = image/1000.; // convert to meters

    // filter
    {
    INIT_PROFILING;
    filter_->Filter(image, delta_time, Eigen::VectorXd::Zero(dimension_));
    MEASURE("-----------------> total time for filtering");
    }

    // get the mean estimation for the robot joints
    // Casting is a disgusting hack to make sure that the correct equal-operator is used
    // TODO: Make this right
    *mean_ = (Eigen::VectorXd)(filter_->state_distribution().mean());

    // DEBUG to see depth images
    robot_renderer_->state(*mean_);
        std::vector<int> indices;
        std::vector<float> depth;
        robot_renderer_->Render(camera_matrix_,
                image.rows(),
                image.cols(),
                indices,
                depth);
    //image_viz_ = boost::shared_ptr<vis::ImageVisualizer>(new vis::ImageVisualizer(image.rows(),image.cols()));
        vis::ImageVisualizer image_viz(image.rows(),image.cols());
        image_viz.set_image(image);
        image_viz.add_points(indices, depth);
    //image_viz.show_image("enchilada ", 500, 500, 1.0);
    //////

    std::map<std::string, double> joint_positions;
    mean_->GetJointState(joint_positions);


    ros::Time t = ros::Time::now();
    // publish movable joints
    robot_state_publisher_->publishTransforms(joint_positions,  t, tf_prefix_);
    // make sure there is a identity transformation between base of real robot and estimated robot
    publishTransform(t, root_, tf::resolve(tf_prefix_, root_));
    // publish fixed transforms
    robot_state_publisher_->publishFixedTransforms(tf_prefix_);
    // publish image
    sensor_msgs::Image overlay;
    image_viz.get_image(overlay);
    publishImage(t, overlay);

    // publish point cloud
    Observation full_image = ri::Ros2Eigen<Scalar>(ros_image, 1);
    if(!data_in_meters_)
      full_image = full_image/1000.; // convert to meters
    Eigen::Matrix3d temp = camera_matrix_;
    camera_matrix_.topLeftCorner(2,3) *= double(downsampling_factor_);
    publishPointCloud(full_image, t);
    camera_matrix_ = temp;


    MEASURE("-----------------> GRAND TOTAL");

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    return *mean_;

}
































void RobotTracker::publishImage(const ros::Time& time,
        sensor_msgs::Image &image)
{
    image.header.frame_id = tf::resolve(tf_prefix_, camera_frame_);
    image.header.stamp = time;
    pub_rgb_image_.publish (image);
}

void RobotTracker::publishTransform(const ros::Time& time,
        const std::string& from,
        const std::string& to)
{
    static tf::TransformBroadcaster br;
    tf::Transform transform;
    transform.setIdentity();
    br.sendTransform(tf::StampedTransform(transform, time, from, to));
}

void RobotTracker::publishPointCloud(const Observation& image,
                                     const ros::Time& stamp)
{

    float bad_point = std::numeric_limits<float>::quiet_NaN();

    sensor_msgs::PointCloud2Ptr points = boost::make_shared<sensor_msgs::PointCloud2 > ();
    points->header.frame_id =  tf::resolve(tf_prefix_, camera_frame_);
    points->header.stamp = stamp;
    points->width        = image.cols();
    points->height       = image.rows();
    points->is_dense     = false;
    points->is_bigendian = false;
    points->fields.resize( 3 );
    points->fields[0].name = "x";
    points->fields[1].name = "y";
    points->fields[2].name = "z";
    int offset = 0;
    for (size_t d = 0;
     d < points->fields.size ();
     ++d, offset += sizeof(float)) {
      points->fields[d].offset = offset;
      points->fields[d].datatype =
    sensor_msgs::PointField::FLOAT32;
      points->fields[d].count  = 1;
    }

    points->point_step = offset;
    points->row_step   =
      points->point_step * points->width;

    points->data.resize (points->width *
             points->height *
             points->point_step);

    for (size_t u = 0, nRows = image.rows(), nCols = image.cols(); u < nCols; ++u)
      for (size_t v = 0; v < nRows; ++v)
    {
      float depth = image(v,u);
      if(depth!=depth)// || depth==0.0)
        {
          // depth is invalid
          memcpy (&points->data[v * points->row_step + u * points->point_step + points->fields[0].offset], &bad_point, sizeof (float));
          memcpy (&points->data[v * points->row_step + u * points->point_step + points->fields[1].offset], &bad_point, sizeof (float));
          memcpy (&points->data[v * points->row_step + u * points->point_step + points->fields[2].offset], &bad_point, sizeof (float));
        }
      else
        {
          // depth is valid
          float x = ((float)u - camera_matrix_(0,2)) * depth / camera_matrix_(0,0);
          float y = ((float)v - camera_matrix_(1,2)) * depth / camera_matrix_(1,1);
          memcpy (&points->data[v * points->row_step + u * points->point_step + points->fields[0].offset], &x, sizeof (float));
          memcpy (&points->data[v * points->row_step + u * points->point_step + points->fields[1].offset], &y, sizeof (float));
          memcpy (&points->data[v * points->row_step + u * points->point_step + points->fields[2].offset], &depth, sizeof (float));
        }
    }

    if (  pub_point_cloud_->getNumSubscribers () > 0)
      pub_point_cloud_->publish (points);
}
