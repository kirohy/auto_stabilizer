#include "AutoStabilizer.h"
#include <cnoid/BodyLoader>
#include <cnoid/ForceSensor>
#include <cnoid/RateGyroSensor>
#include <cnoid/ValueTree>
#include <cnoid/EigenUtil>
#include "MathUtil.h"
#include "CnoidBodyUtil.h"
#include <limits>

#define DEBUG false

static const char* AutoStabilizer_spec[] = {
  "implementation_id", "AutoStabilizer",
  "type_name",         "AutoStabilizer",
  "description",       "AutoStabilizer component",
  "version",           "0.0",
  "vendor",            "Naoki-Hiraoka",
  "category",          "example",
  "activity_type",     "DataFlowComponent",
  "max_instance",      "10",
  "language",          "C++",
  "lang_type",         "compile",
  ""
};

AutoStabilizer::Ports::Ports() :
  m_qRefIn_("qRef", m_qRef_),
  m_refTauIn_("refTauIn", m_refTau_),
  m_refBasePosIn_("refBasePosIn", m_refBasePos_),
  m_refBaseRpyIn_("refBaseRpyIn", m_refBaseRpy_),
  m_qActIn_("qAct", m_qAct_),
  m_dqActIn_("dqAct", m_dqAct_),
  m_actImuIn_("actImuIn", m_actImu_),

  m_qOut_("q", m_q_),
  m_genTauOut_("genTauOut", m_genTau_),
  m_genBasePoseOut_("genBasePoseOut", m_genBasePose_),
  m_genBaseTformOut_("genBaseTformOut", m_genBaseTform_),

  m_genBasePosOut_("genBasePosOut", m_genBasePos_),
  m_genBaseRpyOut_("genBaseRpyOut", m_genBaseRpy_),

  m_AutoStabilizerServicePort_("AutoStabilizerService") {
}

AutoStabilizer::AutoStabilizer(RTC::Manager* manager) : RTC::DataFlowComponentBase(manager),
  ports_(),
  debugLevel_(0)
{
  this->ports_.m_service0_.setComp(this);
}

