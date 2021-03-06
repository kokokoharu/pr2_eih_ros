#include "ros/ros.h"
#include <ros/console.h>

#include <tf/transform_listener.h>
#include <tf/transform_datatypes.h>
#include <tf_conversions/tf_eigen.h>
#include "sensor_msgs/PointCloud2.h"
#include <std_msgs/Empty.h>
#include <std_msgs/Float64.h>
#include <std_msgs/String.h>

#include <pcl_conversions/pcl_conversions.h>
// PCL specific includes
//#include <pcl/conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <pcl/io/openni_grabber.h>
#include <pcl/io/pcd_io.h>

#include <Eigen/Eigen>
using namespace Eigen;

#include <boost/thread.hpp>

#include <pcl/console/parse.h>
#include <pcl/gpu/kinfu_large_scale/kinfu.h>
#include <pcl/gpu/kinfu_large_scale/raycaster.h>
//#include <pcl/gpu/kinfu_large_scale/marching_cubes.h>
#include <pcl/gpu/kinfu_large_scale/tsdf_volume.h>
#include <pcl/gpu/containers/initialization.h>
#include <pcl/gpu/containers/device_array.h>

// stuff for writing tsdf vectors
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <iostream>

#include <cuda_runtime.h>
#include <assert.h>

typedef float VoxelT;
typedef short WeightT;

#define WIDTH_FULL   640 // 256
#define HEIGHT_FULL    480 // 192

#define W_SUB 64 // 64
#define H_SUB 48 // 48
#define N_SUB (W_SUB*H_SUB)

//#define USE_COLOR
//#define SAVE_TSDF
#define FIND_OCCLUSIONS
//#define PUBLISH_WEIGHTS

#ifdef FIND_OCCLUSIONS
#include <pcl_utils/occluded_region_finder.h>
#endif

boost::shared_ptr<tf::TransformListener> listener;
ros::Publisher pub, current_pointcloud_pub, variable_pub, markers_pub, points_pub, regions_pub, plane_pub,
			  object_points_pub, plane_points_pub, logger_pub;
ros::Subscriber signal_sub, head_points_sub, reset_sub, head_camera_time_sub;
bool downloading;
int counter;
bool publish_kinfu_under_cam_depth_reg;
pcl::PointCloud<pcl::PointXYZRGB> last_cloud;
pcl::PointCloud<pcl::PointXYZ> last_head_cloud;
int current;
pcl::gpu::kinfuLS::KinfuTracker *pcl_kinfu_tracker;
bool using_head_camera;
bool head_camera_active;

namespace carmine
{
const int WIDTH = 640;
const int HEIGHT = 480;

// https://vision.in.tum.de/data/datasets/rgbd-dataset/file_formats
const double fx = 525.0;
const double fy = 525.0;
const double cx = 319.5;
const double cy = 239.5;
}

namespace kinfu
{
// x and y must be equal!
const double x = 2;
const double y = 2;
const double z = 2;

const float shifting_distance = 5.0f;
}

struct null_deleter     //prevents the calling of destructor for Boost Shared Pointers
{
    void operator()(void const *) const
    {
    }
};

template<typename MergedT, typename PointT>
typename pcl::PointCloud<MergedT>::Ptr merge(const pcl::PointCloud<PointT>& points, const pcl::PointCloud<pcl::RGB>& colors)
{
    typename pcl::PointCloud<MergedT>::Ptr merged_ptr(new pcl::PointCloud<MergedT>());

    pcl::copyPointCloud (points, *merged_ptr);

    for (size_t i = 0; i < colors.size (); ++i)
        merged_ptr->points[i].rgba = colors.points[i].rgba;

    return merged_ptr;
}

void reset_kinfu(const std_msgs::EmptyConstPtr& empty)
{
    if (!downloading)
    {
        downloading = true;
        std::cout << "Resetting pointcloud..." << std::endl;
        pcl_kinfu_tracker->reset();
        std::cout << "Reset pointcloud" << std::endl;
        downloading = false;
    }
}

