#include <ros/ros.h>
#include "std_msgs/String.h"
#include "sensor_msgs/LaserScan.h"
#include "geometry_msgs/Pose.h"
#include "geometry_msgs/PoseStamped.h"
#include "geometry_msgs/PointStamped.h"
#include "pole.cpp"
#include "localization/scan_point.h"
#include "tf/transform_datatypes.h"
#include <cmath>

const double kPi = 3.141592653589;

class Loc {
 public:
	Loc() {
		ROS_INFO("Started localization node");
		sub_ = n_.subscribe("scan",1000, &Loc::Callback, this);
		ROS_INFO("Subscribed to \"scan\" topic");
		pub_pose_ = n_.advertise<geometry_msgs::PoseStamped>("bot_pose",1000);
		pub_pole_ = n_.advertise<geometry_msgs::PointStamped>("pole_pos",1000);
		initiation_ = true;	//start with initiation
		pose_.pose.position.x = -2000;	//for recognition if first time calculating
		StateHandler();
	}

 private:
	ros::NodeHandle n_;
	ros::Subscriber sub_;
	ros::Publisher pub_pose_;
	ros::Publisher pub_pole_;

	sensor_msgs::LaserScan scan_;
	std::vector<Pole> poles_;
	geometry_msgs::PoseStamped pose_;
	bool initiation_;

	void CorrectAngle(double& angle) {
    while(angle > kPi) angle -= 2*kPi;
    while(angle < -kPi) angle += 2*kPi;
	}

	void StateHandler() {	//runs either initiation or localization
		while (ros::ok()) {
			if (initiation_) {
				ROS_INFO("started initiation");
				while (initiation_ && ros::ok()) InitiatePoles();
			}
			ROS_INFO("started localization");
			if (!initiation_) {
				while (!initiation_ && ros::ok()) Locate();
			}
		}
	}

	void InitiatePoles() {
		ros::Time begin = ros::Time::now();
		std::vector<std::vector<localization::scan_point> > extracted_scan_points;
		ROS_INFO("Gathering data...");
		double init_duration = 2;
		while ((ros::Time::now()-begin).sec < init_duration && ros::ok()) {	//gather data for 5 seconds
			ros::Rate loop_rate(25);
			ros::spinOnce();	//get one scan
			std::vector<localization::scan_point> temp_scan_points;
			ExtractPoleScans(&temp_scan_points);	//extract relevant poles
			extracted_scan_points.push_back(temp_scan_points);	//save them
			loop_rate.sleep();
		}
		ROS_INFO("Gathered %lu/%d scans", extracted_scan_points.size(), (int)(25*init_duration));
		if (extracted_scan_points.size() < 25) {		//check if enough data was gathered
			extracted_scan_points.clear();	//discard data
			ROS_WARN("Gathering data failed during initiation!");
		}
		else {
			ROS_INFO("Success!");
			std::vector<localization::scan_point> averaged_scan_points;
			for (int i = 0; i < extracted_scan_points.size(); i++) {	//combine all vectors of different measurements to one vector
				for (int j = 0; j < extracted_scan_points[i].size(); j++) {
					averaged_scan_points.push_back(extracted_scan_points[i][j]);
				}
			}
			MinimizeScans(&averaged_scan_points);		//average over all measurements
			for (int i = 0; i < averaged_scan_points.size(); i++) {	
				ROS_INFO("pole (polar) at %f m %f rad", averaged_scan_points[i].distance, averaged_scan_points[i].angle);
				//if (i == 0) ROS_INFO("error %f m",averaged_scan_points[i].distance-6.666667);		//check distance errors
				//if (i == 1) ROS_INFO("error %f m",averaged_scan_points[i].distance-8.91667);
			}
			std::vector<localization::xy_point> xy_poles = ScanToXY(averaged_scan_points);
			for (int i = 0; i < xy_poles.size(); i++) ROS_INFO("pole (kart.) at [%f %f]", xy_poles[i].x, xy_poles[i].y);	//print poles for debugging
			for (int i = 0; i < averaged_scan_points.size(); i++) {	//fill pole vector
				poles_.push_back(Pole(xy_poles[i], averaged_scan_points[i], ros::Time::now(), i));
			}
			PublishPoles();
			initiation_ = false;
		}
	}

	void PublishPoles() {
		//ROS_INFO("Publishing poles...");
		for (int i = 0; i < poles_.size(); i++) {
			geometry_msgs::PointStamped point;
			point.header.seq = 1;
			point.header.stamp = ros::Time::now();
			point.header.frame_id = "fixed_frame";
			point.point.x = poles_[i].xy_coords().x;
			point.point.y = poles_[i].xy_coords().y;
			point.point.z = 0;
			pub_pole_.publish(point);
		}
		//ROS_INFO("Success!");
	}

