#include "change_detection.h"

int DetectedObject::s_id = 0;


const float diff_dist = 0.01;
const float add_crop_static = 0.20; //the amount that should be added to each cluster in the static version when doing the crop
const float icp_max_corr_dist = 0.15;
const float icp_max_corr_dist_plane = 0.05;
const int icp_max_iter = 500;
const float icp_ransac_thr = 0.009;
const float overlap = 0.7;
const float color_weight = 0.25;
const float overlap_weight = 0.75;
const float min_score_thr = 0.6;

bool comparePoint(std::pair<PointNormal, int> p1, std::pair<PointNormal, int> p2){
    if (!pcl::isFinite(p1.first))
        return false;
    else if (!pcl::isFinite(p2.first))
        return true;
    if (p1.first.x != p2.first.x)
        return p1.first.x > p2.first.x;
    else if (p1.first.y != p2.first.y)
        return  p1.first.y > p2.first.y;
    else
        return p1.first.z > p2.first.z;
}

void ChangeDetection::init(pcl::PointCloud<PointNormal>::Ptr ref_cloud, pcl::PointCloud<PointNormal>::Ptr curr_cloud,
                           Eigen::Vector4f ref_plane_coeffs, Eigen::Vector4f curr_plane_coeffs,
                           pcl::PointCloud<pcl::PointXYZ>::Ptr ref_convex_hull_pts, pcl::PointCloud<pcl::PointXYZ>::Ptr curr_convex_hull_pts,
                           std::string output_path) {
    ref_cloud_= ref_cloud;
    curr_cloud_ = curr_cloud;
    ref_plane_coeffs_ = ref_plane_coeffs;
    curr_plane_coeffs_ = curr_plane_coeffs;
    curr_convex_hull_pts_ = curr_convex_hull_pts;
    ref_convex_hull_pts_ = ref_convex_hull_pts;
    output_path_ = output_path;
}


