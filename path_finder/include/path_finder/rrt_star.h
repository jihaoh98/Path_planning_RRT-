/*
Copyright (C) 2022 Hongkai Ye (kyle_yeh@163.com)
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
*/
#ifndef RRT_STAR_H
#define RRT_STAR_H

#include "occ_grid/occ_map.h"
#include "visualization/visualization.hpp"
#include "sampler.h"
#include "node.h"
#include "kdtree.h"

#include <ros/ros.h>
#include <utility>
#include <queue>

namespace path_plan
{
  class RRTStar
  {
  public:
    RRTStar(){};
    RRTStar(const ros::NodeHandle &nh, const env::OccMap::Ptr &mapPtr) : nh_(nh), map_ptr_(mapPtr)
    {
      nh_.param("RRT_Star/steer_length", steer_length_, 0.0);
      nh_.param("RRT_Star/search_radius", search_radius_, 0.0);
      nh_.param("RRT_Star/search_time", search_time_, 0.0);
      nh_.param("RRT_Star/max_tree_node_nums", max_tree_node_nums_, 0);
      nh_.param("RRT_Star/use_informed_sampling", use_informed_sampling_, true);

      ROS_WARN_STREAM("[RRT*] param: steer_length: " << steer_length_);
      ROS_WARN_STREAM("[RRT*] param: search_radius: " << search_radius_);
      ROS_WARN_STREAM("[RRT*] param: search_time: " << search_time_);
      ROS_WARN_STREAM("[RRT*] param: max_tree_node_nums: " << max_tree_node_nums_);
      ROS_WARN_STREAM("[RRT*] param: use_informed_sampling: " << use_informed_sampling_);

      // set the range of sampling
      sampler_.setSamplingRange(mapPtr->getOrigin(), mapPtr->getMapSize());

      valid_tree_node_nums_ = 0;
      nodes_pool_.resize(max_tree_node_nums_);
      for (int i = 0; i < max_tree_node_nums_; ++i)
      {
        nodes_pool_[i] = new TreeNode;
      }
    }
    ~RRTStar(){};

    bool plan(const Eigen::Vector3d &s, const Eigen::Vector3d &g)
    {
      // reset all the variable
      reset();
      if (!map_ptr_->isStateValid(s))
      {
        ROS_ERROR("[RRT*]: Start pos collide or out of bound");
        return false;
      }
      if (!map_ptr_->isStateValid(g))
      {
        ROS_ERROR("[RRT*]: Goal pos collide or out of bound");
        return false;
      }
      /* construct start and goal nodes */
      start_node_ = nodes_pool_[1];
      start_node_->x = s;
      start_node_->cost_from_start = 0.0;

      goal_node_ = nodes_pool_[0];
      goal_node_->x = g;
      goal_node_->cost_from_start = DBL_MAX; // important
      valid_tree_node_nums_ = 2;             // put start and goal in the tree

      ROS_INFO("[RRT*]: RRT starts planning a path");
      
      // !------------
      sampler_.reset(); // firstly don't use the informed sampling, only in find the first solution case
      if (use_informed_sampling_)
      {
        calInformedSet(10000000000.0, s, g, scale_, trans_, rot_);
        sampler_.setInformedTransRot(trans_, rot_);
      }
      // !----------------
      return rrt_star(s, g);
    }

    vector<Eigen::Vector3d> getPath()
    {
      return final_path_;
    }

    vector<vector<Eigen::Vector3d>> getAllPaths()
    {
      return path_list_;
    }

    vector<std::pair<double, double>> getSolutions()
    {
      return solution_cost_time_pair_list_;
    }

    // only for edge and the point in the search process
    void setVisualizer(const std::shared_ptr<visualization::Visualization> &visPtr)
    {
      vis_ptr_ = visPtr;
    };

  private:
    // nodehandle params
    ros::NodeHandle nh_;

    // Biased sampling
    BiasSampler sampler_;

    // for informed sampling
    Eigen::Vector3d trans_, scale_;
    Eigen::Matrix3d rot_;
    bool use_informed_sampling_;

    double steer_length_;
    double search_radius_;
    double search_time_;

    int max_tree_node_nums_;
    int valid_tree_node_nums_;