	void PublishPose() {
		//ROS_INFO("Publishing pose...");
		pub_pose_.publish(pose_);
		//ROS_INFO("Success!");
	}

	std::vector<localization::xy_point> ScanToXY(const std::vector<localization::scan_point> scan) {
		std::vector<localization::xy_point> xy_vector;
		for (int i = 0; i < scan.size(); i++) {
			//convert scan to xy in robot coordinate system
			localization::xy_point point;
			point.x = scan[i].distance*cos(scan[i].angle);
			point.y = scan[i].distance*sin(scan[i].angle);
			xy_vector.push_back(point);
		}
		double x_dif = xy_vector[0].x;
		double y_dif = xy_vector[0].y;
		double rot_ang = atan2(xy_vector[1].y-xy_vector[0].y, xy_vector[1].x-xy_vector[0].x);	//get rotational angle
		//ROS_INFO("rotational angle %f", rot_ang);
		for (int i = 0; i < xy_vector.size(); i++) {
			//rotate and move from robot to fixed coordinate system
			xy_vector[i].x = xy_vector[i].x - x_dif;	//move
			xy_vector[i].y = xy_vector[i].y - y_dif;
			//ROS_INFO("pole before rotating [%f %f]", xy_vector[i].x, xy_vector[i].y);
			double temp_x = xy_vector[i].x;
			xy_vector[i].x = cos(rot_ang)*temp_x + sin(rot_ang)*xy_vector[i].y;	//rotate
			xy_vector[i].y = -sin(rot_ang)*temp_x + cos(rot_ang)*xy_vector[i].y;
		}
		return xy_vector;
	}

	void Locate() {
		ros::Rate loop_rate(25);
		ros::spinOnce();
		std::vector<localization::scan_point> locate_scans;
		ExtractPoleScans(&locate_scans);	//get relevant scan points
		UpdatePoles(locate_scans);		//assign scans to respective poles
		PublishPoles();
		PublishPose();
		loop_rate.sleep();
	}

	void UpdatePoles(const std::vector<localization::scan_point> &scans_to_sort) {
		ros::Time current_time = ros::Time::now();
		for(int i = 0; i < scans_to_sort.size(); i++) {	//find closest pole for every scan
			double min_dist = 2000000;
			int index = -1;
			for (int j = 0; j < poles_.size(); j++) {
				localization::scan_point current_scan = poles_[j].laser_coords();
				double current_dist = pow(scans_to_sort[i].distance*cos(scans_to_sort[i].angle) - current_scan.distance*cos(current_scan.angle),2)
				+ pow(scans_to_sort[i].distance*sin(scans_to_sort[i].angle) - current_scan.distance*sin(current_scan.angle),2);
				if (current_dist < min_dist) {
					min_dist = current_dist;
					index = j;
				}
			}
			assert(index != -1);
			poles_[index].update(scans_to_sort[i], current_time);
		}
		for (int i = 0; i < poles_.size(); i++) {	//hide all missing poles
			if (poles_[i].time() != current_time) poles_[i].disappear();
		}
		//PrintPoleScanData();
		GetPose();
		EstimateIniviblePoles();
		PrintPose();
	}

	void GetPose() {
		std::vector<geometry_msgs::Pose> pose_vector;
		for (int i = 0; i < poles_.size(); i++) {		//loop over poles
			if (!poles_[i].visible()) continue;
			int j = i+1;
			while (!poles_[j].visible() && j < poles_.size()) j++;
			if (j > poles_.size()-1) break;
			calcPose(poles_[i], poles_[j], &pose_vector);
			i = j;
		}
		double x = 0, y = 0, theta = 0;
		//ROS_INFO("y: %f", y);
		for (int i = 0; i < pose_vector.size(); i++) {	//average over all results
			x += pose_vector[i].position.x;
			y += pose_vector[i].position.y;
			//ROS_INFO("y: %f", y);
			theta += tf::getYaw(pose_vector[i].orientation);
		}
		x /= pose_vector.size();
		y /= pose_vector.size();
		theta /= pose_vector.size();
		pose_.pose.position.x = x;
		pose_.pose.position.y = y;
		pose_.pose.position.z = 0;
		pose_.pose.orientation = tf::createQuaternionMsgFromYaw(theta);
		pose_.header.seq = 1;
		pose_.header.stamp = ros::Time::now();
		pose_.header.frame_id = "fixed_frame";
	}

	void EstimateIniviblePoles() {
		//ROS_INFO("Estimating poles");
		for (int i = 0; i < poles_.size(); i++) {
			if (!poles_[i].visible()) {
				double dx = pose_.pose.position.x - poles_[i].xy_coords().x;
				double dy = pose_.pose.position.y - poles_[i].xy_coords().y;
				localization::scan_point temp_scan;
				temp_scan.angle = atan2(dy,dx)+3.1415927-tf::getYaw(pose_.pose.orientation);
				CorrectAngle(temp_scan.angle);
				temp_scan.distance = pow(pow(dx,2)+pow(dy,2),0.5);
				poles_[i].update(temp_scan);
				//ROS_INFO("Changed pole %d to %f m %f rad", i, temp_scan.distance, temp_scan.angle);
			}
		}
		//ROS_INFO("Done estimating");
	}

