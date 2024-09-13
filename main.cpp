#include <rclcpp/rclcpp.hpp>
#include "sensorSubscriber.h"
#include "isaeslam/slamParameters.h"
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <iostream>

int main(int argc, char **argv) {
    rclcpp::init(argc, argv);

    // Create the SLAM parameter object
    std::string path = "/home/deos/ce.debeunne/colcon_ws/src/SaDVIO/ros/config";
    isae::SLAMParameters slam_param(path);

    std::cout << slam_param._config.slam_mode << std::endl;

    // Start the sensor subscriber
    std::shared_ptr<SensorSubscriber> sensor_subscriber =
        std::make_shared<SensorSubscriber>(slam_param.getDataProvider());
    
    rclcpp::spin(sensor_subscriber);

    return 0;

}