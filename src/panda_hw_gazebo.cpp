#include <panda_hw_gazebo/panda_hw_gazebo.h>
#include <urdf/model.h>

#include <kdl/frames.hpp>
#include <kdl_parser/kdl_parser.hpp>
#include <tf_conversions/tf_kdl.h>

#include <thread>

namespace
{

double clamp(const double val, const double min_val, const double max_val)
{
  return std::min(std::max(val, min_val), max_val);
}

}

namespace panda_hw_gazebo
{


bool PandaRobotHWSim::initSim(
  const std::string& robot_namespace,
  ros::NodeHandle model_nh,
  gazebo::physics::ModelPtr parent_model,
  const urdf::Model *const urdf_model,
  std::vector<transmission_interface::TransmissionInfo> transmissions)
{
  gazebo_ros_control::DefaultRobotHWSim::initSim(robot_namespace,model_nh,parent_model,urdf_model,transmissions);

    //Register model handle & interface
    franka_hw::FrankaModelHandle model_handle("panda_model", jacobian_, gravity_, coriolis_, mass_matrix_); // TODO arm id
    franka_model_interface_.registerHandle(model_handle);
    registerInterface(&franka_model_interface_);
    ROS_INFO("Registered model interface");

    //Register state handle & interface
    franka_hw::FrankaStateHandle state_handle("panda_robot", robot_state_); // TODO arm id, model_nh za ros zadeve
    franka_state_interface_.registerHandle(state_handle);  
    registerInterface(&franka_state_interface_);
    ROS_INFO("Registered state interface");

    // State puiblisher
   double publish_rate(100.0);
   if (!model_nh.getParam("publish_rate", publish_rate)) {
     ROS_INFO_STREAM("panda_hw_gazebo: publish_rate not found. Defaulting to "
                    << publish_rate);
   }

  // Realtime publisher
  rate_trigger_ = franka_hw::TriggerRate(publish_rate);
  publisher_franka_states_.init(model_nh, "/franka_state_controller/franka_states", 1); // TODO use remap

  // Turn off some resources heavy calculations
  model_nh.getParam("mass_calculation_needed", mass_calculation_needed);
  model_nh.getParam("coriolis_calculation_needed", coriolis_calculation_needed);
  model_nh.getParam("gravity_calculation_needed", gravity_calculation_needed);
  model_nh.getParam("robot_state_needed", robot_state_needed);

  initKDL(model_nh);

  return true;
}

bool PandaRobotHWSim::initKDL(const ros::NodeHandle& nh) {
 std::string urdf_xml;
  ROS_DEBUG_NAMED("kinematics", "Reading xml file from parameter server");
  if (!nh.getParam("robot_description", urdf_xml))
  {
    ROS_FATAL_NAMED("kinematics",
        "Could not load the xml from parameter server: %s", urdf_xml.c_str());
    return false;
  }
  
  root_name_ = "panda_link0";
  if (!nh.getParam("root_name", root_name_))
  {
    ROS_WARN_STREAM_NAMED("kinematics",
        "No root name for Kinematic Chain found on parameter server, using " << root_name_);
  }

  tip_name_ = "panda_link7";
  if (!nh.getParam("tip_name", tip_name_))
  {
    ROS_WARN_STREAM_NAMED("kinematics",
        "No tip name for Kinematic Chain found on parameter server, using " << tip_name_);
  }
 
  robot_model_.initString(urdf_xml);
  if (!kdl_parser::treeFromUrdfModel(robot_model_, tree_))
  {
    ROS_FATAL_NAMED("kinematics",
        "Failed to extract kdl tree from xml robot description.");
    return false;
  }
  
  // Init solvers
  if (!createKinematicChain(tip_name_))
  {
    return false;
  }
  
  return true;
}


bool PandaRobotHWSim::createKinematicChain(std::string tip_name) 
{
  if(kinematic_chain_map_.find(tip_name) != kinematic_chain_map_.end())
  {
    ROS_WARN_NAMED("kinematics", "Kinematic chain from %s to %s already exists!",
                   root_name_.c_str(), tip_name.c_str());
    return false;
  }
  Kinematics kin;
  if (!tree_.getChain(root_name_, tip_name, kin.chain))
  {
    ROS_ERROR_NAMED("kinematics", "Couldn't find chain %s to %s",
                    root_name_.c_str(), tip_name.c_str());
    return false;
  }
  // Save off Joint Names
  for (size_t seg_idx = 0; seg_idx < kin.chain.getNrOfSegments(); seg_idx++)
  {
    const auto& jnt = kin.chain.getSegment(seg_idx).getJoint();
    if (jnt.getTypeName() == "None" || jnt.getTypeName() == "Unknown" || jnt.getTypeName() == "Fixed")
      continue;
    kin.joint_names.push_back(kin.chain.getSegment(seg_idx).getJoint().getName());
  }
  
  kdl_ = new KDLMethods;
  kdl_->initialise(kin.chain);
  kinematic_chain_map_.insert(std::make_pair(tip_name, std::move(kin)));
  return true;
}

void PandaRobotHWSim::readSim(ros::Time time, ros::Duration period)
{
  gazebo_ros_control::DefaultRobotHWSim::readSim(time,period);

    num_jnts = kinematic_chain_map_[tip_name_].chain.getNrOfJoints();
    KDL::JntArray jnt_pos(num_jnts), jnt_vel(num_jnts), jnt_eff(num_jnts);

    updateRobotStateJoints(jnt_pos, jnt_vel, jnt_eff);

    // Multi-Threading leads to problems when reading out at same time. 
    // std::vector<std::thread> threads;
    // if (robot_state_needed) threads.push_back(std::thread(&PandaRobotHWSim::updateJacobian,this,jnt_pos, jnt_vel));
    // if (coriolis_calculation_needed) threads.push_back(std::thread(&PandaRobotHWSim::updateCoriolisVec,this,jnt_pos, jnt_vel));
    // if (gravity_calculation_needed) threads.push_back(std::thread(&PandaRobotHWSim::updateGravityVec,this,jnt_pos));
    // if (mass_calculation_needed) threads.push_back(std::thread(&PandaRobotHWSim::updateMassMatrixKDL,this,jnt_pos));
    // if (robot_state_needed) threads.push_back(std::thread(&PandaRobotHWSim::publishRobotStateMsg,this)); 
    // for (auto &th : threads) th.join();
    
  if (robot_state_needed) updateJacobian(jnt_pos,jnt_vel);
  if (coriolis_calculation_needed) updateCoriolisVec(jnt_pos,jnt_vel);
  if (gravity_calculation_needed) updateGravityVec(jnt_pos);
  if (mass_calculation_needed) updateMassMatrixKDL(jnt_pos);
  if (robot_state_needed) publishRobotStateMsg();
}


void PandaRobotHWSim::updateRobotStateJoints(KDL::JntArray& jnt_pos, KDL::JntArray& jnt_vel, KDL::JntArray& jnt_eff)
{

  KDL::SetToZero(jnt_pos);
  KDL::SetToZero(jnt_vel);
  KDL::SetToZero(jnt_eff);
  
  for(size_t jnt_idx = 0; jnt_idx < num_jnts; jnt_idx++)
  {
      // jointStateToKDL
      jnt_pos(jnt_idx) = joint_position_[jnt_idx];
      jnt_vel(jnt_idx) = joint_velocity_[jnt_idx];
      jnt_eff(jnt_idx) = joint_effort_[jnt_idx];
      // joint state to robot_state
      robot_state_.q[jnt_idx] =  joint_position_[jnt_idx];
      robot_state_.dq[jnt_idx] = joint_velocity_[jnt_idx];
      robot_state_.tau_J[jnt_idx] = joint_effort_[jnt_idx];
  }


   for(const auto& chain : kinematic_chain_map_)
    {

      if(chain.first == tip_name_)
      {
        geometry_msgs::Pose pose;
        computePositionFK(chain.second, jnt_pos, pose);

        Eigen::Quaterniond q(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
        Eigen::Matrix3d m = q.toRotationMatrix();
        Eigen::Vector3d t(pose.position.x, pose.position.y, pose.position.z);

        Eigen::Matrix4d Trans; 
        Trans.setIdentity();   // Set to Identity to make bottom row of Matrix 0,0,0,1
        Trans.block<3,3>(0,0) = m;
        Trans.block<3,1>(0,3) = t;

        Eigen::Map<Eigen::RowVectorXd> flattened_mat(Trans.data(), Trans.size());

        std::vector<double> vec(flattened_mat.data(), flattened_mat.data() + flattened_mat.size());
        std::copy_n(vec.begin(), 16, robot_state_.O_T_EE.begin());

      }
    }
}

void PandaRobotHWSim::updateCoriolisVec(const KDL::JntArray& jnt_pos, const KDL::JntArray& jnt_vel)
{
    KDL::JntArray C(num_jnts);
    kdl_->JntToCoriolis(jnt_pos,jnt_vel,C);

    for (size_t jnt_idx = 0; jnt_idx < num_jnts; jnt_idx++)
    {
      coriolis_[jnt_idx] = C(jnt_idx,0);
    }
} 

void PandaRobotHWSim::updateGravityVec(const KDL::JntArray& jnt_pos)
{
    KDL::JntArray G(num_jnts); 
    kdl_->JntToGravity(jnt_pos,G);

    for (size_t jnt_idx = 0; jnt_idx < num_jnts; jnt_idx++)
    {
      gravity_[jnt_idx] = G(jnt_idx,0);
    }
} 

// very resources hungry
void PandaRobotHWSim::updateMassMatrixKDL(const KDL::JntArray& jnt_pos)
{
    KDL::JntSpaceInertiaMatrix H(num_jnts); 
    kdl_->JntToMass(jnt_pos,H); 
    Eigen::Map<Eigen::RowVectorXd> H_vec(H.data.data(), H.data.size());
    for (size_t i = 0; i < mass_matrix_.size(); i++)
    {
      mass_matrix_[i] = H_vec(i);
    }
}

// even slower
void PandaRobotHWSim::updateMassMatrixModel()
{
    Eigen::Matrix<double,7,1> q(robot_state_.q.data());
    Eigen::Matrix7d mass_matrix = MassMatrix(q);
    Eigen::Map<Eigen::Matrix7d>(mass_matrix_.data(),7,7) = mass_matrix;
}

//TODO: check RBDL, pinocchio 


void PandaRobotHWSim::updateJacobian(const KDL::JntArray& jnt_pos, const KDL::JntArray& jnt_vel)
{
    KDL::Jacobian J;
    J.resize(num_jnts);
    kdl_->JacobianJntToJac(jnt_pos, J);

    Eigen::Matrix<double, 6, 1> ee_vel = J.data * jnt_vel.data;
    Eigen::Map<Eigen::RowVectorXd> J_vec(J.data.data(), J.data.size());

    for (size_t i = 0; i < jacobian_.size(); i++) {
      jacobian_[i] = J_vec(i);
    }

}

bool PandaRobotHWSim::computePositionFK(const Kinematics& kin,
                                               const KDL::JntArray& jnt_pos,
                                               geometry_msgs::Pose& result)
{
  KDL::Frame p_out;
  if (kdl_->PosFKJntToCart(jnt_pos, p_out, kin.chain.getNrOfSegments()) < 0)
  {
    return false;
  }
  tf::poseKDLToMsg(p_out, result);
  return true;
}

void PandaRobotHWSim::publishRobotStateMsg() {
  // void FrankaStateController::publishFrankaStates(const ros::Time& time) {

  if (rate_trigger_() && publisher_franka_states_.trylock()) {

    for (size_t i = 0; i < robot_state_.q.size(); i++) {
      publisher_franka_states_.msg_.q[i] = robot_state_.q[i];
      //publisher_franka_states_.msg_.q_d[i] = robot_state_.q_d[i];
      publisher_franka_states_.msg_.dq[i] = robot_state_.dq[i];
      //publisher_franka_states_.msg_.dq_d[i] = robot_state_.dq_d[i];
      //publisher_franka_states_.msg_.ddq_d[i] = robot_state_.ddq_d[i];
      publisher_franka_states_.msg_.tau_J[i] = robot_state_.tau_J[i];
      //publisher_franka_states_.msg_.dtau_J[i] = robot_state_.dtau_J[i];
      //publisher_franka_states_.msg_.tau_J_d[i] = robot_state_.tau_J_d[i];
      //publisher_franka_states_.msg_.theta[i] = robot_state_.theta[i];
      //publisher_franka_states_.msg_.dtheta[i] = robot_state_.dtheta[i];
      //publisher_franka_states_.msg_.joint_collision[i] = robot_state_.joint_collision[i];
      //publisher_franka_states_.msg_.joint_contact[i] = robot_state_.joint_contact[i];
      //publisher_franka_states_.msg_.tau_ext_hat_filtered[i] = robot_state_.tau_ext_hat_filtered[i];
    }

    for (size_t i = 0; i < robot_state_.O_T_EE.size(); i++)
    {
      publisher_franka_states_.msg_.O_T_EE[i] = robot_state_.O_T_EE[i];
    }
    
    publisher_franka_states_.unlockAndPublish();
  }
  
}


}

PLUGINLIB_EXPORT_CLASS(panda_hw_gazebo::PandaRobotHWSim, gazebo_ros_control::RobotHWSim)