	void PrintPose() {
		ROS_INFO("Averaged [%f %f] %f rad\n", pose_.pose.position.x, pose_.pose.position.y, tf::getYaw(pose_.pose.orientation));
	}

	void PrintPoleScanData() {
		for (int i = 0; i < poles_.size(); i++) 
			ROS_INFO("found pole%d at %f m %f rad", i, poles_[i].laser_coords().distance, poles_[i].laser_coords().angle);
	}

	void correctAngle(double& angle) {
    while(angle > kPi) angle -= 2*kPi;
    while(angle < -kPi) angle += 2*kPi;
  }

  void calcPose(const Pole &pole1, const Pole &pole2, std::vector<geometry_msgs::Pose> *pose_vector) {
    //pole coordinates
    double xp1 = pole1.xy_coords().x;
    double yp1 = pole1.xy_coords().y;
    double xp2 = pole2.xy_coords().x;
    double yp2 = pole2.xy_coords().y;

    //scan coordinates
    double a_dist = pole1.laser_coords().distance;
    double a_ang = pole1.laser_coords().angle;
    double b_dist = pole2.laser_coords().distance;
    double b_ang = pole2.laser_coords().angle;

    geometry_msgs::Pose temp_pose;
    temp_pose.position.z = 0;

    //calculate possible points
    const double D = pow((xp2-xp1)*(xp2-xp1)+(yp2-yp1)*(yp2-yp1),0.5);
    double to_root = (D+a_dist+b_dist)*(D+a_dist-b_dist)*(D-a_dist+b_dist)*(-D+a_dist+b_dist);	//to check if circles have intersection
    while (to_root < 0) {		//if no intersection slowly widen circles
    	a_dist += 0.001;
    	b_dist += 0.001;
    	to_root = (D+a_dist+b_dist)*(D+a_dist-b_dist)*(D-a_dist+b_dist)*(-D+a_dist+b_dist);
    	//ROS_INFO("corrected");
    }
    const double delta = 1.0/4*pow(to_root,0.5);
    double x1_circle = (xp1+xp2)/2+(xp2-xp1)*(a_dist*a_dist-b_dist*b_dist)/(2*D*D) + 2*(yp1-yp2)/(D*D)*delta;
    double x2_circle = (xp1+xp2)/2+(xp2-xp1)*(a_dist*a_dist-b_dist*b_dist)/(2*D*D) - 2*(yp1-yp2)/(D*D)*delta;
    double y1_circle = (yp1+yp2)/2+(yp2-yp1)*(a_dist*a_dist-b_dist*b_dist)/(2*D*D) - 2*(xp1-xp2)/(D*D)*delta;
    double y2_circle = (yp1+yp2)/2+(yp2-yp1)*(a_dist*a_dist-b_dist*b_dist)/(2*D*D) + 2*(xp1-xp2)/(D*D)*delta;
    
    //bot orientation for possible points
    double theta1_circle = kPi - a_ang + atan2(y1_circle-yp1,x1_circle-xp1);
    double theta2_circle = kPi - a_ang + atan2(y2_circle-yp1,x2_circle-xp1);
    //ROS_INFO("P1C [%f %f] %f", x1_circle, y1_circle, theta1_circle);
    //ROS_INFO("P2C [%f %f] %f", x2_circle, y2_circle, theta2_circle);
  
    

    if (pose_.pose.position.x != -2000) {	//use Newton Method
    	double theta_old = -2000;
    	double theta = tf::getYaw(pose_.pose.orientation);
    	while(abs(theta - theta_old) > 0.001) {
    		theta_old = theta;
    		//ROS_INFO("theta_old %f", theta_old);
    		double f_x = a_dist*cos(a_ang + theta_old -kPi) +xp1 -b_dist*cos(b_ang + theta_old - kPi) -xp2;
    		//ROS_INFO("f_x %15.15f", f_x);
    		double f_x_prime = -a_dist*sin(a_ang + theta_old -kPi) + b_dist*sin(b_ang + theta_old - kPi);
    		//ROS_INFO("f_x_prime %15.15f", f_x_prime);
    		theta = theta_old - (f_x/f_x_prime);
    		//ROS_INFO("Newton iteration");
    	}
    	double alpha1 = a_ang +theta -kPi;
    	double alpha2 = b_ang +theta -kPi;
    	double x1_newton = a_dist*cos(alpha1)+xp1;
    	double x2_newton = b_dist*cos(alpha2)+xp2;
    	double y1_newton = a_dist*sin(alpha1)+yp1;
    	double y2_newton = b_dist*sin(alpha2)+yp2;
    	double x_newton = (x1_newton+x2_newton)/2;
    	double y_newton = (y1_newton+y2_newton)/2;
    	double theta_newton = theta;
    	//ROS_INFO("P_N [%f %f] %f", x_newton, y_newton, theta_newton);
    	double check_dist1 = abs(pow(x1_circle-x_newton,2)+pow(y1_circle-y_newton,2));
    	double check_dist2 = abs(pow(x2_circle-x_newton,2)+pow(y2_circle-y_newton,2));
    	if (check_dist1 < check_dist2) {	//use newton's method to find best circle point
    		temp_pose.orientation = tf::createQuaternionMsgFromYaw(theta1_circle); 
    		temp_pose.position.x = x1_circle; 
    		temp_pose.position.y = y1_circle;
    	}
    	else {
    		temp_pose.orientation = tf::createQuaternionMsgFromYaw(theta2_circle); 
    		temp_pose.position.x = x2_circle; 
    		temp_pose.position.y = y2_circle;
    	}
    	/*temp_pose.orientation = tf::createQuaternionMsgFromYaw(theta_newton); 
    	temp_pose.position.x = x_newton; 
    	temp_pose.position.y = y_newton;*/
    }
    else {	//no Newton if first time
    	//check which pose is the correct one 
    	bool first = (kPi + atan2(y1_circle-yp2,x1_circle-xp2)-theta1_circle-b_ang < 0.1);
    	bool second = (kPi + atan2(y1_circle-yp2,x1_circle-xp2)-theta2_circle-b_ang < 0.1);
    	assert(first || second);
	  	correctAngle(theta1_circle);
	  	correctAngle(theta2_circle);
    	//put in correct pose
    	if (first) {
    		temp_pose.orientation = tf::createQuaternionMsgFromYaw(theta1_circle); 
    		temp_pose.position.x = x1_circle; 
    		temp_pose.position.y = y1_circle;
    	}
    	else {
    		temp_pose.orientation = tf::createQuaternionMsgFromYaw(theta2_circle); 
    		temp_pose.position.x = x2_circle; 
    		temp_pose.position.y = y2_circle;
    	}
    }

    //print pose 
    ROS_INFO("From poles %d,%d: [%f %f] %f rad", pole1.i(), pole2.i(), temp_pose.position.x, temp_pose.position.y, tf::getYaw(temp_pose.orientation));
    pose_vector->push_back(temp_pose);
  }


