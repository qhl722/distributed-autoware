/*
 *  Copyright (c) 2018, Nagoya University
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

#include "astar_navi.h"
#include <chrono>
#include <errno.h>

int cnt = 0;
int index_x = 0;
int index_y = 0;
double **sum;

AstarNavi::AstarNavi() : nh_(), private_nh_("~")
{
  private_nh_.param<double>("waypoints_velocity", waypoints_velocity_, 5.0);
  private_nh_.param<double>("update_rate", update_rate_, 1.0);
  private_nh_.param<int>("block_number", N, 11);
  private_nh_.param<double>("block_range", RANGE, 3.0);
  private_nh_.param<int>("iteration", ITER, 10);
  private_nh_.param<int>("area_search", area_search_, 0);

  lane_pub_ = nh_.advertise<autoware_msgs::LaneArray>("lane_waypoints_array", 1, true);
  costmap_sub_ = nh_.subscribe("costmap", 1, &AstarNavi::costmapCallback, this);
  current_pose_sub_ = nh_.subscribe("current_pose", 1, &AstarNavi::currentPoseCallback, this);
  goal_pose_sub_ = nh_.subscribe("move_base_simple/goal", 1, &AstarNavi::goalPoseCallback, this);

  visual_pub_ = nh_.advertise<geometry_msgs::PoseStamped>("visual_hitachi_pose", 1);

  costmap_initialized_ = false;
  current_pose_initialized_ = false;
  goal_pose_initialized_ = false;

  sum = (double**)malloc(sizeof(double*)*N);
  for (int i = 0; i < N; i++) {
    sum[i] = (double*)malloc(sizeof(double)*N);
    for (int j = 0; j < N; j++) {
      sum[i][j] = 0;
    }
  }
}

AstarNavi::~AstarNavi()
{
}

void AstarNavi::costmapCallback(const nav_msgs::OccupancyGrid& msg)
{
  std::cout << "costmapCallback" << std::endl;
  costmap_ = msg;
  tf::poseMsgToTF(costmap_.info.origin, local2costmap_);

  costmap_initialized_ = true;
}

void AstarNavi::currentPoseCallback(const geometry_msgs::PoseStamped& msg)
{
  std::cout << "currentPoseCallback" << std::endl;
  if (!costmap_initialized_)
  {
    return;
  }

  current_pose_global_ = msg;
  current_pose_local_.pose = transformPose(
      current_pose_global_.pose, getTransform(costmap_.header.frame_id, current_pose_global_.header.frame_id));
  current_pose_local_.header.frame_id = costmap_.header.frame_id;
  current_pose_local_.header.stamp = current_pose_global_.header.stamp;

  current_pose_initialized_ = true;
}

void AstarNavi::goalPoseCallback(const geometry_msgs::PoseStamped& msg)
{
  std::cout << "goalPoseCallback" << std::endl;
  if (!costmap_initialized_)
  {
    return;
  }

  goal_pose_global_ = msg;
  goal_pose_local_.pose =
      transformPose(goal_pose_global_.pose, getTransform(costmap_.header.frame_id, goal_pose_global_.header.frame_id));
  goal_pose_local_.header.frame_id = costmap_.header.frame_id;
  goal_pose_local_.header.stamp = goal_pose_global_.header.stamp;

  goal_pose_initialized_ = true;

  ROS_INFO_STREAM("Subscribed goal pose and transform from " << msg.header.frame_id << " to "
                                                             << goal_pose_local_.header.frame_id << "\n"
                                                             << goal_pose_local_.pose);
}

tf::Transform AstarNavi::getTransform(const std::string& from, const std::string& to)
{
  tf::StampedTransform stf;
  try
  {
    tf_listener_.lookupTransform(from, to, ros::Time(0), stf);
  }
  catch (tf::TransformException ex)
  {
    ROS_ERROR("%s", ex.what());
  }
  return stf;
}

void AstarNavi::run()
{
  ros::Rate rate(update_rate_);

  std::chrono::time_point<std::chrono::system_clock> start, end;

  nav_msgs::Path empty_path;
  empty_path.header.stamp = ros::Time::now();
  empty_path.header.frame_id = costmap_.header.frame_id;

  FILE *fp = fopen("/home/tomoya/sandbox/astar_prob.csv", "w");
  if (fp == NULL) {
    fprintf(stderr, "fopen error astar_prob.csv\n");
    exit(EXIT_FAILURE);
  }

  double **time_array;
  bool **result_array;
  time_array = (double**)malloc(sizeof(double*)*N*N);
  result_array = (bool**)malloc(sizeof(bool*)*N*N);
  for (int i = 0; i < N * N; i++) 
  {
    time_array[i] = (double*)malloc(sizeof(double)*ITER);
    result_array[i] = (bool*)malloc(sizeof(bool)*ITER);
  }

  int area_index, loop_index;
  
  while (ros::ok())
  {
    ros::spinOnce();

    if (!costmap_initialized_ || !current_pose_initialized_ || !goal_pose_initialized_)
    {
      std::cout << "costmap: " << costmap_initialized_ << ", current_pose: " << current_pose_initialized_ << ", goal_pose: " << goal_pose_initialized_ << std::endl;
      rate.sleep();
      continue;
    }

    // start = std::chrono::system_clock::now();

    // initialize vector for A* search, this runs only once
    astar.initialize(costmap_);

    // update local goal pose
    goalPoseCallback(goal_pose_global_);

    geometry_msgs::PoseStamped m_pose;
    m_pose.header = current_pose_local_.header;
    m_pose.pose.position.x = goal_pose_local_.pose.position.x + RANGE * (index_x - (N/2 + 1));
    m_pose.pose.position.y = goal_pose_local_.pose.position.y + RANGE * (index_y - (N/2 + 1));
    m_pose.pose.position.z = goal_pose_local_.pose.position.z;
    m_pose.pose.orientation.x = goal_pose_local_.pose.orientation.x;
    m_pose.pose.orientation.y = goal_pose_local_.pose.orientation.y;
    m_pose.pose.orientation.z = goal_pose_local_.pose.orientation.z;
    m_pose.pose.orientation.w = goal_pose_local_.pose.orientation.w;

    start = std::chrono::system_clock::now();

    // execute astar search
    // ros::WallTime start = ros::WallTime::now();
    bool result;
    if (area_search_) 
      result = astar.makePlan(current_pose_local_.pose, m_pose.pose);
    else
      result = astar.makePlan(current_pose_local_.pose, goal_pose_local_.pose);

    // ros::WallTime end = ros::WallTime::now();

    end = std::chrono::system_clock::now();

    double time1 = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

    area_index = index_x + index_y * N;
    loop_index = cnt % ITER;

    time_array[area_index][loop_index] = time1; 
    result_array[area_index][loop_index] = result;

    // ROS_INFO("Astar planning: %f [s]", (end - start).toSec());
    if (area_search_) visual_pub_.publish(m_pose);
    else visual_pub_.publish(goal_pose_local_);

    if (result)
    {
      ROS_INFO("Found GOAL!");
      publishWaypoints(astar.getPath(), waypoints_velocity_);
      sum[index_y][index_x] += time1;
    }
    else
    {
      ROS_INFO("Can't find goal...");
      publishStopWaypoints();
    }

    std::cout << "cnt: " << cnt << ", index_x: " << index_x << ", index_y: " << index_y << std::endl;

    // if (index_x == N-1 && loop_index == (ITER - 1)) {
    //   for (int i = 0; i < N; i++) {
    //     fprintf(fp, "%lf,", sum[index_y][i] / (double)ITER);
    //   }
    //   fprintf(fp, "\n");
    //   fflush(fp);
    //   std::cout << "result output." << std::endl;
    //   if (index_y == N-1) {
    //     fclose(fp);
	  //     std::cout << "finished." << std::endl;
	  //     exit(EXIT_SUCCESS);
    //   }
    // }

    if (index_x == N-1 && index_y == N-1 && loop_index == (ITER - 1))
    {
      for (int i = 0; i < ITER; i++)
      {
        std::cout << "writing " << i << "th array.\r";
        for (int j = 0; j < N*N; j++)
        {
          fprintf(fp, "%lf,", time_array[j][i]);
        }
        for (int j = 0; j < N*N; j++)
        {
          fprintf(fp, "%d,", result_array[j][i]);
        }
        fprintf(fp, "\n");
        fflush(fp);
      }

      std::cout << std::endl << "finish writing." << std::endl;
      fflush(fp);
      fclose(fp);

      exit(EXIT_SUCCESS);
    }

    cnt++;
    index_x = cnt / ITER;
    index_y = index_x / N;
    index_x = index_x % N;

    astar.reset();
    rate.sleep();
  }
}

void AstarNavi::publishWaypoints(const nav_msgs::Path& path, const double& velocity)
{
  autoware_msgs::Lane lane;
  lane.header.frame_id = "map";
  lane.header.stamp = path.header.stamp;
  lane.increment = 0;

  for (const auto& pose : path.poses)
  {
    autoware_msgs::Waypoint wp;
    wp.pose.header = lane.header;
    wp.pose.pose = transformPose(pose.pose, getTransform(lane.header.frame_id, pose.header.frame_id));
    wp.pose.pose.position.z = current_pose_global_.pose.position.z;  // height = const
    wp.twist.twist.linear.x = velocity / 3.6;  // velocity = const
    lane.waypoints.push_back(wp);
  }

  autoware_msgs::LaneArray lane_array;
  lane_array.lanes.push_back(lane);
  lane_pub_.publish(lane_array);
}

void AstarNavi::publishStopWaypoints()
{
  nav_msgs::Path path;
  geometry_msgs::PoseStamped pose;  // stop path
  pose.header.stamp = ros::Time::now();
  pose.header.frame_id = current_pose_global_.header.frame_id;
  pose.pose = current_pose_global_.pose;
  path.poses.push_back(pose);
  publishWaypoints(path, 0.0);
}