void ChangeDetection::compute(std::vector<DetectedObject> &ref_result, std::vector<DetectedObject> &curr_result) {

    if (ref_cloud_->empty() || curr_cloud_->empty()) {
        std::cerr << "You have to call ChangeDetection::init before calling ChangeDetection::compute !!" << std::endl;
        return;
    }

    refineNormals(curr_cloud_);
    refineNormals(ref_cloud_);

    //----------------------------downsample input clouds for faster computation-------
    double ds_leaf_size = 0.01;
    pcl::PointCloud<PointNormal>::Ptr curr_cloud_downsampled (new pcl::PointCloud<PointNormal>);
    pcl::PointCloud<PointNormal>::Ptr ref_cloud_downsampled (new pcl::PointCloud<PointNormal>);
    curr_cloud_downsampled = downsampleCloud(curr_cloud_, ds_leaf_size);
    ref_cloud_downsampled = downsampleCloud(ref_cloud_, ds_leaf_size);

    //---------------------------------------------------------------------------------


    //call computeObjectsOnPlanes twice with different input.
    //The result contains points of clusters detected on top of the main plane
    //---Current objects------
    std::string curr_res_path =  output_path_ + "/curr_cloud/";
    boost::filesystem::create_directories(curr_res_path);
    ExtractObjectsFromPlanes extract_curr_objects(curr_cloud_downsampled, curr_plane_coeffs_, curr_convex_hull_pts_, curr_res_path);
    PlaneWithObjInd curr_objects_merged = extract_curr_objects.computeObjectsOnPlanes();

    PointNormal nan_point;
    nan_point.x = nan_point.y = nan_point.z = std::numeric_limits<float>::quiet_NaN();
    pcl::PointCloud<PointNormal>::Ptr curr_objects_plane_cloud (new pcl::PointCloud<PointNormal>(curr_cloud_downsampled->width, curr_cloud_downsampled->height, nan_point));
    const std::vector<int> &curr_object_ind = curr_objects_merged.obj_indices;
    for (size_t i = 0; i < curr_object_ind.size(); i++) {
        curr_objects_plane_cloud->points.at(curr_object_ind[i]) = curr_cloud_downsampled->points.at(curr_object_ind[i]);
    }
    std::vector<pcl::PointIndices> pot_object_ind = cleanResult(curr_objects_plane_cloud, 0.02);
    std::vector<PlaneWithObjInd> curr_objects;
    //cluster detected objects in separate objects
    for (size_t i = 0; i < pot_object_ind.size(); i++) {
        PlaneWithObjInd pot_object;
        pot_object.plane = curr_objects_merged.plane;
        pot_object.obj_indices = pot_object_ind[i].indices;
        curr_objects.push_back(pot_object);
    }
    //mergeObjects(curr_objects); //this is necessary here because it could happen that the same object was extracted twice from slightly different planes

    //---Reference objects---
    std::string ref_res_path =  output_path_ + "/ref_cloud";
    boost::filesystem::create_directories(ref_res_path);
    ExtractObjectsFromPlanes extract_ref_objects(ref_cloud_downsampled, ref_plane_coeffs_, ref_convex_hull_pts_, ref_res_path);
    PlaneWithObjInd ref_objects_merged = extract_ref_objects.computeObjectsOnPlanes();
    pcl::PointCloud<PointNormal>::Ptr ref_objects_plane_cloud (new pcl::PointCloud<PointNormal>(ref_cloud_downsampled->width, ref_cloud_downsampled->height, nan_point));
    const std::vector<int> &ref_object_ind = ref_objects_merged.obj_indices;
    for (size_t i = 0; i < ref_object_ind.size(); i++) {
        ref_objects_plane_cloud->points.at(ref_object_ind[i]) = ref_cloud_downsampled->points.at(ref_object_ind[i]);
    }
    pot_object_ind = cleanResult(ref_objects_plane_cloud, 0.02);
    std::vector<PlaneWithObjInd> ref_objects;
    //cluster detected objects in separate objects
    for (size_t i = 0; i < pot_object_ind.size(); i++) {
        PlaneWithObjInd pot_object;
        pot_object.plane = ref_objects_merged.plane;
        pot_object.obj_indices = pot_object_ind[i].indices;
        ref_objects.push_back(pot_object);
    }
    //mergeObjects(ref_objects); //this is necessary here because it could happen that the same object was extracted twice from slightly different planes

    pcl::io::savePCDFileBinary(curr_res_path + "/objects_from_plane.pcd", *curr_objects_plane_cloud);
    pcl::io::savePCDFileBinary(ref_res_path + "/objects_from_plane.pcd", *ref_objects_plane_cloud);

    //------------------------------LOCAL VERIFICATION IF WANTED--------------------------------------------------------
    if (do_LV_before_matching && curr_objects.size() != 0 && ref_objects.size() != 0) {
        //TODO fill static_objects in the result objects
        LocalObjectVerificationParams lv_params{diff_dist, add_crop_static, icp_max_corr_dist, icp_max_corr_dist_plane,
                    icp_max_iter, icp_ransac_thr, color_weight, overlap_weight, min_score_thr};
        //local object verification
        LocalObjectVerification local_verification(curr_objects, ref_cloud_downsampled, curr_cloud_downsampled, lv_params);
        local_verification.setDebugOutputPath(curr_res_path);
        std::vector<PlaneWithObjInd> novel_objects = local_verification.verify_changed_objects();
        pcl::PointCloud<PointNormal>::Ptr novel_objects_cloud = fromObjectVecToObjectCloud(novel_objects, curr_cloud_downsampled);
        cleanResult(novel_objects_cloud, 0.02);
        pcl::io::savePCDFileBinary(curr_res_path + "/result_after_LV.pcd", *novel_objects_cloud);

        //filter out objects with similar color to supporting plane and are planar
        std::string objects_corr_planar_path = curr_res_path + "/objects_with_corr_planar/";
        boost::filesystem::create_directories(objects_corr_planar_path);
        filterPlanarAndColor(novel_objects, curr_cloud_downsampled, objects_corr_planar_path);
        novel_objects_cloud = fromObjectVecToObjectCloud(novel_objects, curr_cloud_downsampled);
        pcl::io::savePCDFileBinary(curr_res_path + "/result_after_LV_filtering_planar_objects.pcd", *novel_objects_cloud);
        objectRegionGrowing(curr_cloud_downsampled, novel_objects, 3000);  //this removes very big clusters after growing
        mergeObjects(novel_objects); //in case an object was detected several times (disjoint sets originally, but prob. overlapping after region growing)
        novel_objects_cloud = fromObjectVecToObjectCloud(novel_objects, curr_cloud_downsampled);
        pcl::io::savePCDFileBinary(curr_res_path + "/result_after_LV_objectGrowing.pcd", *novel_objects_cloud);

        curr_objects = novel_objects;


        //verify removed objects
        local_verification.setCurrCloud(ref_cloud_downsampled);
        local_verification.setRefCloud(curr_cloud_downsampled);
        local_verification.setPotentialObjects(ref_objects);
        local_verification.setDebugOutputPath(ref_res_path);
        std::vector<PlaneWithObjInd> disappeared_objects = local_verification.verify_changed_objects();
        pcl::PointCloud<PointNormal>::Ptr disappeared_objects_cloud = fromObjectVecToObjectCloud(disappeared_objects, ref_cloud_downsampled);
        cleanResult(disappeared_objects_cloud, 0.02);
        pcl::io::savePCDFileBinary(ref_res_path + "/result_after_LV.pcd", *disappeared_objects_cloud);

        objects_corr_planar_path = ref_res_path + "/objects_with_corr_planar/";
        boost::filesystem::create_directories(objects_corr_planar_path);
        filterPlanarAndColor(disappeared_objects, ref_cloud_downsampled, objects_corr_planar_path);
        disappeared_objects_cloud = fromObjectVecToObjectCloud(disappeared_objects, ref_cloud_downsampled);
        pcl::io::savePCDFileBinary(ref_res_path + "/result_after_LV_filtering_planar_objects.pcd", *disappeared_objects_cloud);
        objectRegionGrowing(ref_cloud_downsampled, disappeared_objects, 3000);  //this removes very big clusters after growing
        mergeObjects(disappeared_objects); //in case an object was detected several times (disjoint sets originally, but prob. overlapping after region growing)
        disappeared_objects_cloud = fromObjectVecToObjectCloud(disappeared_objects, ref_cloud_downsampled);
        pcl::io::savePCDFileBinary(ref_res_path + "/result_after_LV_objectGrowing.pcd", *disappeared_objects_cloud);

        ref_objects = disappeared_objects;
    }


    //----------------Upsample again to have the objects and planes in full resolution----------
    upsampleObjectsAndPlanes(curr_cloud_, curr_cloud_downsampled, curr_objects, ds_leaf_size, curr_res_path);
    upsampleObjectsAndPlanes(ref_cloud_, ref_cloud_downsampled, ref_objects, ds_leaf_size, ref_res_path);
    pcl::PointCloud<PointNormal>::Ptr novel_objects_cloud = fromObjectVecToObjectCloud(curr_objects, curr_cloud_);
    pcl::io::savePCDFileBinary(curr_res_path + "/eval_sem_plane_final_orig.pcd", *novel_objects_cloud);
    pcl::PointCloud<PointNormal>::Ptr disappeared_objects_cloud = fromObjectVecToObjectCloud(ref_objects, ref_cloud_);
    pcl::io::savePCDFileBinary(ref_res_path + "/eval_sem_plane_final_orig.pcd", *disappeared_objects_cloud);

    //save each object including its supporting plane
    std::string objects_with_planes_path = curr_res_path + "/final_objects_with_planes/";
    boost::filesystem::create_directories(objects_with_planes_path);
    saveObjectsWithPlanes(objects_with_planes_path, curr_objects, curr_cloud_); //save only objects with size > 15 (similar to cleanResult())
    objects_with_planes_path = ref_res_path + "/final_objects_with_planes/";
    boost::filesystem::create_directories(objects_with_planes_path);
    saveObjectsWithPlanes(objects_with_planes_path, ref_objects, ref_cloud_); //save only objects with size > 15 (similar to cleanResult())


    //------------------Match detected objects against each other with PPF-----------------------
    //depending on if LV was performed before, static objects can be matched
    std::string model_path = output_path_ + "/model_objects/";
    std::vector<DetectedObject> ref_objects_vec;
    for (size_t o = 0; o < ref_objects.size(); o++) {
        pcl::PointCloud<PointNormal>::Ptr object_cloud{new pcl::PointCloud<PointNormal>};
        pcl::PointCloud<PointNormal>::Ptr plane_cloud{new pcl::PointCloud<PointNormal>};

        pcl::ExtractIndices<PointNormal> extract;
        extract.setInputCloud (ref_cloud_);
        extract.setIndices (ref_objects[o].plane.plane_ind);
        extract.setNegative (false);
        extract.setKeepOrganized(false);
        extract.filter(*plane_cloud);

        pcl::PointIndices::Ptr obj_ind(new pcl::PointIndices);
        obj_ind->indices = ref_objects[o].obj_indices;
        extract.setIndices(obj_ind);
        extract.filter(*object_cloud);

        DetectedObject obj(object_cloud, plane_cloud, ref_objects[o].plane);
        std::string obj_folder = model_path + std::to_string(obj.getID()); //PPF uses the folder name as model_id!
        boost::filesystem::create_directories(obj_folder);
        pcl::io::savePCDFile(obj_folder + "/3D_model.pcd", *object_cloud); //each detected reference object is saved in a folder for further use with PPF
        obj.object_folder_path_ = obj_folder;
        ref_objects_vec.push_back(obj);
    }
    if (curr_objects.size() == 0) { //all objects removed from ref
        for (size_t i = 0; i < ref_objects_vec.size(); i++) {
            ref_objects_vec[i].state_ = ObjectState::REMOVED;
        }
        ref_result = ref_objects_vec;
        return;
    }

    std::vector<DetectedObject> curr_objects_vec;
    for (size_t o = 0; o < curr_objects.size(); o++) {
        pcl::PointCloud<PointNormal>::Ptr object_cloud{new pcl::PointCloud<PointNormal>};
        pcl::PointCloud<PointNormal>::Ptr plane_cloud{new pcl::PointCloud<PointNormal>};

        pcl::ExtractIndices<PointNormal> extract;
        extract.setInputCloud (curr_cloud_);
        extract.setIndices (curr_objects[o].plane.plane_ind);
        extract.setNegative (false);
        extract.setKeepOrganized(false);
        extract.filter(*plane_cloud);

        pcl::PointIndices::Ptr obj_ind(new pcl::PointIndices);
        obj_ind->indices = curr_objects[o].obj_indices;
        extract.setIndices(obj_ind);
        extract.filter(*object_cloud);

        DetectedObject obj(object_cloud, plane_cloud, curr_objects[o].plane);
        curr_objects_vec.push_back(obj);
    }

    if (ref_objects.size() == 0) { //all objects are new in curr
        for (size_t i = 0; i < curr_objects_vec.size(); i++) {
            curr_objects_vec[i].state_ = ObjectState::NEW;
        }
        curr_result = curr_objects_vec;
        return;
    }


    //matches between same plane different timestamps
    //TODO different code if LV was performed
    ObjectMatching object_matching(ref_objects_vec, curr_objects_vec, model_path, ppf_config_path_);
    std::vector<Match> matches = object_matching.compute(ref_result, curr_result);



    //Visualize two clouds next to each other. Reference cloud showing disappeared objects.
    //Current cloud showing novel objects. Both showing displaced objects.
//    ObjectVisualization vis(ref_cloud_, disappeared_objects_cloud, curr_cloud_, novel_objects_cloud);
//    vis.visualize();

}