	void ExtractPoleScans(std::vector<localization::scan_point> *scan_pole_points) {
		scan_pole_points->clear();	//clear old scan points
		for (int i = 0; i < scan_.intensities.size(); i++) {
			//TODO: some kind of clever function for intensities
			if (scan_.intensities[i] > 1000) {
				localization::scan_point temp;
				temp.distance = scan_.ranges[i];
				temp.angle = (scan_.angle_min+scan_.angle_increment*i);
				scan_pole_points->push_back(temp);
			}
		}
		MinimizeScans(scan_pole_points);
	}

	void MinimizeScans(std::vector<localization::scan_point> *scan) {
		std::vector<localization::scan_point> target; 
		std::vector<int> trash;
		//don't run if no poles visible
		if (!scan->empty()) {
			//loop over all poles
			for (int i = 0; i < scan->size(); i++) {
				//don't run if pole is already done
				if(std::find(trash.begin(), trash.end(), i) != trash.end());
				else {
					//loop over remaining poles
					//check if point is last in vector
					target.push_back(scan->at(i));
					int ppp = 1;	//points per pole
					if(i+1 != scan->size()) for (int j = i+1; j < scan->size(); j++) {
						//check trash
						if(std::find(trash.begin(), trash.end(), j) != trash.end());
						else {
							//check if close enough
							if (abs((scan->at(i).angle - scan->at(j).angle)*scan->at(i).distance) < 0.2 
								&& abs(scan->at(i).distance - scan->at(j).distance) < 0.2) {
								ppp++;
								trash.push_back(j);
								target.back().angle += scan->at(j).angle;
								target.back().distance += scan->at(j).distance;
							}
						}
					}
					trash.push_back(i);
					//average
					//ROS_INFO("Found %d point/s for pole %d", ppp, i+1);
					target.back().distance /= ppp;
					target.back().angle /= ppp;
				}
				
			}
		}
		*scan = target;
	}

	void Callback(const sensor_msgs::LaserScan &scan) {
		scan_ = scan;
	}

};

int main(int argc, char **argv) {
	ros::init(argc, argv, "localization");
	Loc *loc = new Loc();
	ROS_INFO("Location node shutting down!");
}