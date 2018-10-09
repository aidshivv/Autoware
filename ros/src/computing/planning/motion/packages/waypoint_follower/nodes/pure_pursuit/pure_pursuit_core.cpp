/*
 *  Copyright (c) 2015, Nagoya University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of Autoware nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "pure_pursuit_core.h"

namespace waypoint_follower
{
// Constructor
PurePursuitNode::PurePursuitNode()
  : private_nh_("~")
  , pp_()
  , LOOP_RATE_(30)
  , is_waypoint_set_(false)
  , is_pose_set_(false)
  , is_velocity_set_(false)
  , is_config_set_(false)
  , current_linear_velocity_(0)
  , command_linear_velocity_(0)
  , param_flag_(-1)
  , const_lookahead_distance_(4.0)
  , const_velocity_(5.0)
  , lookahead_distance_ratio_(2.0)
  , minimum_lookahead_distance_(6.0)
  , positionstop_state_(false)
{
  initForROS();

  // initialize for PurePursuit
  pp_.setLinearInterpolationParameter(is_linear_interpolation_);
}

// Destructor
PurePursuitNode::~PurePursuitNode()
{
}

void PurePursuitNode::initForROS()
{
  // ros parameter settings
  private_nh_.param("is_linear_interpolation", is_linear_interpolation_, bool(true));
  // ROS_INFO_STREAM("is_linear_interpolation : " << is_linear_interpolation_);
  private_nh_.param("publishes_for_steering_robot", publishes_for_steering_robot_, bool(false));
  nh_.param("vehicle_info/wheel_base", wheel_base_, double(2.7));

  // setup subscriber
  sub1_ = nh_.subscribe("final_waypoints", 10, &PurePursuitNode::callbackFromWayPoints, this);
  sub2_ = nh_.subscribe("current_pose", 10, &PurePursuitNode::callbackFromCurrentPose, this);
  sub3_ = nh_.subscribe("config/waypoint_follower", 10, &PurePursuitNode::callbackFromConfig, this);
  sub4_ = nh_.subscribe("current_velocity", 10, &PurePursuitNode::callbackFromCurrentVelocity, this);
  sub5_ = nh_.subscribe("/decision_maker/state", 10, &PurePursuitNode::callbackFromState, this);

  // setup publisher
  pub1_ = nh_.advertise<geometry_msgs::TwistStamped>("twist_raw", 10);
  pub2_ = nh_.advertise<autoware_msgs::ControlCommandStamped>("ctrl_cmd", 10);
  pub11_ = nh_.advertise<visualization_msgs::Marker>("next_waypoint_mark", 0);
  pub12_ = nh_.advertise<visualization_msgs::Marker>("next_target_mark", 0);
  pub13_ = nh_.advertise<visualization_msgs::Marker>("search_circle_mark", 0);
  pub14_ = nh_.advertise<visualization_msgs::Marker>("line_point_mark", 0);  // debug tool
  pub15_ = nh_.advertise<visualization_msgs::Marker>("trajectory_circle_mark", 0);
  pub16_ = nh_.advertise<std_msgs::Float32>("angular_gravity", 0);
  pub17_ = nh_.advertise<std_msgs::Float32>("deviation_of_current_position", 0);
  virtual_pose_pub_ = nh_.advertise<geometry_msgs::PoseStamped>(ros::this_node::getName() + "/virtual_current_pose",0);
  // pub7_ = nh.advertise<std_msgs::Bool>("wf_stat", 0);
}

void PurePursuitNode::run()
{
  ROS_INFO_STREAM("pure pursuit start");
  ros::Rate loop_rate(LOOP_RATE_);
  while (ros::ok())
  {
    ros::spinOnce();
    if (!is_pose_set_ || !is_waypoint_set_ || !is_velocity_set_ || !is_config_set_)
    {
      ROS_WARN("Necessary topics are not subscribed yet ... ");
      loop_rate.sleep();
      continue;
    }

    pp_.setLookaheadDistance(computeLookaheadDistance());
    pp_.setMinimumLookaheadDistance(minimum_lookahead_distance_);

    static double kappa = 1e-8;
    bool can_get_curvature = true;
    can_get_curvature = pp_.canGetCurvature(&kappa);
    publishTwistStamped(can_get_curvature, kappa);
    publishControlCommandStamped(can_get_curvature, kappa);

    // for visualization with Rviz
    pub11_.publish(displayNextWaypoint(pp_.getPoseOfNextWaypoint()));
    pub13_.publish(displaySearchRadius(pp_.getCurrentPose().position, pp_.getLookaheadDistance()));
    pub12_.publish(displayNextTarget(pp_.getPoseOfNextTarget()));
    pub15_.publish(displayTrajectoryCircle(
        waypoint_follower::generateTrajectoryCircle(pp_.getPoseOfNextTarget(), pp_.getCurrentPose())));
    std_msgs::Float32 angular_gravity_msg;
    angular_gravity_msg.data = computeAngularGravity(computeCommandVelocity(), kappa);
    pub16_.publish(angular_gravity_msg);

    publishDeviationCurrentPosition(pp_.getCurrentPose().position, pp_.getCurrentWaypoints());

    is_pose_set_ = false;
    is_velocity_set_ = false;
    is_waypoint_set_ = false;

    loop_rate.sleep();
  }
}

void PurePursuitNode::publishTwistStamped(const bool &can_get_curvature, const double &kappa) const
{
  geometry_msgs::TwistStamped ts;
  ts.header.stamp = ros::Time::now();
  ts.twist.linear.x = can_get_curvature ? computeCommandVelocity() : 0;
  ts.twist.angular.z = can_get_curvature ? kappa * ts.twist.linear.x : 0;
  pub1_.publish(ts);
}

void PurePursuitNode::publishControlCommandStamped(const bool &can_get_curvature, const double &kappa) const
{
  if (!publishes_for_steering_robot_)
    return;

  autoware_msgs::ControlCommandStamped ccs;
  ccs.header.stamp = ros::Time::now();
  ccs.cmd.linear_velocity = can_get_curvature ? fabs(computeCommandVelocity()) : 0;
  ccs.cmd.linear_acceleration = can_get_curvature ? computeCommandAccel() : 0;
  ccs.cmd.steering_angle = can_get_curvature ? convertCurvatureToSteeringAngle(wheel_base_, kappa) : 0;

  pub2_.publish(ccs);
}

double PurePursuitNode::computeLookaheadDistance() const
{
  if (param_flag_ == enumToInteger(Mode::dialog))
    return const_lookahead_distance_;

  double maximum_lookahead_distance = current_linear_velocity_ * 10;
  double ld = current_linear_velocity_ * lookahead_distance_ratio_;

  return ld < minimum_lookahead_distance_ ? minimum_lookahead_distance_ :
                                            ld > maximum_lookahead_distance ? maximum_lookahead_distance : ld;
}

double PurePursuitNode::computeCommandVelocity() const
{
  const int sgn = (command_linear_velocity_ < 0) ? -1 : 1;
  if (param_flag_ == enumToInteger(Mode::dialog))
    return sgn * kmph2mps(const_velocity_);

  return command_linear_velocity_;
}

double PurePursuitNode::computeCommandAccel() const
{
  const geometry_msgs::Pose current_pose = pp_.getCurrentPose();
  const geometry_msgs::Pose target_pose = pp_.getCurrentWaypoints().at(1).pose.pose;

  // v^2 - v0^2 = 2ax
  const double x =
      std::hypot(current_pose.position.x - target_pose.position.x, current_pose.position.y - target_pose.position.y);
  const double v0 = current_linear_velocity_;
  const double v = computeCommandVelocity();
  const double a = (v * v - v0 * v0) / (2 * x);
  return a;
}

double PurePursuitNode::computeAngularGravity(double velocity, double kappa) const
{
  const double gravity = 9.80665;
  return (velocity * velocity) / (1.0 / kappa * gravity);
}

void PurePursuitNode::callbackFromConfig(const autoware_msgs::ConfigWaypointFollowerConstPtr &config)
{
  param_flag_ = config->param_flag;
  const_lookahead_distance_ = config->lookahead_distance;
  const_velocity_ = config->velocity;
  lookahead_distance_ratio_ = config->lookahead_ratio;
  minimum_lookahead_distance_ = config->minimum_lookahead_distance;
  is_config_set_ = true;
  delay_ = config->delay;
}

void PurePursuitNode::publishDeviationCurrentPosition(const geometry_msgs::Point &point,
                                                      const std::vector<autoware_msgs::waypoint> &waypoints) const
{
  // Calculate the deviation of current position from the waypoint approximate line

  if (waypoints.size() < 3)
  {
    return;
  }

  double a, b, c;
  double linear_flag_in =
      getLinearEquation(waypoints.at(2).pose.pose.position, waypoints.at(1).pose.pose.position, &a, &b, &c);

  std_msgs::Float32 msg;
  msg.data = getDistanceBetweenLineAndPoint(point, a, b, c);

  pub17_.publish(msg);
  return;
}

void PurePursuitNode::callbackFromCurrentPose(const geometry_msgs::PoseStampedConstPtr &msg)
{
  geometry_msgs::PoseStamped current_pose = *msg;
  if(current_angular_velocity_ != 0)
  {
    double r = current_linear_velocity_ / current_angular_velocity_;
    double theta = current_angular_velocity_ * delay_;
    tf::Quaternion current_quat(current_pose.pose.orientation.x,current_pose.pose.orientation.y,current_pose.pose.orientation.z,current_pose.pose.orientation.w);
    double current_roll, current_pitch, current_yaw;
    tf::Matrix3x3(current_quat).getRPY(current_roll, current_pitch, current_yaw);
    double dx = 2 * r * std::sin(0.5 * theta) * std::sin(theta + current_yaw);
    double dy = 2 * r * std::sin(0.5 * theta) * std::cos(theta + current_yaw);
    current_pose.pose.position.x = current_pose.pose.position.x + dx;
    current_pose.pose.position.y = current_pose.pose.position.y + dy;
    tf::Quaternion virtual_quat = tf::createQuaternionFromRPY(current_roll, current_pitch, current_yaw + theta);
    geometry_msgs::Quaternion quat;
    quaternionTFToMsg(virtual_quat,quat);
    current_pose.pose.orientation = quat;
    pp_.setCurrentPose(current_pose);
  }
  else
  {
    double l = current_linear_velocity_ * delay_;
    tf::Quaternion current_quat(current_pose.pose.orientation.x,current_pose.pose.orientation.y,current_pose.pose.orientation.z,current_pose.pose.orientation.w);
    double current_roll, current_pitch, current_yaw;
    tf::Matrix3x3(current_quat).getRPY(current_roll, current_pitch, current_yaw);
    double dx = l * std::sin(current_yaw);
    double dy = l * std::cos(current_yaw);
    current_pose.pose.position.x = msg->pose.position.x + dx;
    current_pose.pose.position.y = msg->pose.position.y + dy;
    current_pose.pose.orientation = current_pose.pose.orientation;
    pp_.setCurrentPose(current_pose);
  }
  virtual_pose_pub_.publish(current_pose);
  is_pose_set_ = true;
  return;
}

void PurePursuitNode::callbackFromCurrentVelocity(const geometry_msgs::TwistStampedConstPtr &msg)
{
  current_linear_velocity_ = msg->twist.linear.x;
  current_angular_velocity_ = msg->twist.angular.z;
  pp_.setCurrentVelocity(current_linear_velocity_);
  is_velocity_set_ = true;
  return;
}

void PurePursuitNode::callbackFromWayPoints(const autoware_msgs::laneConstPtr &msg)
{
  command_linear_velocity_ = (!msg->waypoints.empty()) ? msg->waypoints.at(0).twist.twist.linear.x : 0;
  autoware_msgs::lane expanded_lane(*msg);
  connectVirtualLastWaypoints(&expanded_lane);
  pp_.setCurrentWaypoints(expanded_lane.waypoints);
  is_waypoint_set_ = true;
  return;
}

void PurePursuitNode::connectVirtualLastWaypoints(autoware_msgs::lane* lane)
{
  if (lane->waypoints.size() < 2)
  {
    return;
  }
  const geometry_msgs::Pose& p0 = lane->waypoints[0].pose.pose;
  const geometry_msgs::Pose& p1 = lane->waypoints[1].pose.pose;
  const geometry_msgs::Pose& pn = lane->waypoints.back().pose.pose;
  const geometry_msgs::Point rlt = calcRelativeCoordinate(p1.position, p0);
  const int dir = (rlt.x > 0) ? 1 : -1;
  const double interval = getPlaneDistance(p0.position, p1.position);

  autoware_msgs::waypoint virtual_last_waypoint;
  virtual_last_waypoint.pose.pose.orientation = pn.orientation;
  virtual_last_waypoint.twist.twist.linear.x = 0.0;

  geometry_msgs::Point virtual_last_point_rlt;
  for (double dist = minimum_lookahead_distance_; dist > 0.0; dist -= interval)
  {
    virtual_last_point_rlt.x += interval * dir;
    virtual_last_waypoint.pose.pose.position = calcAbsoluteCoordinate(virtual_last_point_rlt, pn);
    lane->waypoints.push_back(virtual_last_waypoint);
  }
}

void PurePursuitNode::callbackFromState(const std_msgs::StringConstPtr &msg)
{
  const bool is_ps = (msg->data.find("PositionStop") != std::string::npos);
  if (is_ps)
  {
    positionstop_state_ = true;
  }
  else
  {
    positionstop_state_ = false;
  }
}

double convertCurvatureToSteeringAngle(const double &wheel_base, const double &kappa)
{
  return atan(wheel_base * kappa);
}

}  // waypoint_follower