// v4r/common/Downsampler.cpp has a more advanced method if normals are given
pcl::PointCloud<PointNormal>::Ptr ChangeDetection::downsampleCloud(pcl::PointCloud<PointNormal>::Ptr input, double leafSize)
{
    std::cout << "PointCloud before filtering has: " << input->points.size () << " data points." << std::endl;

    // Create the filtering object: downsample the dataset using a leaf size of 1cm
    pcl::VoxelGrid<PointNormal> vg;
    pcl::PointCloud<PointNormal>::Ptr cloud_filtered (new pcl::PointCloud<PointNormal>);
    vg.setInputCloud (input);
    vg.setLeafSize (leafSize, leafSize, leafSize);
    vg.setDownsampleAllData(true);
    vg.filter (*cloud_filtered);
    std::cout << "PointCloud after filtering has: " << cloud_filtered->points.size ()  << " data points." << std::endl;

    return cloud_filtered;
}

void ChangeDetection::upsampleObjectsAndPlanes(pcl::PointCloud<PointNormal>::Ptr orig_cloud, pcl::PointCloud<PointNormal>::Ptr ds_cloud,
                                               std::vector<PlaneWithObjInd> &objects, double leaf_size, std::string res_path) {
    pcl::octree::OctreePointCloudSearch<PointNormal>::Ptr octree;
    octree.reset(new pcl::octree::OctreePointCloudSearch<PointNormal>(leaf_size));
    octree->setInputCloud(orig_cloud);
    octree->addPointsFromInputCloud();
    std::cout << "Created octree" << std::endl;

    for (size_t i = 0; i < objects.size(); i++) {
        //upsample plane indices
        pcl::PointIndices::Ptr plane_indices = objects[i].plane.plane_ind;
        pcl::PointCloud<PointNormal>::Ptr plane_ds_cloud(new pcl::PointCloud<PointNormal>);
        //extract object indices from downsampled cloud
        pcl::ExtractIndices<PointNormal> extract_object_ind;
        extract_object_ind.setInputCloud (ds_cloud);
        extract_object_ind.setIndices(plane_indices);
        extract_object_ind.setKeepOrganized(false);
        extract_object_ind.setNegative (false);
        extract_object_ind.filter (*plane_ds_cloud);
        std::tuple<pcl::PointCloud<PointNormal>::Ptr, std::vector<int> > obj_cloud_ind_tuple = upsampleObjects(octree, orig_cloud, plane_ds_cloud, res_path, i);
        objects[i].plane.plane_ind->indices = std::get<1>(obj_cloud_ind_tuple);

        PointNormal minPt_object, maxPt_object;
        pcl::getMinMax3D (*std::get<0>(obj_cloud_ind_tuple), minPt_object, maxPt_object);

        /// crop cloud
        float crop_margin = 0.5; //this margin is not so important it is just reducing the number of points for the next step
        pcl::PointCloud<PointNormal>::Ptr cloud_crop(new pcl::PointCloud<PointNormal>);
        pcl::PassThrough<PointNormal> pass;
        pass.setInputCloud(orig_cloud);
        pass.setFilterFieldName("z");
        pass.setFilterLimits(minPt_object.z, maxPt_object.z + crop_margin);
        pass.setKeepOrganized(true);
        pass.filter(*cloud_crop);

        //make the plane a little thicker. these plane inliers are then checked against object indices to remove overlapping points
        pcl::SampleConsensusModelPlane<PointNormal>::Ptr dit (new pcl::SampleConsensusModelPlane<PointNormal> (cloud_crop));
        std::vector<int> inliers;
        Eigen::Vector4f vec_coeff (objects[i].plane.coeffs->values.data ()); //from pcl::ModelCoefficients to Eigen::Vector
        dit -> selectWithinDistance (vec_coeff, leaf_size/2, inliers);

        //also object indices need to be upsampled (this could lead to a state where a point is assigned to an object AND the plane!)
        std::vector<int> obj_indices = objects[i].obj_indices;
        pcl::PointCloud<PointNormal>::Ptr object_ds_cloud(new pcl::PointCloud<PointNormal>);
        //extract object indices from downsampled cloud
        extract_object_ind.setInputCloud (ds_cloud);
        extract_object_ind.setIndices(boost::make_shared<std::vector<int> > (obj_indices));
        extract_object_ind.setKeepOrganized(false);
        extract_object_ind.setNegative (false);
        extract_object_ind.filter (*object_ds_cloud);
        //pcl::io::savePCDFileBinary(res_path + "/ds_objects" + std::to_string(i)+ ".pcd", *object_ds_cloud);
        obj_cloud_ind_tuple= upsampleObjects(octree, orig_cloud, object_ds_cloud, res_path, i);
        std::vector<int> ind =get<1>(obj_cloud_ind_tuple);

        //remove points that are detected as planes and object from the object. we don't want plane points in the object for better feature detection
        std::unordered_set<int> set(ind.begin(), ind.end());
        for (auto a : inliers) {
            set.erase(a); //erase element if it exists in the set
        }
        objects[i].obj_indices.clear();
        std::copy(set.begin(), set.end(), back_inserter(objects[i].obj_indices));

    }
}