RTC::ReturnCode_t AutoStabilizer::onInitialize(){

  // add ports
  this->addInPort("qRef", this->ports_.m_qRefIn_);
  this->addInPort("refTauIn", this->ports_.m_refTauIn_);
  this->addInPort("refBasePosIn", this->ports_.m_refBasePosIn_);
  this->addInPort("refBaseRpyIn", this->ports_.m_refBaseRpyIn_);
  this->addInPort("qAct", this->ports_.m_qActIn_);
  this->addInPort("dqAct", this->ports_.m_dqActIn_);
  this->addInPort("actImuIn", this->ports_.m_actImuIn_);
  this->addOutPort("q", this->ports_.m_qOut_);
  this->addOutPort("genTauOut", this->ports_.m_genTauOut_);
  this->addOutPort("genBasePoseOut", this->ports_.m_genBasePoseOut_);
  this->addOutPort("genBaseTformOut", this->ports_.m_genBaseTformOut_);
  this->addOutPort("genBasePosOut", this->ports_.m_genBasePosOut_);
  this->addOutPort("genBaseRpyOut", this->ports_.m_genBaseRpyOut_);
  this->ports_.m_AutoStabilizerServicePort_.registerProvider("service0", "AutoStabilizerService", this->ports_.m_service0_);
  this->addPort(this->ports_.m_AutoStabilizerServicePort_);

  {
    // load dt
    std::string buf; this->getProperty("dt", buf);
    this->dt_ = std::stod(buf);
    if(this->dt_ <= 0.0){
      this->getProperty("exec_cxt.periodic.rate", buf);
      double rate = std::stod(buf);
      if(rate > 0.0){
        this->dt_ = 1.0/rate;
      }else{
        std::cerr << "\x1b[31m[" << this->m_profile.instance_name << "] " << "dt is invalid" << "\x1b[39m" << std::endl;
        return RTC::RTC_ERROR;
      }
    }
  }

  {
    // load robot model
    cnoid::BodyLoader bodyLoader;
    std::string fileName; this->getProperty("model", fileName);
    if (fileName.find("file://") == 0) fileName.erase(0, strlen("file://"));
    cnoid::BodyPtr robot = bodyLoader.load(fileName);
    if(!robot){
      std::cerr << "\x1b[31m[" << this->m_profile.instance_name << "] " << "failed to load model[" << fileName << "]" << "\x1b[39m" << std::endl;
      return RTC::RTC_ERROR;
    }
    this->refRobotRaw_ = robot;
    this->refRobotRaw_->calcForwardKinematics(); this->refRobotRaw_->calcCenterOfMass();
    this->refRobot_ = robot;
    this->refRobot_->calcForwardKinematics(); this->refRobot_->calcCenterOfMass();
    this->actRobotRaw_ = robot->clone();
    this->actRobotRaw_->calcForwardKinematics(); this->actRobotRaw_->calcCenterOfMass();
    this->actRobot_ = robot->clone();
    this->actRobot_->calcForwardKinematics(); this->actRobot_->calcCenterOfMass();
    this->genRobot_ = robot->clone();
    this->genRobot_->calcForwardKinematics(); this->genRobot_->calcCenterOfMass();
    this->actRobotTqc_ = robot->clone();
    this->actRobotTqc_->calcForwardKinematics(); this->actRobotTqc_->calcCenterOfMass();
  }

  {
    // load end_effector
    cnoid::DeviceList<cnoid::ForceSensor> forceSensors(this->refRobotRaw_->devices());
    std::string endEffectors; this->getProperty("end_effectors", endEffectors);
    std::stringstream ss_endEffectors(endEffectors);
    std::string buf;
    while(std::getline(ss_endEffectors, buf, ',')){
      std::string name;
      std::string parentLink;
      cnoid::Vector3 localp;
      cnoid::Vector3 localaxis;
      double localangle;

      //   name, parentLink, (not used), x, y, z, theta, ax, ay, az
      name = buf;
      if(!std::getline(ss_endEffectors, buf, ',')) break; parentLink = buf;
      if(!std::getline(ss_endEffectors, buf, ',')) break; // not used
      if(!std::getline(ss_endEffectors, buf, ',')) break; localp[0] = std::stod(buf);
      if(!std::getline(ss_endEffectors, buf, ',')) break; localp[1] = std::stod(buf);
      if(!std::getline(ss_endEffectors, buf, ',')) break; localp[2] = std::stod(buf);
      if(!std::getline(ss_endEffectors, buf, ',')) break; localaxis[0] = std::stod(buf);
      if(!std::getline(ss_endEffectors, buf, ',')) break; localaxis[1] = std::stod(buf);
      if(!std::getline(ss_endEffectors, buf, ',')) break; localaxis[2] = std::stod(buf);
      if(!std::getline(ss_endEffectors, buf, ',')) break; localangle = std::stod(buf);

      // check validity
      name.erase(std::remove(name.begin(), name.end(), ' '), name.end()); // remove whitespace
      parentLink.erase(std::remove(name.begin(), name.end(), ' '), name.end()); // remove whitespace
      if(!this->refRobotRaw_->link(parentLink)){
        std::cerr << "\x1b[31m[" << this->m_profile.instance_name << "] " << " link [" << parentLink << "]" << " is not found for " << name << "\x1b[39m" << std::endl;
        return RTC::RTC_ERROR;
      }
      cnoid::Matrix3 localR;
      if(localaxis.norm() == 0) localR = cnoid::Matrix3::Identity();
      else localR = Eigen::AngleAxisd(localangle, localaxis.normalized()).toRotationMatrix();
      cnoid::Position localT;
      localT.translation() = localp;
      localT.linear() = localR;

      // 各EndEffectorsから親リンク側に遡っていき、最初に見つかったForceSensorをEndEffectorに対応付ける. 以後、ForceSensorの値を座標変換したものがEndEffectorが受けている力とみなされる. 見つからなければ受けている力は常に0とみなされる
      std::string forceSensor = "";
      {
        cnoid::LinkPtr link = this->refRobotRaw_->link(parentLink);
        bool found = false;
        while (link != nullptr && found == false) {
          for (size_t j = 0; j < forceSensors.size(); j++) {
            if(forceSensors[j]->link() == link) {
              forceSensor = forceSensors[j]->name();
              found = true;
              break;
            }
          }
        }
      }

      this->endEffectorParams_.push_back(name, parentLink, localT, forceSensor);
    }
  }

  for(int i=0;i<this->endEffectorParams_.name.size();i++){
    this->impedanceController_.push_back();
  }

  // 0番目が右脚. 1番目が左脚. という仮定がある.
  if(this->endEffectorParams_.name.size() < NUM_LEGS || this->endEffectorParams_.name[RLEG] != "rleg" || this->endEffectorParams_.name[LLEG] != "lleg"){
    std::cerr << "\x1b[31m[" << this->m_profile.instance_name << "] " << " this->endEffectorParams_.name.size() < 2 || this->endEffectorParams_.name[0] != \"rleg\" || this->endEffectorParams_.name[1] != \"lleg\" not holds" << "\x1b[39m" << std::endl;
    return RTC::RTC_ERROR;
  }

  {
    // generate LegParams
    // init-poseのとき両脚が水平かつX軸が前方であるという仮定がある
    cnoid::Position defautFootMidCoords = mathutil::calcMidCoords(std::vector<cnoid::Position>{cnoid::Position(this->refRobot_->link(this->endEffectorParams_.parentLink[RLEG])->T()*this->endEffectorParams_.localT[RLEG]),cnoid::Position(this->refRobot_->link(this->endEffectorParams_.parentLink[LLEG])->T()*this->endEffectorParams_.localT[LLEG])},
                                                            std::vector<double>{1,1});
    for(int i=0; i<NUM_LEGS; i++){
      cnoid::Position defaultPose = this->refRobot_->link(this->endEffectorParams_.parentLink[i])->T()*this->endEffectorParams_.localT[i];
      this->gaitParam_.defaultTranslatePos[i] = defautFootMidCoords.inverse() * defaultPose.translation();
      this->gaitParam_.defaultTranslatePos[i][2] = 0.0;
    }
  }

  {
    // generate JointParams
    for(int i=0;i<this->genRobot_->numJoints();i++){
      cnoid::LinkPtr joint = this->genRobot_->joint(i);
      JointParam param;
      param.name = joint->name();
      double climit = 0.0, gearRatio = 0.0, torqueConst = 0.0;
      joint->info()->read("climit",climit); joint->info()->read("gearRatio",gearRatio); joint->info()->read("torqueConst",torqueConst);
      param.maxTorque = climit * gearRatio * torqueConst;
      param.controllable = true;
      this->jointParams_.push_back(param);
    }

    // apply margin to jointlimit
    for(int i=0;i<this->genRobot_->numJoints();i++){
      cnoid::LinkPtr joint = this->genRobot_->joint(i);
      if(joint->q_upper() - joint->q_lower() > 0.002){
        joint->setJointRange(joint->q_lower()+0.001,joint->q_upper()-0.001);
      }
      // JointVelocityについて. 1.0だと安全.4.0は脚.10.0はlapid manipulation らしい. limitを小さくしすぎた状態で、速い指令を送ると、狭いlimitの中で高優先度タスクを頑張って満たそうとすることで、低優先度タスクを満たす余裕がなくエラーが大きくなってしまうことに注意.
      if(joint->dq_upper() - joint->dq_lower() > 0.02){
        joint->setJointVelocityRange(joint->dq_lower()+0.01,joint->dq_upper()-0.01);
      }
    }
  }

  {
    // add more ports (ロボットモデルやEndEffectorの情報を使って)

    // 各EndEffectorにつき、ref<name>WrenchInというInPortをつくる
    this->ports_.m_refWrenchIn_.resize(this->endEffectorParams_.name.size());
    this->ports_.m_refWrench_.resize(this->endEffectorParams_.name.size());
    for(int i=0;i<this->endEffectorParams_.name.size();i++){
      std::string name = "ref"+this->endEffectorParams_.name[i]+"WrenchIn";
      this->ports_.m_refWrenchIn_[i] = std::make_unique<RTC::InPort<RTC::TimedDoubleSeq> >(name.c_str(), this->ports_.m_refWrench_[i]);
      this->addInPort(name.c_str(), *(this->ports_.m_refWrenchIn_[i]));
    }

    // 各ForceSensorにつき、act<name>InというInportをつくる
    cnoid::DeviceList<cnoid::ForceSensor> forceSensors(this->actRobotRaw_->devices());
    this->ports_.m_actWrenchIn_.resize(forceSensors.size());
    this->ports_.m_actWrench_.resize(forceSensors.size());
    for(int i=0;i<forceSensors.size();i++){
      std::string name = "act"+forceSensors[i]->name()+"In";
      this->ports_.m_actWrenchIn_[i] = std::make_unique<RTC::InPort<RTC::TimedDoubleSeq> >(name.c_str(), this->ports_.m_actWrench_[i]);
      this->addInPort(name.c_str(), *(this->ports_.m_actWrenchIn_[i]));
    }
  }

  // initialize parameters
  this->loop_ = 0;


  // TODO

  // std::string jointLimitTableStr;
  // if(this->getProperties().hasKey("joint_limit_table")) jointLimitTableStr = std::string(this->getProperties()["joint_limit_table"]);
  // else jointLimitTableStr = std::string(this->m_pManager->getConfig()["joint_limit_table"]); // 引数 -o で与えたプロパティを捕捉
  // std::vector<std::shared_ptr<joint_limit_table::JointLimitTable> > jointLimitTables = joint_limit_table::readJointLimitTablesFromProperty (this->robot_com_, jointLimitTableStr);
  // std::cerr << "[" << this->m_profile.instance_name << "] joint_limit_table: " << jointLimitTableStr<<std::endl;
  // for(size_t i=0;i<jointLimitTables.size();i++){
  //   // apply margin
  //   for(size_t j=0;j<jointLimitTables[i]->lLimitTable().size();j++){
  //     if(jointLimitTables[i]->uLimitTable()[j] - jointLimitTables[i]->lLimitTable()[j] > 0.002){
  //       jointLimitTables[i]->uLimitTable()[j] -= 0.001;
  //       jointLimitTables[i]->lLimitTable()[j] += 0.001;
  //     }
  //   }
  //   this->jointLimitTablesMap_[jointLimitTables[i]->getSelfJoint()].push_back(jointLimitTables[i]);
  // }

  return RTC::RTC_OK;
}

