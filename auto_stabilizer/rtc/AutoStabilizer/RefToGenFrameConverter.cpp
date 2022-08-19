#include "RefToGenFrameConverter.h"
#include "CnoidBodyUtil.h"
#include "MathUtil.h"

bool RefToGenFrameConverter::initGenRobot(const GaitParam& gaitParam, // input
                                          cnoid::BodyPtr& genRobot, cpp_filters::TwoPointInterpolatorSE3& o_footMidCoords, cnoid::Vector3& o_genCogVel) const{ // output
  genRobot->rootLink()->T() = gaitParam.refRobotRaw->rootLink()->T();
  genRobot->rootLink()->v() = gaitParam.refRobotRaw->rootLink()->v();
  genRobot->rootLink()->w() = gaitParam.refRobotRaw->rootLink()->w();
  for(int i=0;i<genRobot->numJoints();i++){
    genRobot->joint(i)->q() = gaitParam.refRobotRaw->joint(i)->q();
    genRobot->joint(i)->dq() = gaitParam.refRobotRaw->joint(i)->dq();
    genRobot->joint(i)->u() = gaitParam.refRobotRaw->joint(i)->u();
  }
  genRobot->calcForwardKinematics();
  cnoid::Position refFootMidCoords = this->calcRefFootMidCoords(genRobot, gaitParam);
  cnoid::Position footMidCoords = mathutil::orientCoordToAxis(refFootMidCoords, cnoid::Vector3::UnitZ());
  cnoidbodyutil::moveCoords(genRobot, footMidCoords, refFootMidCoords);
  genRobot->calcForwardKinematics(true);
  genRobot->calcCenterOfMass();

  cnoid::Vector3 genCogVel = cnoid::Vector3::Zero();

  o_footMidCoords.reset(footMidCoords);
  o_genCogVel = genCogVel;
  return true;
}