void activate_head_camera(const std_msgs::Float64ConstPtr duration)
{
  if (!head_camera_active)
    {
      head_camera_active = true;
      ros::Duration(duration->data).sleep();
      head_camera_active = false;
    }
}

void publish_to_logger(const std::vector<short>& tsdf_weights) {
	int num_zero = 0, num_nonzero = 0;
	for(int i=0; i < tsdf_weights.size(); ++i) {
		if (tsdf_weights[i] > 0) {
			num_nonzero++;
		} else {
			num_zero++;
		}
	}

	std::stringstream ss_zero;
	std_msgs::String msg_zero;
	ss_zero << "kinfu zero weights " << num_zero;
	msg_zero.data = ss_zero.str().c_str();
	logger_pub.publish(msg_zero);

	std::stringstream ss_nonzero;
	std_msgs::String msg_nonzero;
	ss_nonzero << "kinfu nonzero weights " << num_nonzero;
	msg_nonzero.data = ss_nonzero.str().c_str();
	logger_pub.publish(msg_nonzero);
}

void get_occluded(const std_msgs::EmptyConstPtr& str)
{
    if (!downloading)
    {
        downloading = true;
        std::cout << "Publishing kinfu cloud...\n";
        #ifndef USE_COLOR
        pcl::PointCloud<pcl::PointXYZ> current_cloud;
        // Download tsdf and convert to pointcloud
        pcl::gpu::kinfuLS::TsdfVolume tsdf = pcl_kinfu_tracker->volume();

        std::cout << "Fetching cloud host...\n";
        tsdf.fetchCloudHost(current_cloud);
        // Publish the data
        sensor_msgs::PointCloud2 output;
        //toROSMsg(transformed_cloud, output);
        toROSMsg(current_cloud, output);
        output.header.stamp = ros::Time::now();
        //output.header.frame_id = "/camera_rgb_optical_frame";
        output.header.frame_id = "/kinfu_frame";
        pub.publish (output);
        if (publish_kinfu_under_cam_depth_reg)
        {
        	variable_pub.publish(output);
        }
        std::cout << "Kinfu cloud published\n";
        std::cout << "Kinfu cloud size: " << output.data.size() << "\n\n";


        #if defined(FIND_OCCLUSIONS) || defined(SAVE_TSDF) || defined(PUBLISH_WEIGHTS)
        // TODO: I don't know if these variable names are all correct
        // i.e. maybe it should be called kinfu_to_rgb, etc.

        // get the transform from from rgb optical frame to kinfu frame
        tf::StampedTransform rgb_to_kinfu_write;
        listener->waitForTransform("/camera_rgb_optical_frame", "/kinfu_frame",
                                   ros::Time(0), ros::Duration(5));
        listener->lookupTransform("/camera_rgb_optical_frame", "/kinfu_frame",
                                  ros::Time(0), rgb_to_kinfu_write);


        std::vector<float> tsdf_vector;
        std::vector<short> tsdf_weights;
        //tsdf.save("kinfu_tsdf.dat"); // doesn't work for some reason


        tsdf.downloadTsdfAndWeights(tsdf_vector, tsdf_weights);
        std::cout << "distances: " << tsdf_vector.size() << std::endl;
        std::cout << "weights: " << tsdf_weights.size() << std::endl;
        publish_to_logger(tsdf_weights);

        // transform kinfu points back to rgb optical frame
        Affine3d current_transform_write;
        tf::transformTFToEigen(rgb_to_kinfu_write, current_transform_write);

        Matrix4d transformation_matrix = current_transform_write.matrix();
        #endif // defined(FIND_OCCLUSIONS) || defined(SAVE_TSDF) || defined(PUBLISH_WEIGHTS)

        #ifdef PUBLISH_WEIGHTS
        // TODO: fill this in later with code to publish weighted cloud -- currently in separate file (get_weight_cloud.cpp)
        #endif

        #ifdef FIND_OCCLUSIONS
//		pcl::PointCloud<pcl::PointXYZ>::Ptr zero_crossing_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new  pcl::PointCloud<pcl::PointXYZ>);
//        pcl::PointCloud<pcl::PointXYZ>::Ptr foreground_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new  pcl::PointCloud<pcl::PointXYZ>);
//        PointCloudVoxelGrid::CloudType::Ptr inverse_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new  pcl::PointCloud<pcl::PointXYZ>);
        pcl::PointCloud<pcl::PointXYZ>::Ptr current_cloud_ptr (new pcl::PointCloud<pcl::PointXYZ> (current_cloud));
        occluded_region_finder::find_occluded_regions(tsdf_vector, tsdf_weights, current_cloud_ptr, transformation_matrix, false, "kinfu", markers_pub, points_pub, regions_pub, plane_pub, object_points_pub, plane_points_pub); //,
        //zero_crossing_cloud, foreground_cloud, inverse_cloud);
        #endif // FIND_OCCLUSIONS


        #ifdef SAVE_TSDF
        // write the vectors as binary data

        std::stringstream current_stream;
        current_stream << current;

        std::ofstream dist_out;
        const char* dist_pointer = reinterpret_cast<const char*>(&tsdf_vector[0]);
        size_t bytes = tsdf_vector.size() * sizeof(tsdf_vector[0]);
        std::string dist_file = "kinfu_dist" + current_stream.str() + ".dat";
        dist_out.open(dist_file.c_str(), std::ios::out | std::ios::binary);
        dist_out.write(dist_pointer, bytes);
        dist_out.close();

        std::ofstream weight_out;
        const char* weight_pointer = reinterpret_cast<const char*>(&tsdf_weights[0]);
        bytes = tsdf_weights.size() * sizeof(tsdf_weights[0]);

        std::string weight_file = "kinfu_weights" + current_stream.str() + ".dat";
        weight_out.open(weight_file.c_str(), std::ios::out | std::ios::binary);
        weight_out.write(weight_pointer, bytes);
        weight_out.close();


        std::ofstream matrix_outstream;
        std::string matrix_file = "transform_matrix" + current_stream.str() + ".txt";
        matrix_outstream.open(matrix_file.c_str());
        for (int x = 0; x < transformation_matrix.rows(); x++)
        {
            for (int y = 0; y < transformation_matrix.cols(); y++)
            {
                matrix_outstream << transformation_matrix(x, y) << std::endl;
            }
        }
        matrix_outstream.close();


        current++;
        std::cout << "saved!" << std::endl;
        #endif // SAVE_TSDF

        #else
        // new way of doing it, with color

        pcl::gpu::DeviceArray<pcl::PointXYZ> cloud_buffer_device_;
        pcl::gpu::DeviceArray<pcl::RGB> point_colors_device_;

        pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_ptr_ = pcl::PointCloud<pcl::PointXYZ>::Ptr (new pcl::PointCloud<pcl::PointXYZ>);
        pcl::PointCloud<pcl::RGB>::Ptr point_colors_ptr_ = pcl::PointCloud<pcl::RGB>::Ptr (new pcl::PointCloud<pcl::RGB>);

        pcl::gpu::DeviceArray<pcl::PointXYZ> extracted = pcl_kinfu_tracker->volume().fetchCloud(cloud_buffer_device_); // cloud_buffer_device_ is just another pcl::gpu::DeviceArray<pcl::PointXYZ>
        extracted.download (cloud_ptr_->points);
        cloud_ptr_->width = (int)cloud_ptr_->points.size ();
        cloud_ptr_->height = 1;

        pcl_kinfu_tracker->colorVolume().fetchColors(extracted, point_colors_device_); // same as above for point_colors_device
        point_colors_device_.download(point_colors_ptr_->points);
        point_colors_ptr_->width = (int)point_colors_ptr_->points.size ();
        point_colors_ptr_->height = 1;


        pcl::PointCloud<pcl::PointXYZRGB> current_cloud = *(merge<pcl::PointXYZRGB>(*cloud_ptr_, *point_colors_ptr_));

        #endif // ndef USE_COLOR

        downloading = false;
    }

}