// static function
bool AutoStabilizer::readInPortData(AutoStabilizer::Ports& ports, cnoid::BodyPtr refRobotRaw, cnoid::BodyPtr actRobotRaw, EndEffectorParam& endEffectors){
  bool qRef_updated = false;
  if(ports.m_qRefIn_.isNew()){
    ports.m_qRefIn_.read();
    if(ports.m_qRef_.data.length() == refRobotRaw->numJoints()){
      for(int i=0;i<ports.m_qRef_.data.length();i++){
        if(std::isfinite(ports.m_qRef_.data[i])) refRobotRaw->joint(i)->q() = ports.m_qRef_.data[i];
      }
      qRef_updated = true;
    }
  }
  if(ports.m_refTauIn_.isNew()){
    ports.m_refTauIn_.read();
    if(ports.m_refTau_.data.length() == refRobotRaw->numJoints()){
      for(int i=0;i<ports.m_refTau_.data.length();i++){
        if(std::isfinite(ports.m_refTau_.data[i])) refRobotRaw->joint(i)->u() = ports.m_refTau_.data[i];
      }
    }
  }
  if(ports.m_refBasePosIn_.isNew()){
    ports.m_refBasePosIn_.read();
    if(std::isfinite(ports.m_refBasePos_.data.x) && std::isfinite(ports.m_refBasePos_.data.y) && std::isfinite(ports.m_refBasePos_.data.z)){
      refRobotRaw->rootLink()->p()[0] = ports.m_refBasePos_.data.x;
      refRobotRaw->rootLink()->p()[1] = ports.m_refBasePos_.data.y;
      refRobotRaw->rootLink()->p()[2] = ports.m_refBasePos_.data.z;
    }
  }
  if(ports.m_refBaseRpyIn_.isNew()){
    ports.m_refBaseRpyIn_.read();
    if(std::isfinite(ports.m_refBaseRpy_.data.r) && std::isfinite(ports.m_refBaseRpy_.data.p) && std::isfinite(ports.m_refBaseRpy_.data.y)){
      refRobotRaw->rootLink()->R() = cnoid::rotFromRpy(ports.m_refBaseRpy_.data.r, ports.m_refBaseRpy_.data.p, ports.m_refBaseRpy_.data.y);
    }
  }
  refRobotRaw->calcForwardKinematics();
  refRobotRaw->calcCenterOfMass();

  for(int i=0;i<ports.m_refWrenchIn_.size();i++){
    if(ports.m_refWrenchIn_[i]->isNew()){
      ports.m_refWrenchIn_[i]->read();
      if(ports.m_refWrench_[i].data.length() == 6){
        for(int j=0;j<6;j++){
          if(std::isfinite(ports.m_refWrench_[i].data[j])) endEffectors.refWrenchOrigin[i][j] = ports.m_refWrench_[i].data[j];
        }
      }
    }
  }

  if(ports.m_qActIn_.isNew()){
    ports.m_qActIn_.read();
    if(ports.m_qAct_.data.length() == actRobotRaw->numJoints()){
      for(int i=0;i<ports.m_qAct_.data.length();i++){
        if(std::isfinite(ports.m_qAct_.data[i])) actRobotRaw->joint(i)->q() = ports.m_qAct_.data[i];
      }
    }
  }
  if(ports.m_dqActIn_.isNew()){
    ports.m_dqActIn_.read();
    if(ports.m_dqAct_.data.length() == actRobotRaw->numJoints()){
      for(int i=0;i<ports.m_dqAct_.data.length();i++){
        if(std::isfinite(ports.m_dqAct_.data[i])) actRobotRaw->joint(i)->dq() = ports.m_dqAct_.data[i];
      }
    }
  }
  if(ports.m_actImuIn_.isNew()){
    ports.m_actImuIn_.read();
    if(std::isfinite(ports.m_actImu_.data.r) && std::isfinite(ports.m_actImu_.data.p) && std::isfinite(ports.m_actImu_.data.y)){
      actRobotRaw->calcForwardKinematics();
      cnoid::RateGyroSensorPtr imu = actRobotRaw->findDevice<cnoid::RateGyroSensor>("gyrometer");
      cnoid::Matrix3 imuR = imu->link()->R() * imu->R_local();
      cnoid::Matrix3 actR = cnoid::rotFromRpy(ports.m_actImu_.data.r, ports.m_actImu_.data.p, ports.m_actImu_.data.y);
      actRobotRaw->rootLink()->R() = Eigen::Matrix3d(Eigen::AngleAxisd(actR) * Eigen::AngleAxisd(imuR.transpose() * actRobotRaw->rootLink()->R())); // 単純に3x3行列の空間でRを積算していると、だんだん数値誤差によって回転行列でなくなってしまう恐れがあるので念の為
    }
  }
  actRobotRaw->calcForwardKinematics();
  actRobotRaw->calcCenterOfMass();

  cnoid::DeviceList<cnoid::ForceSensor> forceSensors(actRobotRaw->devices());
  for(int i=0;i<ports.m_actWrenchIn_.size();i++){
    if(ports.m_actWrenchIn_[i]->isNew()){
      ports.m_actWrenchIn_[i]->read();
      if(ports.m_actWrench_[i].data.length() == 6){
        for(int j=0;j<6;j++){
          if(std::isfinite(ports.m_refWrench_[i].data[j])) forceSensors[i]->F()[j] = ports.m_actWrench_[i].data[j];
        }
      }
    }
  }


  return qRef_updated;
}