std::tuple<pcl::PointCloud<PointNormal>::Ptr, std::vector<int> > ChangeDetection::upsampleObjects(pcl::octree::OctreePointCloudSearch<PointNormal>::Ptr octree, pcl::PointCloud<PointNormal>::Ptr orig_input_cloud,
                                                                                                  pcl::PointCloud<PointNormal>::Ptr objects_ds_cloud, std::string output_path, int counter) {

    /// instead we just add all points wihtin 1 cm to the object points
    std::vector<int> nn_indices;
    std::vector<float> nn_distances;
    std::vector<int> orig_object_ind;

    for (size_t i = 0; i < objects_ds_cloud->points.size(); ++i) {
        if (!pcl::isFinite(objects_ds_cloud->points[i]))
            continue;

        PointNormal &p_object = objects_ds_cloud->points[i];

        if (octree->radiusSearch(p_object, octree->getResolution(), nn_indices, nn_distances) > 0){
            for (size_t j = 0; j < nn_indices.size(); j++) {
                orig_object_ind.push_back(nn_indices[j]);
            }
        }
    }
    std::sort(orig_object_ind.begin(), orig_object_ind.end());
    orig_object_ind.erase(std::unique(orig_object_ind.begin(), orig_object_ind.end()), orig_object_ind.end());

    pcl::PointCloud<PointNormal>::Ptr orig_object(new pcl::PointCloud<PointNormal>);
    //extract object indices from original cloud
    pcl::ExtractIndices<PointNormal> extract_object_ind;
    extract_object_ind.setInputCloud (orig_input_cloud);
    extract_object_ind.setIndices(boost::make_shared<std::vector<int> > (orig_object_ind));
    extract_object_ind.setKeepOrganized(true);
    extract_object_ind.setNegative (false);
    extract_object_ind.filter (*orig_object);
    pcl::io::savePCDFileBinary(output_path + "/object_orig" + std::to_string(counter)+ ".pcd", *orig_object);

    std::tuple<pcl::PointCloud<PointNormal>::Ptr, std::vector<int> > obj_cloud_ind_tuple;
    obj_cloud_ind_tuple = std::make_tuple(orig_object, orig_object_ind);

    return obj_cloud_ind_tuple;
}