void update_kinfu_loop(pcl::gpu::kinfuLS::KinfuTracker *pcl_kinfu_tracker)
{
    while(ros::ok())
    {
        if (!downloading)
        {
            std::cout << "Updating kinfu...\n";

            pcl::PointCloud<pcl::PointXYZRGB> cloud;
            pcl::copyPointCloud(last_cloud, cloud);

            // get the current location of the camera relative to the kinfu frame (see kinfu.launch)
            tf::StampedTransform kinfu_to_camera;
            listener->waitForTransform("/kinfu_frame", "/camera_rgb_optical_frame",
                                       ros::Time(0), ros::Duration(5));
            listener->lookupTransform("/kinfu_frame", "/camera_rgb_optical_frame",
                                      ros::Time(0), kinfu_to_camera);

            // convert camera pose to format suitable for kinfu
            Affine3d affine_current_cam_pose;
            tf::transformTFToEigen(kinfu_to_camera, affine_current_cam_pose);

            pcl::PointCloud<pcl::PointXYZRGB> transformed_cloud;
            pcl::transformPointCloud(cloud, transformed_cloud, affine_current_cam_pose);


            // convert the data into gpu format for kinfu tracker to use
            pcl::gpu::DeviceArray2D<unsigned short> depth(carmine::HEIGHT,carmine::WIDTH);
            std::vector<unsigned short> data(carmine::HEIGHT*carmine::WIDTH);

            #ifdef USE_COLOR
            // the same for color data
            pcl::gpu::DeviceArray2D<pcl::gpu::kinfuLS::PixelRGB> color(carmine::HEIGHT,carmine::WIDTH);
            std::vector<pcl::gpu::kinfuLS::PixelRGB> color_data(carmine::HEIGHT*carmine::WIDTH);
            #endif

            const int cols = carmine::WIDTH;

            int i;
            pcl::PointCloud<pcl::PointXYZRGB>::iterator cloud_iter;
            for(cloud_iter = cloud.begin(), i = 0;
                    cloud_iter != cloud.end();
                    cloud_iter++, i++)
            {
                data[i] = static_cast<unsigned short>(1e3*cloud_iter->z);
            #ifdef USE_COLOR
                pcl::gpu::kinfuLS::PixelRGB current_pixel = pcl::gpu::kinfuLS::PixelRGB();
                current_pixel.r = cloud_iter->r;
                current_pixel.g = cloud_iter->g;
                current_pixel.b = cloud_iter->b;
                color_data[i] = current_pixel;
            #endif

            }


            depth.upload(data, cols);
            #ifdef USE_COLOR
            color.upload(color_data, cols);
            #endif

            // update kinfu tracker with new depth map and camera pose
            (*pcl_kinfu_tracker)(depth, affine_current_cam_pose.cast<float>());
            #ifdef USE_COLOR
            (*pcl_kinfu_tracker)(depth, color);
            #endif



            if (using_head_camera && head_camera_active) {

                pcl::PointCloud<pcl::PointXYZ> head_cloud;
                pcl::copyPointCloud(last_head_cloud, head_cloud);

                // get the current location of the camera relative to the kinfu frame (see kinfu.launch)
                tf::StampedTransform kinfu_to_head_camera;
		std::string head_camera_frame = "/head_camera_rgb_optical_frame";
                listener->waitForTransform("/kinfu_frame", head_camera_frame,
                                           ros::Time(0), ros::Duration(5));
                listener->lookupTransform("/kinfu_frame", head_camera_frame,
                                          ros::Time(0), kinfu_to_head_camera);

                // convert camera pose to format suitable for kinfu
                Affine3d affine_current_head_cam_pose;
                tf::transformTFToEigen(kinfu_to_head_camera, affine_current_head_cam_pose);

                pcl::PointCloud<pcl::PointXYZ> transformed_head_cloud;
                pcl::transformPointCloud(head_cloud, transformed_head_cloud, affine_current_head_cam_pose);


                // convert the data into gpu format for kinfu tracker to use
                pcl::gpu::DeviceArray2D<unsigned short> head_depth(carmine::HEIGHT,carmine::WIDTH);
                std::vector<unsigned short> head_data(carmine::HEIGHT*carmine::WIDTH);

                const int cols = carmine::WIDTH;

                // TODO: Greg - does this really iterate through the points in the correct order?
                int i;
                pcl::PointCloud<pcl::PointXYZ>::iterator head_cloud_iter;
                for(head_cloud_iter = head_cloud.begin(), i = 0;
                        head_cloud_iter != head_cloud.end();
                        head_cloud_iter++, i++)
                {
                    head_data[i] = static_cast<unsigned short>(1e3*head_cloud_iter->z);

                }


                head_depth.upload(head_data, cols);

                // update kinfu tracker with new depth map and camera pose
                (*pcl_kinfu_tracker)(head_depth, affine_current_head_cam_pose.cast<float>());

                std::cout << "added head camera data" << std::endl;

            }

            std::cout << "Updated kinfu!\n\n";
        }

        ros::spinOnce();
    }
}