// static function
bool AutoStabilizer::execAutoStabilizer(const AutoStabilizer::ControlMode& mode, const cnoid::BodyPtr& refRobotRaw, cnoid::BodyPtr& refRobot, const cnoid::BodyPtr& actRobotRaw, cnoid::BodyPtr& actRobot, cnoid::BodyPtr& genRobot, cnoid::BodyPtr& actRobotTqc, EndEffectorParam& endEffectorParams, GaitParam& gaitParam, double dt, const std::vector<JointParam>& jointParams, const FootStepGenerator& footStepGenerator, const LegCoordsGenerator& legCoordsGenerator, const RefToGenFrameConverter& refToGenFrameConverter, const ActToGenFrameConverter& actToGenFrameConverter, const ImpedanceController& impedanceController, const Stabilizer& stabilizer) {
  if(mode.isSyncToABCInit()){ // startAutoBalancer直後の初回

    // FootOrigin座標系を用いてrefRobotRawをgenerate frameに投影しgenRobotとする
    refToGenFrameConverter.initGenRobot(refRobotRaw, endEffectorParams, gaitParam,
                                        genRobot, gaitParam.footMidCoords, gaitParam.genCog, gaitParam.genCogVel);
  }

  // FootOrigin座標系を用いてrefRobotRawをgenerate frameに投影しrefRobotとする
  refToGenFrameConverter.convertFrame(refRobotRaw, endEffectorParams, gaitParam,
                                      refRobot, endEffectorParams.refPose, endEffectorParams.refWrench, gaitParam.dz);


  if(mode.isSyncToABCInit()){ // startAutoBalancer直後の初回
    {
      // footStepNodesListを初期化する
      cnoid::Position rlegCoords = genRobot->link(endEffectorParams.parentLink[RLEG])->T()*endEffectorParams.localT[RLEG];
      cnoid::Position llegCoords = genRobot->link(endEffectorParams.parentLink[LLEG])->T()*endEffectorParams.localT[LLEG];
      gaitParam.footstepNodesList.resize(1);
      gaitParam.footstepNodesList[0].dstCoords = {rlegCoords, llegCoords};
      gaitParam.footstepNodesList[0].supportTime = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
      gaitParam.footstepNodesList[0].remainTime = 0.0;
      gaitParam.srcCoords = {rlegCoords, llegCoords};
      gaitParam.genCoords.clear();
      gaitParam.genCoords.emplace_back(rlegCoords, cnoid::Vector6::Zero(), cnoid::Vector6::Zero(), cpp_filters::HOFFARBIB);
      gaitParam.genCoords.emplace_back(llegCoords, cnoid::Vector6::Zero(), cnoid::Vector6::Zero(), cpp_filters::HOFFARBIB);
      cnoid::Vector3 zmp = 0.5 * (rlegCoords.translation() + rlegCoords.linear()*gaitParam.copOffset[RLEG]) + 0.5 * (llegCoords.translation() + llegCoords.linear()*gaitParam.copOffset[LLEG]);
      gaitParam.refZmpTraj.clear();
      gaitParam.refZmpTraj.push_back(footguidedcontroller::LinearTrajectory<cnoid::Vector3>(zmp,zmp,0.0));
    }

    for(int i=0;i<NUM_LEGS;i++){
      gaitParam.prevSupportPhase[i] = gaitParam.isSupportPhase(i);
    }

    for(int i=0;i<endEffectorParams.name.size();i++){
      endEffectorParams.icOffset[i].reset(cnoid::Vector6::Zero());
      endEffectorParams.stOffset[i].reset(cnoid::Vector6::Zero());
    }
    gaitParam.stOffsetRootRpy.reset(cnoid::Vector3::Zero());
  }

  // FootOrigin座標系を用いてactRobotRawをgenerate frameに投影しactRobotとする
  actToGenFrameConverter.convertFrame(actRobotRaw, endEffectorParams, gaitParam, dt,
                                      actRobot, endEffectorParams.actPose, endEffectorParams.actWrench, gaitParam.actCog,gaitParam.actCogVel);

  // Impedance Controller
  impedanceController.calcImpedanceControl(dt, endEffectorParams,
                                           endEffectorParams.icOffset);
  for(int i=0;i<endEffectorParams.name.size();i++){
    endEffectorParams.icOffset[i].interpolate(dt);
    cnoid::Vector6 icOffset = endEffectorParams.icOffset[i].value();
    endEffectorParams.icTargetPose[i].translation() = icOffset.head<3>() + endEffectorParams.refPose[i].translation();
    endEffectorParams.icTargetPose[i].linear() = cnoid::AngleAxisd(icOffset.tail<3>().norm(),(icOffset.tail<3>().norm()>0)?icOffset.tail<3>().normalized() : cnoid::Vector3::UnitX()) * endEffectorParams.refPose[i].linear();
  }

  // AutoBalancer
  footStepGenerator.calcFootSteps(gaitParam, dt,
                                  gaitParam.footstepNodesList, gaitParam.srcCoords);
  legCoordsGenerator.calcLegCoords(gaitParam, dt,
                                   gaitParam.refZmpTraj, gaitParam.genCoords, gaitParam.footstepNodesList, gaitParam.srcCoords, gaitParam.footMidCoords, gaitParam.prevSupportPhase);
  legCoordsGenerator.calcCOMCoords(gaitParam, dt, 9.80665, genRobot->mass(),
                                   gaitParam.genCog, gaitParam.genCogVel);
  for(int i=0;i<endEffectorParams.name.size();i++){
    if(i<NUM_LEGS) endEffectorParams.abcTargetPose[i] = gaitParam.genCoords[i].value();
    else endEffectorParams.abcTargetPose[i] = endEffectorParams.icTargetPose[i];
  }

  // Stabilizer
  if(mode.isSTRunning()){
    stabilizer.execStabilizer(refRobot, actRobot, genRobot, gaitParam, endEffectorParams, dt, 9.80665, genRobot->mass(),
                              actRobotTqc, gaitParam.stOffsetRootRpy, endEffectorParams.stOffset);
  }else if(mode.isSyncToStopST()){ // stopST直後の初回
    gaitParam.stOffsetRootRpy.setGoal(cnoid::Vector3::Zero(),mode.remainTime());
    for(int i=0;i<endEffectorParams.name.size();i++){
      endEffectorParams.stOffset[i].setGoal(cnoid::Vector6::Zero(),mode.remainTime());
    }
  }
  gaitParam.stOffsetRootRpy.interpolate(dt);
  gaitParam.stTargetRootPose.translation() = refRobot->rootLink()->p();
  gaitParam.stTargetRootPose.linear() /*generate frame*/= gaitParam.footMidCoords.value().linear() * cnoid::rotFromRpy(gaitParam.stOffsetRootRpy.value()/*gaitParam.footMidCoords frame*/) * gaitParam.footMidCoords.value().linear().transpose() * refRobot->rootLink()->R()/*generate frame*/;
  for(int i=0;i<endEffectorParams.name.size();i++){
    endEffectorParams.stOffset[i].interpolate(dt);
    cnoid::Vector6 stOffset = endEffectorParams.stOffset[i].value();
    endEffectorParams.stTargetPose[i].translation() = stOffset.head<3>() + endEffectorParams.abcTargetPose[i].translation();
    endEffectorParams.stTargetPose[i].linear() = cnoid::AngleAxisd(stOffset.tail<3>().norm(),(stOffset.tail<3>().norm()>0)?stOffset.tail<3>().normalized() : cnoid::Vector3::UnitX()) * endEffectorParams.abcTargetPose[i].linear();
  }


  return true;
}