void ChangeDetection::objectRegionGrowing(pcl::PointCloud<PointNormal>::Ptr cloud, std::vector<PlaneWithObjInd> &objects, int max_object_size) {
    for (size_t i = 0; i < objects.size(); i++) {
        pcl::PointCloud<PointNormal>::Ptr object_cloud(new pcl::PointCloud<PointNormal>);
        for (size_t p = 0; p < objects[i].obj_indices.size(); p++) {
            object_cloud->points.push_back(cloud->points[objects[i].obj_indices[p]]);
        }
        PointNormal minPt_object, maxPt_object;
        pcl::getMinMax3D (*object_cloud, minPt_object, maxPt_object);

        /// crop cloud
        float crop_margin = 0.5; //choosing a larger crop margin lets objects grow larger (especially FP objects) that are then filtered out
        pcl::PointCloud<PointNormal>::Ptr cloud_crop(new pcl::PointCloud<PointNormal>);
        pcl::PassThrough<PointNormal> pass;
        pass.setInputCloud(cloud);
        pass.setFilterFieldName("x");
        pass.setFilterLimits(minPt_object.x - crop_margin, maxPt_object.x + crop_margin);
        pass.setKeepOrganized(true);
        pass.filter(*cloud_crop);
        pass.setInputCloud(cloud_crop);
        pass.setFilterFieldName("y");
        pass.setFilterLimits(minPt_object.y - crop_margin, maxPt_object.y + crop_margin);
        pass.setKeepOrganized(true);
        pass.filter(*cloud_crop);
        pass.setInputCloud(cloud_crop);
        pass.setFilterFieldName("z");
        pass.setFilterLimits(minPt_object.z - crop_margin, maxPt_object.z + crop_margin);
        pass.setKeepOrganized(true);
        pass.filter(*cloud_crop);

        //remove supporting plane
        //we do not use the plane points directly because there is a chance that they do not cover the whole plane.
        //This is because the plane extraction part operates on semantic segmentation and if one plane is assigned to several labels, they are not part of the detected plane.
        pcl::SampleConsensusModelPlane<PointType>::Ptr dit (new pcl::SampleConsensusModelPlane<PointType> (cloud_crop));
        std::vector<int> inliers;
        Eigen::Vector4f vec_coeff (objects[i].plane.coeffs->values.data ()); //from pcl::ModelCoefficients to Eigen::Vector
        dit -> selectWithinDistance (vec_coeff, 0.01, inliers);
        PointNormal nan_point;
        nan_point.x = nan_point.y = nan_point.z = std::numeric_limits<float>::quiet_NaN();
        for (size_t p = 0; p < inliers.size(); p++) {
            cloud_crop->points[inliers[p]] = nan_point;
        }

        //        pcl::io::savePCDFileBinary("/home/edith/test/crop_no_plane" + std::to_string(i) + ".pcd", *cloud_crop);
        //        pcl::io::savePCDFileBinary("/home/edith/test/object_cloud" + std::to_string(i) + ".pcd", *object_cloud);


        pcl::PointCloud<pcl::Normal>::Ptr scene_normals (new pcl::PointCloud<pcl::Normal>);
        pcl::copyPointCloud(*cloud_crop, *scene_normals);

        //call the region growing method and extract upsampled object
        RegionGrowing<PointNormal, PointNormal> region_growing(cloud_crop, object_cloud, scene_normals, true);
        std::vector<int> orig_object_ind = region_growing.compute();

        pcl::PointCloud<PointNormal>::Ptr orig_object_cloud(new pcl::PointCloud<PointNormal>);
        //extract object indices from downsampled cloud
        pcl::ExtractIndices<PointNormal> extract_object_ind;
        extract_object_ind.setInputCloud (cloud_crop);
        extract_object_ind.setIndices(boost::make_shared<std::vector<int> > (orig_object_ind));
        extract_object_ind.setKeepOrganized(false);
        extract_object_ind.setNegative (false);
        extract_object_ind.filter (*orig_object_cloud);

        PointNormal minPt_orig_object, maxPt_orig_object;
        pcl::getMinMax3D (*orig_object_cloud, minPt_orig_object, maxPt_orig_object);

        if (std::abs(maxPt_orig_object.x - minPt_orig_object.x) < 10*std::abs(maxPt_object.x - minPt_object.x) &&
                std::abs(maxPt_orig_object.y - minPt_orig_object.y) < 10*std::abs(maxPt_object.y - minPt_object.y) &&
                std::abs(maxPt_orig_object.z - minPt_orig_object.z) < 10*std::abs(maxPt_object.z - minPt_object.z) ) //otherwise something went wrong with the region growing (expanded too much)
            objects[i].obj_indices = orig_object_ind;
    }
    std::cout << "Original number of objects: " << objects.size() << ", objects after region growing (big ones got filtered): ";
    objects.erase(
                std::remove_if(
                    objects.begin(),
                    objects.end(),
                    [&](PlaneWithObjInd const & p) { return p.obj_indices.size() > max_object_size; }
                ),
            objects.end()
            );
    std::cout << objects.size() << std::endl;
}