void _cloud_callback (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr& input)
{
    pcl::copyPointCloud(*input, last_cloud);

    sensor_msgs::PointCloud2 output;
    toROSMsg(*input, output);

    output.header.stamp = ros::Time::now();
    output.header.frame_id = "/camera_rgb_optical_frame";

    // Publish the data
    current_pointcloud_pub.publish (output);
    if (!publish_kinfu_under_cam_depth_reg)
    {
        variable_pub.publish(output);
    }

}

void _head_cloud_callback(sensor_msgs::PointCloud2 input) {
    pcl::PointCloud<pcl::PointXYZ> head_points;
    fromROSMsg(input, head_points);

    pcl::copyPointCloud(head_points, last_head_cloud);
}

pcl::gpu::kinfuLS::KinfuTracker* init_kinfu()
{
    // setup kinfu tracker
    pcl::gpu::kinfuLS::KinfuTracker *pcl_kinfu_tracker = new pcl::gpu::kinfuLS::KinfuTracker(Vector3f(kinfu::y, kinfu::z, kinfu::x), kinfu::shifting_distance, carmine::HEIGHT, carmine::WIDTH);
    pcl_kinfu_tracker->setDepthIntrinsics(carmine::fx, carmine::fy, carmine::cx, carmine::cy);

    // the transform from /base_link to /kinfu_frame
    // now published in kinfu.launch, so the tf listener can just read it in
    tf::StampedTransform kinfu_to_camera;
    listener->waitForTransform("/kinfu_frame", "/camera_rgb_optical_frame",
                               ros::Time(0), ros::Duration(6));
    listener->lookupTransform("/kinfu_frame", "/camera_rgb_optical_frame",
                              ros::Time(0), kinfu_to_camera);

    // set initial pose in kinfu_tracker
    Affine3d affine_init_cam_pose;
    tf::transformTFToEigen(kinfu_to_camera, affine_init_cam_pose);
    pcl_kinfu_tracker->setInitialCameraPose(affine_init_cam_pose.cast<float>());
    pcl_kinfu_tracker->reset();
    #ifdef USE_COLOR
    pcl_kinfu_tracker->initColorIntegration();
    #endif

    return pcl_kinfu_tracker;
}