    double first_path_use_time_;
    double final_path_use_time_;

    std::vector<TreeNode *> nodes_pool_;
    TreeNode *start_node_;
    TreeNode *goal_node_;

    vector<Eigen::Vector3d> final_path_;
    vector<vector<Eigen::Vector3d>> path_list_;
    vector<std::pair<double, double>> solution_cost_time_pair_list_;  // 存放终点到起点的dist以及程序已经运行的时间

    // environment
    env::OccMap::Ptr map_ptr_;
    std::shared_ptr<visualization::Visualization> vis_ptr_;

    void reset()
    {
      final_path_.clear();
      path_list_.clear();
      solution_cost_time_pair_list_.clear();
      for (int i = 0; i < valid_tree_node_nums_; i++)
      {
        nodes_pool_[i]->parent = nullptr;
        nodes_pool_[i]->children.clear();
      }
      valid_tree_node_nums_ = 0;
    }

    double calDist(const Eigen::Vector3d &p1, const Eigen::Vector3d &p2)
    {
      return (p1 - p2).norm();
    }

    Eigen::Vector3d steer(const Eigen::Vector3d &nearest_node_p, const Eigen::Vector3d &rand_node_p, double len)
    {
      Eigen::Vector3d diff_vec = rand_node_p - nearest_node_p;
      double dist = diff_vec.norm();
      if (diff_vec.norm() <= len)
        return rand_node_p;
      else
        return nearest_node_p + diff_vec * len / dist;  // len单步长度, dist是实际的距离
    }

    RRTNode3DPtr addTreeNode(RRTNode3DPtr &parent, const Eigen::Vector3d &state,
                             const double &cost_from_start, const double &cost_from_parent)
    {
      RRTNode3DPtr new_node_ptr = nodes_pool_[valid_tree_node_nums_];
      valid_tree_node_nums_++; // 树的节点变加一,因为实现了随机采样扩展
      new_node_ptr->parent = parent;
      parent->children.push_back(new_node_ptr);
      new_node_ptr->x = state;
      new_node_ptr->cost_from_start = cost_from_start;
      new_node_ptr->cost_from_parent = cost_from_parent;
      return new_node_ptr;
    }

    void changeNodeParent(RRTNode3DPtr &node, RRTNode3DPtr &parent, const double &cost_from_parent)
    {
      if(node->parent) // 指针是否为空
        node->parent->children.remove(node); //DON'T FORGET THIS, remove it from its parent's children list
      
      node->parent = parent;
      node->cost_from_parent = cost_from_parent;
      node->cost_from_start = parent->cost_from_start + cost_from_parent;
      parent->children.push_back(node);

      // for all its descendants, change the cost_from_start and tau_from_start;
      RRTNode3DPtr descendant(node);
      std::queue<RRTNode3DPtr> Q;
      Q.push(descendant);
      while (!Q.empty())
      {
        descendant = Q.front();
        Q.pop();
        for (const auto &leafptr : descendant->children)
        {
          leafptr->cost_from_start = leafptr->cost_from_parent + descendant->cost_from_start;
          Q.push(leafptr);
        }
      }
    }

    // Recursive acquisition path
    void fillPath(const RRTNode3DPtr &n, vector<Eigen::Vector3d> &path)
    {
      path.clear();
      RRTNode3DPtr node_ptr = n;
      while (node_ptr->parent)
      {
        path.push_back(node_ptr->x);
        node_ptr = node_ptr->parent;
      }
      path.push_back(start_node_->x);
      std::reverse(std::begin(path), std::end(path));
    }