pcl::PointCloud<PointNormal>::Ptr ChangeDetection::fromObjectVecToObjectCloud(const std::vector<PlaneWithObjInd> objects, pcl::PointCloud<PointNormal>::Ptr cloud) {
    PointNormal nan_point;
    nan_point.x = nan_point.y = nan_point.z = std::numeric_limits<float>::quiet_NaN();

    pcl::PointCloud<PointNormal>::Ptr objects_cloud (new pcl::PointCloud<PointNormal>(cloud->width, cloud->height, nan_point));
    for (size_t o = 0; o < objects.size(); o++) {
        std::vector<int> obj_ind =  objects[o].obj_indices;
        for (size_t i = 0; i < obj_ind.size(); i++) {
            PointType &p = cloud->points.at(obj_ind.at(i));
            objects_cloud->points[obj_ind[i]] = p;
        }
    }
    return objects_cloud;
}

void ChangeDetection::saveObjectsWithPlanes(std::string path, const std::vector<PlaneWithObjInd> objects, pcl::PointCloud<PointNormal>::Ptr cloud) {
    PointLabel nan_point;
    nan_point.x = nan_point.y = nan_point.z = std::numeric_limits<float>::quiet_NaN();

    for (size_t o = 0; o < objects.size(); o++) {
        pcl::PointCloud<PointLabel>::Ptr res_cloud (new pcl::PointCloud<PointLabel>(cloud->width, cloud->height, nan_point));
        std::vector<int> obj_ind =  objects[o].obj_indices;
        if (obj_ind.size() < 15)
            continue;
        for (size_t i = 0; i < obj_ind.size(); i++) {
            const PointNormal &p = cloud->points.at(obj_ind.at(i));
            res_cloud->points[obj_ind[i]].x = p.x;
            res_cloud->points[obj_ind[i]].y = p.y;
            res_cloud->points[obj_ind[i]].z = p.z;
            res_cloud->points[obj_ind[i]].rgb = p.rgb;
            res_cloud->points[obj_ind[i]].label = 0; //label 0 for object
        }
        const std::vector<int> &plane_ind = objects[o].plane.plane_ind->indices;
        for (size_t i = 0; i < plane_ind.size(); i++) {
            const PointNormal &p = cloud->points.at(plane_ind.at(i));
            res_cloud->points[plane_ind[i]].x = p.x;
            res_cloud->points[plane_ind[i]].y = p.y;
            res_cloud->points[plane_ind[i]].z = p.z;
            res_cloud->points[plane_ind[i]].rgb = p.rgb;
            res_cloud->points[plane_ind[i]].label = 1; //label 1 for plane
        }
        pcl::io::savePCDFileBinary(path + "object" + std::to_string(o) + ".pcd", *res_cloud);
    }
}

//this method changes the object vector!
void ChangeDetection::mergeObjects(std::vector<PlaneWithObjInd>& objects) {
    std::cout << "Number of objects before merging " << objects.size() << std::endl;
    //sort point indices for further operations
    for (size_t o = 0; o < objects.size(); o++) {
        std::sort(objects[o].obj_indices.begin(), objects[o].obj_indices.end());
    }

    //iterate through all object and check if any other object has a high enough overlap of same point indices
    //if so, merge the point indices of the two objects (without duplicates) and remove the later one.
    for(std::vector<PlaneWithObjInd>::iterator it = objects.begin(), end = objects.end(); it != end;) {
        std::vector<PlaneWithObjInd>::iterator it_inner = it;
        for(++it_inner; it_inner != end;) {
            std::vector<int> &o_ind = it->obj_indices;
            std::vector<int> &i_ind = it_inner->obj_indices;
            std::vector<int> intersection;
            std::set_intersection(o_ind.begin(),o_ind.end(),
                                  i_ind.begin(),i_ind.end(),
                                  back_inserter(intersection));
            if (intersection.size() > 0) {
                if (o_ind.size()/intersection.size() > 0.8 || i_ind.size()/intersection.size() > 0.8) { //probably the same object
                    o_ind.insert(o_ind.end(), i_ind.begin(), i_ind.end());
                    std::sort(o_ind.begin(), o_ind.end());
                    o_ind.erase(unique(o_ind.begin(), o_ind.end()), o_ind.end());
                    it_inner = objects.erase(it_inner);
                    end = objects.end();
                }
            } else {
                it_inner++;
            }
        }
        it++;
    }
    std::cout << "Number of objects after merging " << objects.size() << std::endl;
}