// static function
bool AutoStabilizer::solveFullbodyIK(cnoid::BodyPtr& genRobot, const cnoid::BodyPtr& refRobot, EndEffectorParam& endEffectorParams, AutoStabilizer::FullbodyIKParam& fullbodyIKParam, double dt, const std::vector<JointParam>& jointParams, const GaitParam& gaitParam) {
  if(fullbodyIKParam.jlim_avoid_weight.size() != 6+genRobot->numJoints()) fullbodyIKParam.jlim_avoid_weight = cnoid::VectorX::Zero(6+genRobot->numJoints());
  cnoid::VectorX dq_weight_all = cnoid::VectorX::Zero(6+genRobot->numJoints());
  for(int i=0;i<6;i++) dq_weight_all[i] = 1.0;
  for(int i=0;i<jointParams.size();i++){
    if(jointParams[i].controllable) dq_weight_all[6+i] = 1.0;
  }

  std::vector<std::shared_ptr<IK::IKConstraint> > ikConstraint;

  // EEF
  for(int i=0;i<endEffectorParams.name.size();i++){
    endEffectorParams.ikPositionConstraint[i]->A_link() = genRobot->link(endEffectorParams.parentLink[i]);
    endEffectorParams.ikPositionConstraint[i]->A_localpos() = endEffectorParams.localT[i];
    endEffectorParams.ikPositionConstraint[i]->B_link() = nullptr;
    endEffectorParams.ikPositionConstraint[i]->B_localpos() = endEffectorParams.stTargetPose[i];
    endEffectorParams.ikPositionConstraint[i]->maxError() << 10.0*dt, 10.0*dt, 10.0*dt, 10.0*dt, 10.0*dt, 10.0*dt;
    endEffectorParams.ikPositionConstraint[i]->precision() << 0.0, 0.0, 0.0, 0.0, 0.0, 0.0; // 強制的にIKをmax loopまで回す
    if(i<NUM_LEGS) endEffectorParams.ikPositionConstraint[i]->weight() << 9.0, 9.0, 9.0, 9.0, 9.0, 9.0;
    else endEffectorParams.ikPositionConstraint[i]->weight() << 1.0, 1.0, 1.0, 1.0, 1.0, 1.0;
    endEffectorParams.ikPositionConstraint[i]->eval_link() = nullptr;
    endEffectorParams.ikPositionConstraint[i]->eval_localR() = endEffectorParams.ikPositionConstraint[i]->B_localpos().linear();
    ikConstraint.push_back(endEffectorParams.ikPositionConstraint[i]);
  }

  // COM
  {
    fullbodyIKParam.comConstraint->A_robot() = genRobot;
    fullbodyIKParam.comConstraint->A_localp() = cnoid::Vector3::Zero();
    fullbodyIKParam.comConstraint->B_robot() = nullptr;
    fullbodyIKParam.comConstraint->B_localp() = gaitParam.genCog;
    fullbodyIKParam.comConstraint->maxError() << 10.0*dt, 10.0*dt, 10.0*dt;
    fullbodyIKParam.comConstraint->precision() << 0.0, 0.0, 0.0; // 強制的にIKをmax loopまで回す
    fullbodyIKParam.comConstraint->weight() << 3.0, 3.0, 3.0;
    fullbodyIKParam.comConstraint->eval_R() = cnoid::Matrix3::Identity();
    ikConstraint.push_back(fullbodyIKParam.comConstraint);
  }

  // Angular Momentum
  {
    fullbodyIKParam.angularMomentumConstraint->robot() = genRobot;
    fullbodyIKParam.angularMomentumConstraint->targetAngularMomentum() = cnoid::Vector3::Zero(); // TODO
    fullbodyIKParam.angularMomentumConstraint->maxError() << 1.0*dt, 1.0*dt, 1.0*dt;
    fullbodyIKParam.angularMomentumConstraint->precision() << 0.0, 0.0, 0.0; // 強制的にIKをmax loopまで回す
    fullbodyIKParam.angularMomentumConstraint->weight() << 1e-4, 1e-4, 0.0; // TODO
    fullbodyIKParam.angularMomentumConstraint->dt() = dt;
    fullbodyIKParam.comConstraint->eval_R() = cnoid::Matrix3::Identity();
    ikConstraint.push_back(fullbodyIKParam.angularMomentumConstraint);
  }

  // root
  {
    fullbodyIKParam.rootPositionConstraint->A_link() = genRobot->rootLink();
    fullbodyIKParam.rootPositionConstraint->A_localpos() = cnoid::Position::Identity();
    fullbodyIKParam.rootPositionConstraint->B_link() = nullptr;
    fullbodyIKParam.rootPositionConstraint->B_localpos() = gaitParam.stTargetRootPose;
    fullbodyIKParam.rootPositionConstraint->maxError() << 10.0*dt, 10.0*dt, 10.0*dt, 10.0*dt, 10.0*dt, 10.0*dt;
    fullbodyIKParam.rootPositionConstraint->precision() << 0.0, 0.0, 0.0, 0.0, 0.0, 0.0; // 強制的にIKをmax loopまで回す
    fullbodyIKParam.rootPositionConstraint->weight() << 0.0, 0.0, 0.0, 3, 3, 3; // 角運動量を利用するときは重みを小さく. 通常時、胴の質量・イナーシャやマスパラ誤差の大きさや、胴を大きく動かすための出力不足などによって、二足動歩行では胴の傾きの自由度を使わない方がよい
    //fullbodyIKParam.rootPositionConstraint->weight() << 0.0, 0.0, 0.0, 3e-1, 3e-1, 3e-1;
    fullbodyIKParam.rootPositionConstraint->eval_link() = nullptr;
    fullbodyIKParam.rootPositionConstraint->eval_localR() = cnoid::Matrix3::Identity();
    ikConstraint.push_back(fullbodyIKParam.rootPositionConstraint);
  }

  // reference angle
  {
    if(fullbodyIKParam.refJointAngleConstraint.size() != genRobot->numJoints()) { // 初回
      fullbodyIKParam.refJointAngleConstraint.clear();
      for(size_t i=0;i<genRobot->numJoints();i++) fullbodyIKParam.refJointAngleConstraint.push_back(std::make_shared<IK::JointAngleConstraint>());
    }
    for(size_t i=0;i<genRobot->numJoints();i++){
      if(!jointParams[i].controllable) continue;
      fullbodyIKParam.refJointAngleConstraint[i]->joint() = genRobot->joint(i);
      fullbodyIKParam.refJointAngleConstraint[i]->maxError() = 10.0 * dt; // 高優先度のmaxError以下にしないと優先度逆転するおそれ
      fullbodyIKParam.refJointAngleConstraint[i]->weight() = 1e-1; // 小さい値すぎると、qp終了判定のtoleranceによって無視されてしまう
      fullbodyIKParam.refJointAngleConstraint[i]->targetq() = refRobot->joint(i)->q();
      fullbodyIKParam.refJointAngleConstraint[i]->precision() = 0.0; // 強制的にIKをmax loopまで回す
      ikConstraint.push_back(fullbodyIKParam.refJointAngleConstraint[i]);
    }
  }

  for(int i=0;i<ikConstraint.size();i++) ikConstraint[i]->debuglevel() = 0; //debuglevel
  fik::solveFullbodyIKLoopFast(genRobot,
                               ikConstraint,
                               fullbodyIKParam.jlim_avoid_weight,
                               dq_weight_all,
                               1,//loop
                               1e-6,
                               0, //debug
                               dt
                               );
  return true;
}