int main (int argc, char** argv)
{
    pcl::console::setVerbosityLevel(pcl::console::L_ERROR);

    // Initialize ROS

    ros::init(argc, argv, "kinfu");
    ros::NodeHandle nh("~");
    //ros::Duration(2).sleep();
    std::string dev;
    // fill in tf listener
    listener.reset(new tf::TransformListener(ros::Duration(20.0)));

    ros::param::param<bool>("/occlusion_parameters/using_head_camera", using_head_camera, false);

    if (!nh.getParam("device_number", dev))
    {
        dev = "1";
    }
    std::string pointcloud_topic;
    if (!nh.getParam("pointcloud_topic", pointcloud_topic))
    {
        pointcloud_topic = "kinfu_points";
    }
    std::cout << "pointcloud_topic: " << pointcloud_topic << "\n";

    // grabber to get data from the device
    pcl::Grabber* grabber = new pcl::OpenNIGrabber("#" + dev);

    // Create a ROS publisher for the output point cloud
    pub = nh.advertise<sensor_msgs::PointCloud2> (pointcloud_topic, 1);
    current_pointcloud_pub = nh.advertise<sensor_msgs::PointCloud2> ("camera_points", 1);
    boost::function<void (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr&)> f = boost::bind(&_cloud_callback, _1);

    #ifdef FIND_OCCLUSIONS
    // ROS publishers for occluded region finding
    markers_pub = nh.advertise<visualization_msgs::MarkerArray> ("objects", 1);
    points_pub = nh.advertise<sensor_msgs::PointCloud2> ("occluded_points", 1);
    regions_pub = nh.advertise<pcl_utils::OccludedRegionArray> ("occluded_regions", 1);
    signal_sub = nh.subscribe<std_msgs::Empty> ("get_occluded", 1, get_occluded);
    plane_pub = nh.advertise<pcl_utils::BoundingBox> ("plane_bounding_box", 1);
    object_points_pub = nh.advertise<sensor_msgs::PointCloud2> ("graspable_points", 1);
    plane_points_pub = nh.advertise<sensor_msgs::PointCloud2>("plane_points", 1);
    reset_sub = nh.subscribe<std_msgs::Empty> ("reset", 1, reset_kinfu);
    #endif

    head_camera_time_sub = nh.subscribe<std_msgs::Float64> ("head_camera_duration", 1, activate_head_camera);

    head_points_sub = nh.subscribe<sensor_msgs::PointCloud2>("/head_camera/depth_registered/points", 1, _head_cloud_callback);

    logger_pub = nh.advertise<std_msgs::String>("/experiment_log", 1);

    std::string variable_topic;
    if (!nh.getParam("handle_points_source", variable_topic))
    {
        publish_kinfu_under_cam_depth_reg = true;
    }
    else if (variable_topic == "kinfu")
    {
        publish_kinfu_under_cam_depth_reg = true;
    }
    else
    {
        publish_kinfu_under_cam_depth_reg = false;
    }

    variable_pub = nh.advertise<sensor_msgs::PointCloud2> ("/camera/depth_registered/points", 1);

    grabber->registerCallback(f);
    grabber->start();

    pcl_kinfu_tracker = init_kinfu();

    ros::spinOnce();

    boost::thread update_kinfu_thread(update_kinfu_loop, pcl_kinfu_tracker);

    std::cout << "Ready to publish clouds\n";
    downloading = false;
    head_camera_active = false;

    current = 1;
//	while(ros::ok()) {
//		ros::spinOnce();
//		ros::Duration(5).sleep();
////		std::string response;
////		std::getline(std::cin, response); // wait for key press
////		if (response == "q") {
////			grabber->stop();
////			pub.publish(output);
////			pcl::io::savePCDFileASCII("kinfu.pcd", current_cloud);
////			exit(0);
////		}
//
//
//	}

    // Spin
    ros::spin ();
    grabber->stop();

    update_kinfu_thread.join();
}