double ChangeDetection::checkColorSimilarity(PlaneWithObjInd& object, pcl::PointCloud<PointNormal>::Ptr cloud, int _nr_bins) {
    int nr_bins = _nr_bins;
    pcl::PointCloud<PointNormal>::Ptr object_cloud (new pcl::PointCloud<PointNormal>);
    pcl::PointCloud<PointNormal>::Ptr supp_plane_cloud (new pcl::PointCloud<PointNormal>);
    std::vector<int> obj_ind =  object.obj_indices;
    for (size_t i = 0; i < obj_ind.size(); i++) {
        PointType &p = cloud->points.at(obj_ind.at(i));
        object_cloud->push_back(p);
    }
    std::vector<int> plane_ind =  object.plane.plane_ind->indices;
    for (size_t i = 0; i < plane_ind.size(); i++) {
        PointType &p = cloud->points.at(plane_ind.at(i));
        supp_plane_cloud->push_back(p);
    }

    //crop supp. plane to get only points around the object
    PointNormal minPt, maxPt;
    pcl::getMinMax3D(*object_cloud, minPt, maxPt);

    /// crop cloud
    float crop_margin = 0.05;
    pcl::PointCloud<PointNormal>::Ptr plane_crop(new pcl::PointCloud<PointNormal>);
    pcl::PassThrough<PointNormal> pass;

    pass.setInputCloud(supp_plane_cloud);
    pass.setFilterFieldName("x");
    pass.setFilterLimits(minPt.x - crop_margin, maxPt.x + crop_margin);
    pass.setKeepOrganized(false);
    pass.filter(*plane_crop);
    pass.setInputCloud(plane_crop);
    pass.setFilterFieldName("y");
    pass.setFilterLimits(minPt.y - crop_margin, maxPt.y + crop_margin);
    pass.filter(*plane_crop);

    ColorHistogram color_hist;
    double corr = color_hist.colorCorr(object_cloud, plane_crop, nr_bins);

    return corr;
}

std::vector<double> ChangeDetection::filterBasedOnColor(std::vector<PlaneWithObjInd>& objects, pcl::PointCloud<PointNormal>::Ptr cloud, int _nr_bins) {
    std::vector<int> obj_ind_to_del;
    std::vector<double> correlations;

    std::cout << "Number of objects before deleting based on colors " << objects.size() << std::endl;
    for (size_t o = 0; o < objects.size(); o++) {
        std::cout << "Check object number " << o << " for color similarity" << std::endl;
        correlations.push_back(checkColorSimilarity(objects[o], cloud, _nr_bins));

        //        pcl::PointCloud<PointNormal>::Ptr combined_cloud(new pcl::PointCloud<PointNormal>);
        //        *combined_cloud += *object_cloud;
        //        *combined_cloud += *plane_crop;
        //        pcl::io::savePCDFileBinary(path + "/object" + std::to_string(o) + "_corr_" + std::to_string(corr)+ ".pcd", *combined_cloud);
        //        pcl::io::savePCDFileBinary(path + "/object" + std::to_string(o) + "_object.pcd", *object_cloud);
        //        pcl::io::savePCDFileBinary(path + "/object" + std::to_string(o) + "plane.pcd", *plane_crop);
    }
    //now remove objects by index starting with the highest index in the list
    for (auto i = obj_ind_to_del.rbegin(); i != obj_ind_to_del.rend(); ++ i)
    {
        objects.erase(objects.begin() + *i);
    }
    std::cout << "Number of objects after deleting based on colors " << objects.size() << std::endl;
    return correlations;
}

//returns the number of plane points (plane inlier index does not correspond to points in original cloud!)
int ChangeDetection::checkPlanarity (PlaneWithObjInd& object, pcl::PointCloud<PointNormal>::Ptr cloud, float _plane_dist_thr) {
    float plane_dist_thr = _plane_dist_thr;

    pcl::PointCloud<PointNormal>::Ptr object_cloud (new pcl::PointCloud<PointNormal>);
    std::vector<int> obj_ind =  object.obj_indices;
    for (size_t i = 0; i < obj_ind.size(); i++) {
        PointType &p = cloud->points.at(obj_ind.at(i));
        object_cloud->push_back(p);
    }
    pcl::SACSegmentation<PointNormal> seg;
    pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
    pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients);

    seg.setOptimizeCoefficients (true);
    seg.setModelType (pcl::SACMODEL_PLANE);
    seg.setMethodType (pcl::SAC_RANSAC);
    seg.setMaxIterations (100);
    seg.setDistanceThreshold (plane_dist_thr);
    seg.setInputCloud (object_cloud);
    seg.segment (*inliers, *coefficients);

    std::cout << "plane points: " << inliers->indices.size() << "; object points threshold " <<  0.9 * obj_ind.size() << std::endl;

    return inliers->indices.size();
}