// static function
bool AutoStabilizer::writeOutPortData(AutoStabilizer::Ports& ports, cnoid::BodyPtr genRobot, const AutoStabilizer::ControlMode& mode, AutoStabilizer::OutputOffsetInterpolators& outputOffsetInterpolators, double dt){
  if(outputOffsetInterpolators.qInterpolator.size() == 0){ // 初回のみ
    for(int i=0;i<genRobot->numJoints();i++){
      outputOffsetInterpolators.qInterpolator.emplace_back(0.0,0.0,0.0,cpp_filters::HOFFARBIB);
      outputOffsetInterpolators.genTauInterpolator.emplace_back(0.0,0.0,0.0,cpp_filters::HOFFARBIB);
    }
  }

  {
    // q
    ports.m_q_.tm = ports.m_qRef_.tm;
    ports.m_q_.data.length(genRobot->numJoints());
    for(int i=0;i<genRobot->numJoints();i++){
      if(!mode.isSyncToABC() && !mode.isSyncToIdle()){
        ports.m_q_.data[i] = genRobot->joint(i)->q();
      }else{
        if(mode.isSyncToABCInit() || mode.isSyncToIdleInit()){
          outputOffsetInterpolators.qInterpolator[i].reset(ports.m_q_.data[i]-genRobot->joint(i)->q());
        }
        outputOffsetInterpolators.qInterpolator[i].setGoal(0.0,mode.remainTime());
        double x,v,a;
        outputOffsetInterpolators.qInterpolator[i].get(x,v,a,dt);
        ports.m_q_.data[i] = genRobot->joint(i)->q() + x;
      }
    }
    ports.m_qOut_.write();
  }

  {
    // tau
    ports.m_genTau_.tm = ports.m_qRef_.tm;
    ports.m_genTau_.data.length(genRobot->numJoints());
    for(int i=0;i<genRobot->numJoints();i++){
      if(!mode.isSyncToABC() && !mode.isSyncToIdle()){
        ports.m_genTau_.data[i] = genRobot->joint(i)->u();
      }else{
        if(mode.isSyncToABCInit() || mode.isSyncToIdleInit()){
          outputOffsetInterpolators.genTauInterpolator[i].reset(ports.m_genTau_.data[i]-genRobot->joint(i)->u());
        }
        outputOffsetInterpolators.genTauInterpolator[i].setGoal(0.0,mode.remainTime());
        double x,v,a;
        outputOffsetInterpolators.genTauInterpolator[i].get(x,v,a,dt);
        ports.m_genTau_.data[i] = genRobot->joint(i)->u() + x;
      }
    }
    ports.m_genTauOut_.write();
  }

  {
    // basePose
    cnoid::Position basePose = genRobot->rootLink()->T();
    if(!mode.isSyncToABC() && !mode.isSyncToIdle()){
      if(mode.isSyncToABCInit() || mode.isSyncToIdleInit()){
        cnoid::Position prevPose;
        prevPose.translation()= cnoid::Vector3(ports.m_genBasePose_.data.position.x,ports.m_genBasePose_.data.position.x,ports.m_genBasePose_.data.position.x);
        prevPose.linear() = cnoid::rotFromRpy(ports.m_genBasePose_.data.orientation.r, ports.m_genBasePose_.data.orientation.p, ports.m_genBasePose_.data.orientation.y);
        outputOffsetInterpolators.genBasePoseInterpolator.reset(prevPose * basePose.inverse());
      }
      outputOffsetInterpolators.genBasePoseInterpolator.setGoal(cnoid::Position::Identity(),mode.remainTime());
      outputOffsetInterpolators.genBasePoseInterpolator.interpolate(dt);
      cnoid::Position offsetPose = outputOffsetInterpolators.genBasePoseInterpolator.value();
      basePose = offsetPose * basePose;
    }
    cnoid::Vector3 basePos = basePose.translation();
    cnoid::Matrix3 baseR = basePose.linear();
    cnoid::Vector3 baseRpy = cnoid::rpyFromRot(basePose.linear());

    ports.m_genBasePose_.tm = ports.m_qRef_.tm;
    ports.m_genBasePose_.data.position.x = basePos[0];
    ports.m_genBasePose_.data.position.y = basePos[1];
    ports.m_genBasePose_.data.position.z = basePos[2];
    ports.m_genBasePose_.data.orientation.r = baseRpy[0];
    ports.m_genBasePose_.data.orientation.p = baseRpy[1];
    ports.m_genBasePose_.data.orientation.y = baseRpy[2];
    ports.m_genBasePoseOut_.write();

    ports.m_genBaseTform_.tm = ports.m_qRef_.tm;
    ports.m_genBaseTform_.data.length(12);
    for(int i=0;i<3;i++){
      ports.m_genBaseTform_.data[i] = basePos[i];
    }
    for(int i=0;i<3;i++){
      for(int j=0;j<3;j++){
        ports.m_genBaseTform_.data[3+i*3+j] = baseR(i,j);// row major
      }
    }
    ports.m_genBaseTformOut_.write();

    ports.m_genBasePos_.tm = ports.m_qRef_.tm;
    ports.m_genBasePos_.data.x = basePos[0];
    ports.m_genBasePos_.data.y = basePos[1];
    ports.m_genBasePos_.data.z = basePos[2];
    ports.m_genBasePosOut_.write();
    ports.m_genBaseRpy_.tm = ports.m_qRef_.tm;
    ports.m_genBaseRpy_.data.r = baseRpy[0];
    ports.m_genBaseRpy_.data.p = baseRpy[1];
    ports.m_genBaseRpy_.data.y = baseRpy[2];
    ports.m_genBaseRpyOut_.write();
  }

  return true;
}