    bool rrt_star(const Eigen::Vector3d &s, const Eigen::Vector3d &g)
    {
      ros::Time rrt_start_time = ros::Time::now();
      bool goal_found = false;
      double c_square = (g - s).squaredNorm() / 4.0; // 相当于不开平方

      /* kd tree init */
      kdtree *kd_tree = kd_create(3);
      //Add start and goal nodes to kd tree
      kd_insert3(kd_tree, start_node_->x[0], start_node_->x[1], start_node_->x[2], start_node_);

      /* main loop */
      // satisfy the time and the number of nodes
      for(int idx = 0; (ros::Time::now() - rrt_start_time).toSec() < search_time_ && valid_tree_node_nums_ < max_tree_node_nums_; ++idx)
      {
        /* biased random sampling */
        Eigen::Vector3d x_rand;
        sampler_.samplingOnce(x_rand);

        // is valid
        if (!map_ptr_->isStateValid(x_rand))
        {
          continue;
        }

        //  get the nearest for x_rand
        struct kdres *p_nearest = kd_nearest3(kd_tree, x_rand[0], x_rand[1], x_rand[2]);
        if(p_nearest == nullptr)
        {
          ROS_ERROR("nearest query error");
          continue;
        }
        RRTNode3DPtr nearest_node = (RRTNode3DPtr)kd_res_item_data(p_nearest);
        kd_res_free(p_nearest); // free this

        // get the new expand node
        Eigen::Vector3d x_new = steer(nearest_node->x, x_rand, steer_length_);
        if (!map_ptr_->isSegmentValid(nearest_node->x, x_new))
        {
          continue;
        }

        /* 1. find parent */
        /* kd_tree bounds search for parent */
        vector<RRTNode3DPtr> neighbour_nodes; // store all the neighbor nodes

        struct kdres *nbr_set;
        nbr_set = kd_nearest_range3(kd_tree, x_new[0], x_new[1], x_new[2], search_radius_);
        if (nbr_set == nullptr)
        {
          ROS_ERROR("bkwd kd range query error");
          break;
        }
        while (!kd_res_end(nbr_set))
        {
          RRTNode3DPtr curr_node = (RRTNode3DPtr)kd_res_item_data(nbr_set);
          neighbour_nodes.emplace_back(curr_node);
          // store range query result so that we dont need to query again for rewire;
          kd_res_next(nbr_set); //go to next in kd tree range query result
        }
        kd_res_free(nbr_set); //reset kd tree range query

        /* choose parent from kd tree range query result*/
        double dist2nearest = calDist(nearest_node->x, x_new);
        double min_dist_from_start(nearest_node->cost_from_start + dist2nearest);
        double cost_from_p(dist2nearest);   // cost from parent
        RRTNode3DPtr min_node(nearest_node); //set the nearest_node as the default parent

        // TODO Choose a parent according to potential cost-from-start values
        // ! Hints:
        // ! 1. Use map_ptr_->isSegmentValid(p1, p2) to check line edge validity;
        // ! 2. Default parent is [nearest_node];
        // ! 3. Store your chosen parent-node-pointer, the according cost-from-parent and cost-from-start
        // !     in [min_node], [cost_from_p], and [min_dist_from_start] respectively;
        // ! 4. [Optional] You can sort the potential parents first in increasing order by cost-from-start value;
        // ! 5. [Optional] You can store the collison-checking results for later usage in the Rewire procedure.
        // ! Implement your own code inside the following loop
        for (auto &curr_node : neighbour_nodes)
        {
          double dist2current = calDist(curr_node->x, x_new);
          double current_dist_from_start = curr_node->cost_from_start + dist2current;
          if (current_dist_from_start < min_dist_from_start)
          {
            if (map_ptr_->isSegmentValid(curr_node->x, x_new))
            {
              min_node = curr_node;
              cost_from_p = dist2current;  //cost from parent
              min_dist_from_start = current_dist_from_start;
            }
          }
        }

        /* parent found within radius, then add a node to rrt and kd_tree */
        /* 1.1 add the randomly sampled node to rrt_tree */
        RRTNode3DPtr new_node(nullptr);
        new_node = addTreeNode(min_node, x_new, min_dist_from_start, cost_from_p);

        /* 1.2 add the randomly sampled node to kd_tree */
        kd_insert3(kd_tree, x_new[0], x_new[1], x_new[2], new_node);
        // end of find parent

        /* 2. try to connect to goal if possible */
        double dist_to_goal = calDist(x_new, goal_node_->x);
        // less than one range
        if (dist_to_goal <= search_radius_)
        {
          // can this node connect the end point directly
          bool is_connected2goal = map_ptr_->isSegmentValid(x_new, goal_node_->x);

          // this test can be omitted if sample-rejction is applied
          // first the cost from start of the goal node is very great
          // we can update the goal node if we can find a better solution
          bool is_better_path = goal_node_->cost_from_start > dist_to_goal + new_node->cost_from_start;
          if (is_connected2goal && is_better_path)
          {
            // The end point is not found by default
            if (!goal_found)
            {
              first_path_use_time_ = (ros::Time::now() - rrt_start_time).toSec();
            }
            goal_found = true;
            changeNodeParent(goal_node_, new_node, dist_to_goal);

            // store the path
            vector<Eigen::Vector3d> curr_best_path;
            fillPath(goal_node_, curr_best_path);
            path_list_.emplace_back(curr_best_path);

            // store the cost and the total time to now
            solution_cost_time_pair_list_.emplace_back(goal_node_->cost_from_start, (ros::Time::now() - rrt_start_time).toSec());

            // ----------informed RRT*
            if (use_informed_sampling_)
            {
              scale_[0] = goal_node_->cost_from_start / 2.0;
              scale_[1] = sqrt(scale_[0] * scale_[0] - c_square);
              scale_[2] = scale_[1];
              sampler_.setInformedSacling(scale_); // set true and the scale begin informed rrt*

              std::vector<visualization::ELLIPSOID> ellps;
              ellps.emplace_back(trans_, scale_, rot_);
              vis_ptr_->visualize_ellipsoids(ellps, "informed_set", visualization::yellow, 0.2);
            }
            // ! ---------------------
          }
        }

        /* 3.rewire */
        // TODO Rewire according to potential cost-from-start values
        // ! Hints:
        // !  1. Use map_ptr_->isSegmentValid(p1, p2) to check line edge validity;
        // !  2. Use changeNodeParent(node, parent, cost_from_parent) to change a node's parent;
        // !  3. the variable [new_node] is the pointer of X_new;
        // !  4. [Optional] You can test whether the node is promising before checking edge collison.
        // ! Implement your own code between the dash lines [--------------] in the following loop
        for (auto &curr_node : neighbour_nodes)
        {
          double best_cost_before_rewire = goal_node_->cost_from_start;
          // ! -------------------------------------
          double dist_to_child = calDist(new_node->x, curr_node->x);
          double current_dist_from_new = new_node->cost_from_start + dist_to_child;
          
          // add in order to reduce unnecessary Rewire (learn from hkye)
          // but the result is not very fascinating
          // heuristic as Euclidean
          double promising_cost  = current_dist_from_new + calDist(curr_node->x, goal_node_->x);
          if (current_dist_from_new < curr_node->cost_from_start && promising_cost < best_cost_before_rewire)
          {
            if (map_ptr_->isSegmentValid(new_node->x, curr_node->x))
            {
              changeNodeParent(curr_node, new_node, calDist(new_node->x, curr_node->x));

              // if could get a better solution
              // after the changeNodeParent, the goal_node_'s cost from start may change
              // we use heuristic to estimate, but heuristic is less than the actual value
              if (best_cost_before_rewire > goal_node_->cost_from_start)
              {
                vector<Eigen::Vector3d> curr_best_path;
                fillPath(goal_node_, curr_best_path);
                path_list_.emplace_back(curr_best_path);
                solution_cost_time_pair_list_.emplace_back(goal_node_->cost_from_start, (ros::Time::now() - rrt_start_time).toSec());

                //informed RRT*
                if (use_informed_sampling_)
                {
                  scale_[0] = goal_node_->cost_from_start / 2.0;
                  scale_[1] = sqrt(scale_[0] * scale_[0] - c_square);
                  scale_[2] = scale_[1];
                  sampler_.setInformedSacling(scale_); // set true(begin informed rrt*) and set scale

                  std::vector<visualization::ELLIPSOID> ellps;
                  ellps.emplace_back(trans_, scale_, rot_);
                  vis_ptr_->visualize_ellipsoids(ellps, "informed_set", visualization::yellow, 0.2);
                }
              }
            }
          }
          // ! -------------------------------------
        }
        /* end of rewire */

        // !-------------
        /* vector<Eigen::Vector3d> vertice;
        vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> edges;
        sampleWholeTree(start_node_, vertice, edges);

        std::vector<visualization::BALL> balls;
        balls.reserve(vertice.size());
        visualization::BALL node_p;
        node_p.radius = 0.06;
        for (size_t i = 0; i < vertice.size(); ++i)
        {
          node_p.center = vertice[i];
          balls.push_back(node_p);
        }

        // 可视化采样的节点和连接采样点之间的路径
        vis_ptr_->visualize_balls(balls, "tree_vertice", visualization::Color::blue, 1.0);
        vis_ptr_->visualize_pairline(edges, "tree_edges", visualization::Color::red, 0.04); */
        // !-------------

      }
      /* end of sample once */

      vector<Eigen::Vector3d> vertice;
      vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> edges;
      sampleWholeTree(start_node_, vertice, edges);

      // balls display all the nodes in search process
      std::vector<visualization::BALL> balls;
      balls.reserve(vertice.size());

      //add every node to balls
      visualization::BALL node_p;
      node_p.radius = 0.06;
      for (size_t i = 0; i < vertice.size(); ++i)
      {
        node_p.center = vertice[i];
        balls.push_back(node_p);
      }

      // 可视化采样的节点和连接采样点之间的路径
      vis_ptr_->visualize_balls(balls, "tree_vertice", visualization::Color::blue, 1.0);
      vis_ptr_->visualize_pairline(edges, "tree_edges", visualization::Color::red, 0.04);

      std::vector<visualization::ELLIPSOID> ellps;
      ellps.emplace_back(trans_, scale_, rot_);
      vis_ptr_->visualize_ellipsoids(ellps, "informed_set", visualization::yellow, 0.2);

      if (goal_found)
      {
        final_path_use_time_ = (ros::Time::now() - rrt_start_time).toSec();
        fillPath(goal_node_, final_path_); // final_path_ store the final path
        ROS_INFO_STREAM("[RRT*]: first path length: " << solution_cost_time_pair_list_.front().first << ", use_time: " << first_path_use_time_);
      }
      else if (valid_tree_node_nums_ == max_tree_node_nums_)
      {
        ROS_ERROR_STREAM("[RRT*]: NOT CONNECTED TO GOAL after " << max_tree_node_nums_ << " nodes added to rrt-tree");
      }
      else
      {
        ROS_ERROR_STREAM("[RRT*]: NOT CONNECTED TO GOAL after " << (ros::Time::now() - rrt_start_time).toSec() << " seconds");
      }
      return goal_found;
    }