bool RefToGenFrameConverter::convertFrame(const GaitParam& gaitParam, double dt,// input
                                          cnoid::BodyPtr& refRobot, std::vector<cnoid::Position>& o_refEEPose, std::vector<cnoid::Vector6>& o_refEEWrench, double& o_refdz, cpp_filters::TwoPointInterpolatorSE3& o_footMidCoords) const{ // output
  cnoidbodyutil::copyRobotState(gaitParam.refRobotRaw, refRobot);

  // footMidCoordsを進める
  cpp_filters::TwoPointInterpolatorSE3 footMidCoords = gaitParam.footMidCoords;
  {
    cnoid::Position rleg = gaitParam.footstepNodesList[0].dstCoords[RLEG];
    rleg.translation() += rleg.linear() * gaitParam.copOffset[RLEG].value();
    rleg.translation() -= rleg.linear() * gaitParam.defaultTranslatePos[RLEG].value();
    cnoid::Position lleg = gaitParam.footstepNodesList[0].dstCoords[LLEG];
    lleg.translation() += lleg.linear() * gaitParam.copOffset[LLEG].value();
    lleg.translation() -= lleg.linear() * gaitParam.defaultTranslatePos[LLEG].value();
    if(gaitParam.footstepNodesList[0].isSupportPhase[RLEG] && gaitParam.footstepNodesList[0].isSupportPhase[LLEG]){ // 両足支持
      cnoid::Position midCoords = mathutil::calcMidCoords(std::vector<cnoid::Position>{rleg, lleg}, std::vector<double>{1.0, 1.0});
      midCoords = mathutil::orientCoordToAxis(midCoords, cnoid::Vector3::UnitZ());
      footMidCoords.setGoal(midCoords, gaitParam.footstepNodesList[0].remainTime);
    }else if(gaitParam.footstepNodesList[0].isSupportPhase[RLEG] && !gaitParam.footstepNodesList[0].isSupportPhase[LLEG]){ // 右足支持
      if(gaitParam.footstepNodesList.size() > 1 &&
         (gaitParam.footstepNodesList[1].isSupportPhase[RLEG] && gaitParam.footstepNodesList[1].isSupportPhase[LLEG]) // 次が両足支持
         ){
        cnoid::Position rleg = gaitParam.footstepNodesList[1].dstCoords[RLEG];
        rleg.translation() += rleg.linear() * gaitParam.copOffset[RLEG].value();
        rleg.translation() -= rleg.linear() * gaitParam.defaultTranslatePos[RLEG].value();
        cnoid::Position lleg = gaitParam.footstepNodesList[1].dstCoords[LLEG];
        lleg.translation() += lleg.linear() * gaitParam.copOffset[LLEG].value();
        lleg.translation() -= lleg.linear() * gaitParam.defaultTranslatePos[LLEG].value();
        cnoid::Position midCoords = mathutil::calcMidCoords(std::vector<cnoid::Position>{rleg, lleg}, std::vector<double>{1.0, 1.0});
        midCoords = mathutil::orientCoordToAxis(midCoords, cnoid::Vector3::UnitZ());
        footMidCoords.setGoal(midCoords, gaitParam.footstepNodesList[0].remainTime + gaitParam.footstepNodesList[1].remainTime);
      }else{
        cnoid::Position midCoords = rleg;
        midCoords = mathutil::orientCoordToAxis(midCoords, cnoid::Vector3::UnitZ());
        footMidCoords.setGoal(midCoords, gaitParam.footstepNodesList[0].remainTime);
      }
    }else if(!gaitParam.footstepNodesList[0].isSupportPhase[RLEG] && gaitParam.footstepNodesList[0].isSupportPhase[LLEG]){ // 左足支持
      if(gaitParam.footstepNodesList.size() > 1 &&
         (gaitParam.footstepNodesList[1].isSupportPhase[RLEG] && gaitParam.footstepNodesList[1].isSupportPhase[LLEG]) // 次が両足支持
         ){
        cnoid::Position rleg = gaitParam.footstepNodesList[1].dstCoords[RLEG];
        rleg.translation() += rleg.linear() * gaitParam.copOffset[RLEG].value();
        rleg.translation() -= rleg.linear() * gaitParam.defaultTranslatePos[RLEG].value();
        cnoid::Position lleg = gaitParam.footstepNodesList[1].dstCoords[LLEG];
        lleg.translation() += lleg.linear() * gaitParam.copOffset[LLEG].value();
        lleg.translation() -= lleg.linear() * gaitParam.defaultTranslatePos[LLEG].value();
        cnoid::Position midCoords = mathutil::calcMidCoords(std::vector<cnoid::Position>{rleg, lleg}, std::vector<double>{1.0, 1.0});
        midCoords = mathutil::orientCoordToAxis(midCoords, cnoid::Vector3::UnitZ());
        footMidCoords.setGoal(midCoords, gaitParam.footstepNodesList[0].remainTime + gaitParam.footstepNodesList[1].remainTime);
      }else{
        cnoid::Position midCoords = lleg;
        midCoords = mathutil::orientCoordToAxis(midCoords, cnoid::Vector3::UnitZ());
        footMidCoords.setGoal(midCoords, gaitParam.footstepNodesList[0].remainTime);
      }
    }else{ // 滞空期 TODO
      if(gaitParam.footstepNodesList[1].isSupportPhase[RLEG] && !gaitParam.footstepNodesList[1].isSupportPhase[LLEG]) { // 次が右足支持
        cnoid::Position midCoords = rleg;
        midCoords = mathutil::orientCoordToAxis(midCoords, cnoid::Vector3::UnitZ());
        footMidCoords.setGoal(midCoords, gaitParam.footstepNodesList[0].remainTime);
      }else if(!gaitParam.footstepNodesList[1].isSupportPhase[RLEG] && gaitParam.footstepNodesList[1].isSupportPhase[LLEG]) { // 次が左足支持
        cnoid::Position midCoords = lleg;
        midCoords = mathutil::orientCoordToAxis(midCoords, cnoid::Vector3::UnitZ());
        footMidCoords.setGoal(midCoords, gaitParam.footstepNodesList[0].remainTime);
      }else{
        cnoid::Position midCoords = mathutil::calcMidCoords(std::vector<cnoid::Position>{rleg, lleg}, std::vector<double>{1.0, 1.0});
        midCoords = mathutil::orientCoordToAxis(midCoords, cnoid::Vector3::UnitZ());
        footMidCoords.setGoal(midCoords, gaitParam.footstepNodesList[0].remainTime);
      }
    }

    footMidCoords.interpolate(dt);
  }

  /*
    次の2つの座標系が一致するようにreference frameとgenerate frameを対応付ける
    - refRobotRawの、refFootOriginWeightとdefaultTranslatePosとcopOffsetに基づいて求めた足裏中間座標 (イメージとしては静止状態の目標ZMP位置にdefaultTranslatePosを作用させたもの)
    - 位置はgenRobotの重心位置 - l. 姿勢はfootMidCoords. (ただしHandFixModeなら、位置のfootMidCoords座標系Y成分はfootMidCoordsの位置.)
      - handControlWeight = 0なら、位置も姿勢もfootMidCoords

    startAutoBalancer直後の初回は、refRobotRawの重心位置とfootOriginのXY位置が異なる場合, refEEPoseが不連続に変化してしまう. startAutoBalancerのtransition_timeで補間してごまかす.
  */

  cnoid::Position refFootMidCoords = this->calcRefFootMidCoords(refRobot, gaitParam);
  double refdz = (refFootMidCoords.inverse() * refRobot->centerOfMass())[2]; // ref重心高さ
  cnoid::Position genFootMidCoords;
  genFootMidCoords.linear() = footMidCoords.value().linear();
  genFootMidCoords.translation() = gaitParam.genCog - gaitParam.l; // 1周期前のlを使っているtが、lは不連続に変化するものではないので良い
  cnoid::Vector3 trans_footMidCoordsLocal = footMidCoords.value().linear().transpose() * (genFootMidCoords.translation() - footMidCoords.value().translation());
  trans_footMidCoordsLocal[1] *= (1.0 - handFixMode.value());
  genFootMidCoords.translation() = footMidCoords.value().translation() + footMidCoords.value().linear() * trans_footMidCoordsLocal;
  cnoidbodyutil::moveCoords(refRobot, genFootMidCoords, refFootMidCoords); // 1周期前のfootMidCoordsを使っているが、footMidCoordsは不連続に変化するものではないのでよい
  refRobot->calcForwardKinematics();
  refRobot->calcCenterOfMass();

  // refEEPoseを計算
  std::vector<cnoid::Position> refEEPose(gaitParam.eeName.size());
  for(int i=0;i<gaitParam.eeName.size();i++){
    refEEPose[i] = refRobot->link(gaitParam.eeParentLink[i])->T() * gaitParam.eeLocalT[i];
  }

  // refEEWrenchを計算
  std::vector<cnoid::Vector6> refEEWrench(gaitParam.eeName.size());
  for(int i=0;i<gaitParam.eeName.size();i++){
    refEEWrench[i].head<3>() = footMidCoords.value().linear() * gaitParam.refEEWrenchOrigin[i].head<3>();
    refEEWrench[i].tail<3>() = footMidCoords.value().linear() * gaitParam.refEEWrenchOrigin[i].tail<3>();
  }

  o_refEEPose = refEEPose;
  o_refEEWrench = refEEWrench;
  o_refdz = refdz;
  o_footMidCoords = footMidCoords;

  return true;
}