RTC::ReturnCode_t AutoStabilizer::onExecute(RTC::UniqueId ec_id){
  std::lock_guard<std::mutex> guard(this->mutex_);

  std::string instance_name = std::string(this->m_profile.instance_name);
  this->loop_++;

  if(!AutoStabilizer::readInPortData(this->ports_, this->refRobotRaw_, this->actRobotRaw_, this->endEffectorParams_)) return RTC::RTC_OK;  // qRef が届かなければ何もしない

  this->mode_.update(this->dt_);
  this->refToGenFrameConverter_.update(this->dt_);

  if(!this->mode_.isABCRunning()) {
    cnoidbodyutil::copyRobotState(this->refRobotRaw_, this->genRobot_);
  }else{
    if(this->mode_.isSyncToABCInit()){ // startAutoBalancer直後の初回
      this->refToGenFrameConverter_.reset();
      this->actToGenFrameConverter_.reset();
      this->footStepGenerator_.reset();
      this->impedanceController_.reset();
    }
    AutoStabilizer::execAutoStabilizer(this->mode_, this->refRobotRaw_, this->refRobot_, this->actRobotRaw_, this->actRobot_, this->genRobot_, this->actRobotTqc_, this->endEffectorParams_, this->gaitParam_, this->dt_, this->jointParams_, this->footStepGenerator_, this->legCoordsGenerator_, this->refToGenFrameConverter_, this->actToGenFrameConverter_, this->impedanceController_, this->stabilizer_);
    AutoStabilizer::solveFullbodyIK(this->genRobot_, this->refRobot_, this->endEffectorParams_, this->fullbodyIKParam_, this->dt_, this->jointParams_, this->gaitParam_);
  }

  AutoStabilizer::writeOutPortData(this->ports_, this->genRobot_, this->mode_, this->outputOffsetInterpolators_, this->dt_);

  return RTC::RTC_OK;
}

RTC::ReturnCode_t AutoStabilizer::onActivated(RTC::UniqueId ec_id){
  std::lock_guard<std::mutex> guard(this->mutex_);
  std::cerr << "[" << m_profile.instance_name << "] "<< "onActivated(" << ec_id << ")" << std::endl;
  // 各種処理を初期化する TODO
  this->mode_.reset();
  return RTC::RTC_OK;
}
RTC::ReturnCode_t AutoStabilizer::onDeactivated(RTC::UniqueId ec_id){
  std::lock_guard<std::mutex> guard(this->mutex_);
  std::cerr << "[" << m_profile.instance_name << "] "<< "onDeactivated(" << ec_id << ")" << std::endl;
  return RTC::RTC_OK;
}
RTC::ReturnCode_t AutoStabilizer::onFinalize(){ return RTC::RTC_OK; }

bool AutoStabilizer::goPos(const double& x, const double& y, const double& th){
  std::lock_guard<std::mutex> guard(this->mutex_);
  return true;
}
bool AutoStabilizer::goVelocity(const double& vx, const double& vy, const double& vth){
  std::lock_guard<std::mutex> guard(this->mutex_);
  if(this->mode_.isABCRunning()){
    this->footStepGenerator_.isGoVelocityMode = true;
    this->footStepGenerator_.cmdVel[0] = vx;
    this->footStepGenerator_.cmdVel[1] = vy;
    this->footStepGenerator_.cmdVel[2] = vth / 180.0 * M_PI;
    return true;
  }else{
    return false;
  }
}
bool AutoStabilizer::goStop(){
  std::lock_guard<std::mutex> guard(this->mutex_);
  if(this->mode_.isABCRunning()){
    this->footStepGenerator_.isGoVelocityMode = false;
    this->footStepGenerator_.cmdVel.setZero();
  }else{
    return false;
  }
  return true;
}
bool AutoStabilizer::jumpTo(const double& x, const double& y, const double& z, const double& ts, const double& tf){
  std::lock_guard<std::mutex> guard(this->mutex_);
  return true;
}
bool AutoStabilizer::setFootSteps(const OpenHRP::AutoStabilizerService::FootstepsSequence& fss, CORBA::Long overwrite_fs_idx){
  std::lock_guard<std::mutex> guard(this->mutex_);
  return true;
}
bool AutoStabilizer::setFootStepsWithParam(const OpenHRP::AutoStabilizerService::FootstepsSequence& fss, const OpenHRP::AutoStabilizerService::StepParamsSequence& spss, CORBA::Long overwrite_fs_idx){
  std::lock_guard<std::mutex> guard(this->mutex_);
  return true;
}
void AutoStabilizer::waitFootSteps(){
  return;
}