    // vertice store the coordinate of all points
    // edges store the coordinate corresponding to two points
    void sampleWholeTree(const RRTNode3DPtr &root, vector<Eigen::Vector3d> &vertice, vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> &edges)
    {
      if (root == nullptr)
        return;

      // whatever dfs or bfs
      RRTNode3DPtr node = root;
      std::queue<RRTNode3DPtr> Q;
      Q.push(node);
      while (!Q.empty())
      {
        // get and pop
        node = Q.front();
        Q.pop();
        for (const auto &leafptr : node->children)
        {
          vertice.push_back(leafptr->x);
          edges.emplace_back(std::make_pair(node->x, leafptr->x));
          Q.push(leafptr);
        }
      }
    }

    void calInformedSet(double a2, const Eigen::Vector3d &foci1, const Eigen::Vector3d &foci2,
                        Eigen::Vector3d &scale, Eigen::Vector3d &trans, Eigen::Matrix3d &rot)
    {
      trans = (foci1 + foci2) / 2.0;

      scale[0] = a2 / 2.0;
      Eigen::Vector3d diff(foci2 - foci1);
      double c_square = diff.squaredNorm() / 4.0;
      scale[1] = sqrt(scale[0] * scale[0] - c_square);
      scale[2] = scale[1];

      rot.col(0) = diff.normalized();
      diff[2] = 0.0;
      // project to the x-y plane and then rotate 90 degree;
      rot.col(1) = Eigen::AngleAxisd(0.5 * M_PI, Eigen::Vector3d::UnitZ()) * diff.normalized(); 
      rot.col(2) = rot.col(0).cross(rot.col(1));
    }
  };

} // namespace path_plan
#endif