std::vector<int> ChangeDetection::removePlanarObjects (std::vector<PlaneWithObjInd>& objects, pcl::PointCloud<PointNormal>::Ptr cloud, float _plane_dist_thr) {
    float plane_dist_thr = _plane_dist_thr;
    std::vector<int> obj_ind_to_del;
    std::vector<int> inlier_size_res;

    std::cout << "Number of objects before deleting planar objects " << objects.size() << std::endl;
    for (size_t o=0; o<objects.size(); o++)
    {
        inlier_size_res.push_back(checkPlanarity(objects[o], cloud, plane_dist_thr));
    }
    for (auto i = obj_ind_to_del.rbegin(); i != obj_ind_to_del.rend(); ++ i)
    {
        objects.erase(objects.begin() + *i);
    }
    std::cout << "Number of objects after deleting planar objects " << objects.size() << std::endl;
    return inlier_size_res;
}

void ChangeDetection::filterPlanarAndColor(std::vector<PlaneWithObjInd>& objects, pcl::PointCloud<PointNormal>::Ptr cloud, std::string path, float _plane_dist_thr, int _nr_bins) {
    std::vector<int> obj_ind_to_del;

    std::cout << "Number of objects before filterting based on planarity and color " << objects.size() << std::endl;
    for (size_t o = 0; o < objects.size(); o++) {
        std::cout << "Check object number " << o << std::endl;
        int plane_inliers = checkPlanarity(objects[o], cloud, _plane_dist_thr);
        double corr = checkColorSimilarity(objects[o], cloud, _nr_bins);
        double planarity = (double)plane_inliers/objects[o].obj_indices.size();
        std::cout << "Correlation: " << corr << "; Planarity: " << planarity << std::endl;
        if ((0.25*corr + 0.75 * planarity) > 0.7 ) {
            //if ( (corr > 0.4) && (planarity >0.4) || (corr > 0.9) || (planarity > 0.9) ) {
            obj_ind_to_del.push_back(o);
            std::cout << "Deleted" << std::endl;
        }
    }
    for (auto i = obj_ind_to_del.rbegin(); i != obj_ind_to_del.rend(); ++ i)
    {
        objects.erase(objects.begin() + *i);
    }
    std::cout << "Number of objects after filtering based on planarity and color " << objects.size() << std::endl;
}


//combine the orientation from the reconstruction with the more accurate normals computed with the points
void ChangeDetection::refineNormals(pcl::PointCloud<PointNormal>::Ptr object_cloud) {
    //compute normals
    pcl::PointCloud<pcl::Normal>::Ptr object_normals(new pcl::PointCloud<pcl::Normal>);
    pcl::NormalEstimation<PointNormal, pcl::Normal>::Ptr ne(new pcl::NormalEstimation<PointNormal, pcl::Normal>);
    ne->setRadiusSearch(0.02);
    ne->setInputCloud(object_cloud);
    ne->compute(*object_normals);

    //check orientation
    for (size_t p = 0; p < object_cloud->size(); p++) {
        const auto normal_pre = object_cloud->at(p).getNormalVector4fMap();
        const auto normal_comp = object_normals->at(p).getNormalVector4fMap();
        float normals_dotp = normal_comp.dot(normal_pre);
        if (normals_dotp < 0) { //negative dot product = angle between the two normals is between 90 and 270 deg
            object_normals->at(p).normal_x *= -1;
            object_normals->at(p).normal_y *= -1;
            object_normals->at(p).normal_z *= -1;
        }
    }

    pcl::concatenateFields(*object_cloud, *object_normals, *object_cloud); //data in the second cloud will overwrite the data in the first
}

//removes small clusters from input cloud and returns all valid clusters
std::vector<pcl::PointIndices> ChangeDetection::cleanResult(pcl::PointCloud<PointNormal>::Ptr cloud, float cluster_thr) {
    //clean up small things
    std::vector<pcl::PointIndices> cluster_indices;
    if (cloud->empty()) {
        return cluster_indices;
    }
    //check if cloud only consists of nans
    std::vector<int> nan_ind;
    pcl::PointCloud<PointNormal>::Ptr no_nans_cloud(new pcl::PointCloud<PointNormal>);
    cloud->is_dense = false;
    pcl::removeNaNFromPointCloud(*cloud, *no_nans_cloud, nan_ind);
    if (no_nans_cloud->size() == 0) {
        return cluster_indices;
    }
    if (no_nans_cloud->size() == cloud->size())
        cloud->is_dense = true;


    pcl::search::KdTree<PointNormal>::Ptr tree (new pcl::search::KdTree<PointNormal>);
    tree->setInputCloud (cloud);

    pcl::EuclideanClusterExtraction<PointNormal> ec;
    ec.setClusterTolerance (cluster_thr);
    ec.setMinClusterSize (15);
    ec.setMaxClusterSize (std::numeric_limits<int>::max());
    ec.setSearchMethod(tree);
    ec.setInputCloud (cloud);
    ec.extract (cluster_indices);
    pcl::ExtractIndices<PointNormal> extract;
    extract.setInputCloud (cloud);
    pcl::PointIndices::Ptr c_ind (new pcl::PointIndices);
    for (size_t i = 0; i < cluster_indices.size(); i++) {
        c_ind->indices.insert(std::end(c_ind->indices), std::begin(cluster_indices.at(i).indices), std::end(cluster_indices.at(i).indices));
    }
    extract.setIndices (c_ind);
    extract.setKeepOrganized(true);
    extract.setNegative (false);
    extract.filter (*cloud);

    return cluster_indices;
}