bool AutoStabilizer::releaseEmergencyStop(){
  std::lock_guard<std::mutex> guard(this->mutex_);
  return true;
}


bool AutoStabilizer::startAutoBalancer(){
  if(this->mode_.setNextTransition(ControlMode::START_ABC)){
    std::cerr << "[" << m_profile.instance_name << "] start auto balancer mode" << std::endl;
    while (this->mode_.now() != ControlMode::MODE_ABC) usleep(1000);
    usleep(1000);
    return true;
  }else{
    std::cerr << "[" << this->m_profile.instance_name << "] auto balancer is already started" << std::endl;
    return false;
  }
}
bool AutoStabilizer::stopAutoBalancer(){
  if(this->mode_.setNextTransition(ControlMode::STOP_ABC)){
    std::cerr << "[" << m_profile.instance_name << "] stop auto balancer mode" << std::endl;
    while (this->mode_.now() != ControlMode::MODE_IDLE) usleep(1000);
    usleep(1000);
    return true;
  }else{
    std::cerr << "[" << this->m_profile.instance_name << "] auto balancer is already stopped" << std::endl;
    return false;
  }
}
bool AutoStabilizer::startStabilizer(void){
  if(this->mode_.setNextTransition(ControlMode::START_ST)){
    std::cerr << "[" << m_profile.instance_name << "] start ST" << std::endl;
    while (this->mode_.now() != ControlMode::MODE_ST) usleep(1000);
    usleep(1000);
    return true;
  }else{
    std::cerr << "[" << this->m_profile.instance_name << "] Please start AutoBalancer" << std::endl;
    return false;
  }
}
bool AutoStabilizer::stopStabilizer(void){
  if(this->mode_.setNextTransition(ControlMode::STOP_ST)){
    std::cerr << "[" << m_profile.instance_name << "] stop ST" << std::endl;
    while (this->mode_.now() != ControlMode::MODE_ABC) usleep(1000);
    usleep(1000);
    return true;
  }else{
    std::cerr << "[" << this->m_profile.instance_name << "] Please start AutoBalancer" << std::endl;
    return false;
  }
}

bool AutoStabilizer::startImpedanceController(const std::string& i_name){
  std::lock_guard<std::mutex> guard(this->mutex_);
  if(this->mode_.isABCRunning()){
    for(int i=0;i<this->endEffectorParams_.name.size();i++){
      if(this->endEffectorParams_.name[i] != i_name) continue;
      if(this->impedanceController_.isImpedanceMode[i]) {
        std::cerr << "[" << this->m_profile.instance_name << "] Impedance control [" << i_name << "] is already started" << std::endl;
        return false;
      }
      std::cerr << "[" << this->m_profile.instance_name << "] Start impedance control [" << i_name << "]" << std::endl;
      this->impedanceController_.isImpedanceMode[i] = true;
      return true;
    }
    std::cerr << "[" << this->m_profile.instance_name << "] Could not found impedance controller param [" << i_name << "]" << std::endl;
    return false;
  }else{
    std::cerr << "[" << this->m_profile.instance_name << "] Please start AutoBalancer" << std::endl;
    return false;
  }
}

bool AutoStabilizer::stopImpedanceController(const std::string& i_name){
  std::lock_guard<std::mutex> guard(this->mutex_);
  if(this->mode_.isABCRunning()){
    for(int i=0;i<this->endEffectorParams_.name.size();i++){
      if(this->endEffectorParams_.name[i] != i_name) continue;
      if(!this->impedanceController_.isImpedanceMode[i]) {
        std::cerr << "[" << this->m_profile.instance_name << "] Impedance control [" << i_name << "] is already stopped" << std::endl;
        return false;
      }
      std::cerr << "[" << this->m_profile.instance_name << "] Stop impedance control [" << i_name << "]" << std::endl;
      this->impedanceController_.isImpedanceMode[i] = false;
      this->endEffectorParams_.icOffset[i].setGoal(cnoid::Vector6::Zero(), 2.0);
      return true;
    }
    std::cerr << "[" << this->m_profile.instance_name << "] Could not found impedance controller param [" << i_name << "]" << std::endl;
    return false;
  }else{
    std::cerr << "[" << this->m_profile.instance_name << "] Please start AutoBalancer" << std::endl;
    return false;
  }
}

bool AutoStabilizer::setGaitGeneratorParam(const OpenHRP::AutoStabilizerService::GaitGeneratorParam& i_param){
  std::lock_guard<std::mutex> guard(this->mutex_);
  return true;
}
bool AutoStabilizer::getGaitGeneratorParam(OpenHRP::AutoStabilizerService::GaitGeneratorParam& i_param){
  std::lock_guard<std::mutex> guard(this->mutex_);
  return true;
}
bool AutoStabilizer::setAutoBalancerParam(const OpenHRP::AutoStabilizerService::AutoBalancerParam& i_param){
  std::lock_guard<std::mutex> guard(this->mutex_);
  return true;
}
bool AutoStabilizer::getAutoBalancerParam(OpenHRP::AutoStabilizerService::AutoBalancerParam& i_param){
  std::lock_guard<std::mutex> guard(this->mutex_);
  // i_param.leg_names // refFootOriginWeightとautoControlRatioが必要
  return true;
}
void AutoStabilizer::setStabilizerParam(const OpenHRP::AutoStabilizerService::StabilizerParam& i_param){
  std::lock_guard<std::mutex> guard(this->mutex_);
  return;
}
void AutoStabilizer::getStabilizerParam(OpenHRP::AutoStabilizerService::StabilizerParam& i_param){
  std::lock_guard<std::mutex> guard(this->mutex_);
  return;
}

bool AutoStabilizer::getProperty(const std::string& key, std::string& ret) {
  if (this->getProperties().hasKey(key.c_str())) {
    ret = std::string(this->getProperties()[key.c_str()]);
  } else if (this->m_pManager->getConfig().hasKey(key.c_str())) { // 引数 -o で与えたプロパティを捕捉
    ret = std::string(this->m_pManager->getConfig()[key.c_str()]);
  } else {
    return false;
  }
  std::cerr << "[" << this->m_profile.instance_name << "] " << key << ": " << ret <<std::endl;
  return true;
}


extern "C"{
    void AutoStabilizerInit(RTC::Manager* manager) {
        RTC::Properties profile(AutoStabilizer_spec);
        manager->registerFactory(profile, RTC::Create<AutoStabilizer>, RTC::Delete<AutoStabilizer>);
    }
};