/*
  pcl::gpu::DeviceArray<pcl::PointXYZ> cloud_buffer_device_;
  pcl::gpu::DeviceArray<pcl::RGB> point_colors_device_;

  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_ptr_ = pcl::PointCloud<pcl::PointXYZ>::Ptr (new pcl::PointCloud<pcl::PointXYZ>);
  pcl::PointCloud<pcl::RGB>::Ptr point_colors_ptr_ = pcl::PointCloud<pcl::RGB>::Ptr (new pcl::PointCloud<pcl::RGB>);

  pcl::gpu::DeviceArray<pcl::PointXYZ> extracted = kinfu.volume().fetchCloud (cloud_buffer_device_); // cloud_buffer_device_ is just another pcl::gpu::DeviceArray<pcl::PointXYZ>
  extracted.download (cloud_ptr_->points);
  cloud_ptr_->width = (int)cloud_ptr_->points.size ();
  cloud_ptr_->height = 1;

  kinfu.colorVolume().fetchColors(extracted, point_colors_device_); // same as above for point_colors_device
  point_colors_device_.download(point_colors_ptr_->points);
  point_colors_ptr_->width = (int)point_colors_ptr_->points.size ();
  point_colors_ptr_->height = 1;


  pcl::PointCloud<pcl::PointXYZRGB> current_cloud = std::merge<pcl::PointXYZRGB>(cloud_ptr_, point_colors_ptr_);

 */