cnoid::Position RefToGenFrameConverter::calcRefFootMidCoords(const cnoid::BodyPtr& robot, const GaitParam& gaitParam) const {

  cnoid::Position rleg = robot->link(gaitParam.eeParentLink[RLEG])->T()*gaitParam.eeLocalT[RLEG];
  rleg.translation() += rleg.linear() * gaitParam.copOffset[RLEG].value();
  rleg.translation() -= rleg.linear() * gaitParam.defaultTranslatePos[RLEG].value();
  cnoid::Position lleg = robot->link(gaitParam.eeParentLink[LLEG])->T()*gaitParam.eeLocalT[LLEG];
  lleg.translation() += lleg.linear() * gaitParam.copOffset[LLEG].value();
  lleg.translation() -= lleg.linear() * gaitParam.defaultTranslatePos[LLEG].value();

  cnoid::Position bothmidcoords = mathutil::calcMidCoords(std::vector<cnoid::Position>{rleg, lleg},
                                                          std::vector<double>{1.0, 1.0});
  cnoid::Position rlegmidcoords = rleg;
  cnoid::Position llegmidcoords = lleg;

  double bothweight = std::min(this->refFootOriginWeight[RLEG].value(), this->refFootOriginWeight[LLEG].value());
  double rlegweight = this->refFootOriginWeight[RLEG].value() - bothweight;
  double llegweight = this->refFootOriginWeight[LLEG].value() - bothweight;
  return mathutil::calcMidCoords(std::vector<cnoid::Position>{bothmidcoords, rlegmidcoords, llegmidcoords},
                                 std::vector<double>{bothweight, rlegweight, llegweight});
}
